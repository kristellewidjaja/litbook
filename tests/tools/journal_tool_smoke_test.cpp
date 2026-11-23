// End-to-end tests of litbook_journal. Each test writes a journal with the library, runs
// the program on it and reads what it prints.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/crc32c.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/journal/snapshot_store.hpp"
#include "litbook/protocol/byte_order.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "journal/journal_builder.hpp"
#include "journal/replay_test_support.hpp"
#include "support/temp_dir.hpp"
#include "tools/child_process.hpp"

namespace litbook::journal {
namespace {

using litbook::testing::ChildProcess;
using litbook::testing::TempDir;

constexpr int kRequests = 800;

// A C file that closes itself.
using File = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

// The reply digest exactly as litbook_journal describes it in its usage comment.
class DigestReplies final : public protocol::OutboundSink {
public:
    void send(OwnerId owner, std::span<const std::byte> frame) override {
        std::array<std::byte, 4> o{};
        protocol::store_le(std::span<std::byte>(o), 0, owner);
        crc = crc32c_extend(crc, o);
        crc = crc32c_extend(crc, frame);
        ++count;
        ++per_owner[owner];
    }
    std::uint32_t crc = 0;
    std::uint64_t count = 0;
    std::map<OwnerId, std::uint64_t> per_owner;
};

// What one run of the program printed.
struct ToolRun {
    std::vector<std::string> lines;
    int exit_code = -1;

    [[nodiscard]] bool has(const std::string& line) const {
        for (const std::string& l : lines) {
            if (l == line) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool has_prefix(const std::string& prefix) const {
        for (const std::string& l : lines) {
            if (l.starts_with(prefix)) {
                return true;
            }
        }
        return false;
    }
};

ToolRun run_tool(std::vector<std::string> args) {
    ChildProcess child(LITBOOK_JOURNAL, std::move(args));
    ToolRun run;
    for (std::string line = child.line(); !line.empty(); line = child.line()) {
        run.lines.push_back(line);
    }
    run.exit_code = child.wait_for_exit(std::chrono::seconds(30));
    return run;
}

class JournalToolTest : public ::testing::Test {
protected:
    // Writes a journal of the replay test exchange and runs the same requests through an
    // order entry layer here, so the tests know what the program should print.
    void SetUp() override {
        JournalWriter writer = JournalWriter::create(path_, JournalWriterConfig{});
        ASSERT_EQ(writer.append_session_start(1, testing::replay_order_entry_config()),
                  JournalStatus::Ok);
        SymbolId id = 0;
        for (const char* name : testing::kReplayStocks) {
            ASSERT_EQ(writer.append_add_symbol(
                          1, SymbolListing{.id = id,
                                           .ticker = litbook::testing::unwrap(make_ticker(name)),
                                           .config = testing::replay_book_config()}),
                      JournalStatus::Ok);
            ++id;
        }
        ASSERT_EQ(writer.append_risk_limits(1, testing::kLimitedOwner, testing::kLimitedRisk),
                  JournalStatus::Ok);
        testing::set_up_exchange(orders_);
        for (const pipeline::Request& r : testing::replay_stream(5, kRequests)) {
            ASSERT_EQ(writer.append(r), JournalStatus::Ok);
            testing::apply_request(orders_, r);
        }
        ASSERT_EQ(writer.append(pipeline::make_stop_request(9'000'000'000)), JournalStatus::Ok);
        ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    }

    TempDir dir_;
    std::string path_ = dir_.file("day.journal");
    DigestReplies replies_;
    protocol::OrderEntry<SortedOrderBook> orders_{replies_, testing::replay_order_entry_config()};
};

TEST_F(JournalToolTest, VerifyReportsACleanJournal) {
    const ToolRun run = run_tool({"verify", path_});
    EXPECT_EQ(run.exit_code, 0);
    EXPECT_TRUE(run.has("journal " + path_));
    EXPECT_TRUE(run.has("header ok, first sequence 1"));
    EXPECT_TRUE(run.has_prefix("records " + std::to_string(kRequests + 6) + " (setup 5, frames "));
    EXPECT_TRUE(run.has("sequences 1 to " + std::to_string(kRequests + 6)));
    EXPECT_TRUE(run.has("tail clean"));
    EXPECT_TRUE(run.has_prefix("crc32c "));
}

TEST_F(JournalToolTest, VerifyReportsATornTailWithExitCodeOne) {
    std::filesystem::resize_file(path_, std::filesystem::file_size(path_) - 2);
    const ToolRun run = run_tool({"verify", path_});
    EXPECT_EQ(run.exit_code, 1);
    EXPECT_TRUE(run.has("tail torn (Incomplete), 26 bytes after the last good record"));
}

TEST_F(JournalToolTest, VerifyReportsCorruptionWithExitCodeThree) {
    {
        const File f(std::fopen(path_.c_str(), "r+b"), &std::fclose);
        ASSERT_NE(f, nullptr);
        // Flip every bit of one byte in the middle, so it surely changes.
        ASSERT_EQ(std::fseek(f.get(), 500, SEEK_SET), 0);
        const int old_byte = std::fgetc(f.get());
        ASSERT_NE(old_byte, EOF);
        ASSERT_EQ(std::fseek(f.get(), 500, SEEK_SET), 0);
        ASSERT_NE(std::fputc(old_byte ^ 0xFF, f.get()), EOF);
    }
    const ToolRun run = run_tool({"verify", path_});
    EXPECT_EQ(run.exit_code, 3);
    EXPECT_TRUE(run.has_prefix("tail corrupt, bad record at offset "));
    const ToolRun replay = run_tool({"replay", path_});
    EXPECT_EQ(replay.exit_code, 3);
    EXPECT_TRUE(replay.has("replay failed: JournalCorrupt after 0 records"));
}

TEST_F(JournalToolTest, DumpPrintsOneLinePerRecord) {
    const ToolRun run = run_tool({"dump", path_, "--limit", "3"});
    EXPECT_EQ(run.exit_code, 0);
    EXPECT_TRUE(run.has("1 t=1 SessionStart max owners 4, max live orders 4096, max stocks 3"));
    EXPECT_TRUE(run.has_prefix("2 t=1 AddSymbol id 0 AAPL tick 100 lot 1 band "));
    EXPECT_TRUE(run.has_prefix("3 t=1 AddSymbol id 1 MSFT"));
    EXPECT_TRUE(run.has("... " + std::to_string(kRequests + 3) + " more records"));
    const ToolRun all = run_tool({"dump", path_});
    EXPECT_EQ(all.exit_code, 0);
    EXPECT_TRUE(all.has("5 t=1 RiskLimits owner 3 max order qty 50 max open orders 6"));
    EXPECT_TRUE(all.has(std::to_string(kRequests + 6) + " t=9000000000 Stop"));
}

TEST_F(JournalToolTest, ReplayPrintsWhatTheExchangeDid) {
    const ToolRun run = run_tool({"replay", path_});
    EXPECT_EQ(run.exit_code, 0);
    std::array<char, 96> line{};
    std::snprintf(line.data(), line.size(), "replies %llu crc32c %08x",
                  static_cast<unsigned long long>(replies_.count), replies_.crc);
    EXPECT_TRUE(run.has(line.data())) << line.data();
    EXPECT_TRUE(run.has("live orders " + std::to_string(orders_.live_orders()) + ", trades " +
                        std::to_string(orders_.engine().last_match_id())));
    for (const auto& [owner, sent] : replies_.per_owner) {
        if (owner >= 1 && owner <= testing::kReplayOwners) {
            EXPECT_TRUE(run.has("account " + std::to_string(owner) + " next sequence " +
                                std::to_string(sent + 1)));
        }
    }
    EXPECT_TRUE(run.has_prefix("market events "));
    EXPECT_TRUE(run.has("tail clean"));
    // The same file replayed twice prints the same thing.
    EXPECT_EQ(run_tool({"replay", path_}).lines, run.lines);
}

TEST_F(JournalToolTest, ReplayOfATornJournalStopsAtTheLastGoodRecord) {
    std::filesystem::resize_file(path_, std::filesystem::file_size(path_) - 2);
    const ToolRun run = run_tool({"replay", path_});
    EXPECT_EQ(run.exit_code, 1);
    EXPECT_TRUE(run.has("tail torn"));
    EXPECT_TRUE(run.has_prefix("replayed " + std::to_string(kRequests + 5) + " records"));
}

TEST_F(JournalToolTest, BadArgumentsAndUnreadableFilesExitWithTwo) {
    EXPECT_EQ(run_tool({}).exit_code, 2);
    EXPECT_EQ(run_tool({"verify"}).exit_code, 2);
    EXPECT_EQ(run_tool({"frobnicate", path_}).exit_code, 2);
    EXPECT_EQ(run_tool({"dump", path_, "--limit", "x"}).exit_code, 2);
    EXPECT_EQ(run_tool({"verify", path_, "extra"}).exit_code, 2);
    EXPECT_EQ(run_tool({"verify", dir_.file("missing")}).exit_code, 2);
    // A file that is not a journal at all.
    {
        const File f(std::fopen(dir_.file("text").c_str(), "wb"), &std::fclose);
        ASSERT_NE(f, nullptr);
        std::fputs("this is not a journal, just some text that is long enough\n", f.get());
    }
    const ToolRun run = run_tool({"verify", dir_.file("text")});
    EXPECT_EQ(run.exit_code, 3);
    EXPECT_TRUE(run.has("header BadMagic"));
    EXPECT_EQ(run_tool({"replay", dir_.file("text")}).exit_code, 3);
}

// The directory commands. Each test writes a journal directory with segments (and
// sometimes snapshots) through the builder, which also knows the true state.
class JournalToolDirectoryTest : public ::testing::Test {
protected:
    // 200 records, a snapshot, then two more segments of 200 and 100.
    void write_three_segments_and_a_snapshot() {
        builder_.append(200);
        write_snapshot();
        builder_.rotate();
        builder_.append(200);
        builder_.rotate();
        builder_.append(100);
    }

    void write_snapshot() {
        const Snapshot s = builder_.snapshot();
        ASSERT_EQ(write_snapshot_file(builder_.paths(), s.as_of, testing::encoded(s), false), 0);
        snapshot_as_of_ = s.as_of;
    }

    [[nodiscard]] std::string segment_name(Sequence first) const {
        return segment_file_name(first);
    }

    // The lines replay prints for each account's next sequence number.
    void expect_next_sequences(const ToolRun& run) const {
        const std::vector<std::uint64_t> counts = builder_.reply_counts();
        for (std::size_t owner = 1; owner < counts.size(); ++owner) {
            if (counts[owner] > 0) {
                EXPECT_TRUE(run.has("account " + std::to_string(owner) + " next sequence " +
                                    std::to_string(counts[owner] + 1)))
                    << "account " << owner;
            }
        }
        EXPECT_TRUE(run.has_prefix("live orders " +
                                   std::to_string(builder_.reference().live_orders()) + ", "));
    }

    TempDir dir_;
    testing::JournalBuilder builder_{dir_.path()};
    Sequence snapshot_as_of_ = 0;
};

TEST_F(JournalToolDirectoryTest, VerifyListsTheSegmentsTheSnapshotsAndWhereRecoveryStarts) {
    write_three_segments_and_a_snapshot();
    const ToolRun run = run_tool({"verify", dir_.path()});
    EXPECT_EQ(run.exit_code, 0);
    EXPECT_TRUE(run.has("journal directory " + dir_.path()));
    EXPECT_TRUE(run.has_prefix("segment " + segment_name(1) + ": first sequence 1, records "));
    const std::size_t setup = testing::replay_setup_records().size();
    const Sequence second = setup + 200 + 1;
    EXPECT_TRUE(run.has("segment " + segment_name(second) + ": first sequence " +
                        std::to_string(second) + ", records 200, tail clean"));
    EXPECT_TRUE(run.has("chain ok, records " + std::to_string(setup + 500) + ", next sequence " +
                        std::to_string(builder_.next_sequence())));
    EXPECT_TRUE(run.has("snapshot " + snapshot_file_name(snapshot_as_of_) + ": as of " +
                        std::to_string(snapshot_as_of_) + ", ok"));
    EXPECT_TRUE(
        run.has("recovery starts from the snapshot as of " + std::to_string(snapshot_as_of_)));
}

TEST_F(JournalToolDirectoryTest, ReplayFromTheSnapshotAndFromTheStartAgree) {
    write_three_segments_and_a_snapshot();
    const ToolRun from_snapshot = run_tool({"replay", dir_.path()});
    EXPECT_EQ(from_snapshot.exit_code, 0);
    EXPECT_TRUE(from_snapshot.has("start snapshot as of " + std::to_string(snapshot_as_of_)));
    EXPECT_TRUE(from_snapshot.has("skipped " + std::to_string(snapshot_as_of_) +
                                  " records the snapshot already holds"));
    expect_next_sequences(from_snapshot);

    const ToolRun from_start = run_tool({"replay", dir_.path(), "--from-start"});
    EXPECT_EQ(from_start.exit_code, 0);
    EXPECT_TRUE(from_start.has("start record 1"));
    expect_next_sequences(from_start);
    // From the start, the digest covers every reply the reference produced.
    DigestReplies digest;
    for (const testing::RecordedReply& reply : builder_.replies()) {
        digest.send(reply.owner, reply.bytes);
    }
    std::array<char, 96> line{};
    std::snprintf(line.data(), line.size(), "replies %llu crc32c %08x",
                  static_cast<unsigned long long>(digest.count), digest.crc);
    EXPECT_TRUE(from_start.has(line.data())) << line.data();
}

TEST_F(JournalToolDirectoryTest, DumpWalksEverySegmentInOrder) {
    write_three_segments_and_a_snapshot();
    const ToolRun run = run_tool({"dump", dir_.path()});
    EXPECT_EQ(run.exit_code, 0);
    EXPECT_TRUE(run.has("segment " + segment_name(1)));
    EXPECT_TRUE(run.has_prefix("1 t=1 SessionStart "));
    EXPECT_TRUE(run.has_prefix(std::to_string(builder_.next_sequence() - 1) + " t="));
    const ToolRun limited = run_tool({"dump", dir_.path(), "--limit", "2"});
    EXPECT_EQ(limited.exit_code, 0);
    const std::size_t setup = testing::replay_setup_records().size();
    EXPECT_TRUE(limited.has("... " + std::to_string(setup + 500 - 2) + " more records"));
}

TEST_F(JournalToolDirectoryTest, SnapshotWritesAFileThatEqualsTheTrueState) {
    builder_.append(300);
    builder_.rotate();
    builder_.append(100);
    const ToolRun run = run_tool({"snapshot", dir_.path()});
    EXPECT_EQ(run.exit_code, 0);
    const Sequence as_of = builder_.next_sequence() - 1;
    EXPECT_TRUE(run.has("start record 1"));
    EXPECT_TRUE(run.has_prefix("snapshot as of " + std::to_string(as_of) + " written to " +
                               snapshot_file_name(as_of) + " ("));
    const LoadedSnapshot written =
        read_snapshot_file(SnapshotFile{.as_of = as_of, .path = builder_.paths().snapshot(as_of)});
    ASSERT_EQ(written.status, SnapshotStatus::Ok);
    EXPECT_EQ(written.snapshot, builder_.snapshot());

    // A second run starts from that snapshot and writes the same bytes again.
    const std::vector<std::byte> first = testing::encoded(written.snapshot);
    const ToolRun again = run_tool({"snapshot", dir_.path()});
    EXPECT_EQ(again.exit_code, 0);
    EXPECT_TRUE(again.has("start snapshot as of " + std::to_string(as_of)));
    EXPECT_EQ(
        testing::encoded(read_snapshot_file(
                             SnapshotFile{.as_of = as_of, .path = builder_.paths().snapshot(as_of)})
                             .snapshot),
        first);

    // Once the snapshot covers the first segment, it can go, and only --from-start fails.
    std::filesystem::remove(builder_.paths().segment(1));
    EXPECT_EQ(run_tool({"verify", dir_.path()}).exit_code, 0);
    expect_next_sequences(run_tool({"replay", dir_.path()}));
    const ToolRun from_start = run_tool({"replay", dir_.path(), "--from-start"});
    EXPECT_EQ(from_start.exit_code, 3);
    EXPECT_TRUE(from_start.has_prefix("replay failed: the first segment starts at record "));
}

TEST_F(JournalToolDirectoryTest, ASnapshotOnTopOfAnEarlierOneAddsTheLaterReplies) {
    write_three_segments_and_a_snapshot();
    const ToolRun run = run_tool({"snapshot", dir_.path()});
    EXPECT_EQ(run.exit_code, 0);
    EXPECT_TRUE(run.has("start snapshot as of " + std::to_string(snapshot_as_of_)));
    const Sequence as_of = builder_.next_sequence() - 1;
    const LoadedSnapshot written =
        read_snapshot_file(SnapshotFile{.as_of = as_of, .path = builder_.paths().snapshot(as_of)});
    ASSERT_EQ(written.status, SnapshotStatus::Ok);
    // The reply counts are the earlier snapshot's plus the replies after it.
    EXPECT_EQ(written.snapshot.replies, builder_.reply_counts());
    EXPECT_EQ(written.snapshot, builder_.snapshot());
}

TEST_F(JournalToolDirectoryTest, ADamagedSnapshotIsRefusedAndRecoveryFallsBack) {
    builder_.append(100);
    write_snapshot();
    {
        const File f(std::fopen(builder_.paths().snapshot(snapshot_as_of_), "r+b"), &std::fclose);
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fseek(f.get(), 50, SEEK_SET), 0);
        ASSERT_NE(std::fputc(0xEE, f.get()), EOF);
    }
    const ToolRun run = run_tool({"verify", dir_.path()});
    EXPECT_EQ(run.exit_code, 0);
    EXPECT_TRUE(run.has("snapshot " + snapshot_file_name(snapshot_as_of_) + ": as of " +
                        std::to_string(snapshot_as_of_) + ", refused (BadChecksum)"));
    EXPECT_TRUE(run.has("recovery starts from record 1"));
    const ToolRun replay = run_tool({"replay", dir_.path()});
    EXPECT_TRUE(replay.has("start record 1"));
    expect_next_sequences(replay);
}

TEST_F(JournalToolDirectoryTest, AMissingSegmentIsRefusedWithExitCodeThree) {
    builder_.append(100);
    const auto [first, second] = builder_.rotate();
    builder_.append(100);
    builder_.rotate();
    builder_.append(100);
    std::filesystem::remove(builder_.paths().segment(second));
    const ToolRun run = run_tool({"verify", dir_.path()});
    EXPECT_EQ(run.exit_code, 3);
    EXPECT_TRUE(run.has_prefix("chain Gap at segment "));
    EXPECT_EQ(run_tool({"replay", dir_.path()}).exit_code, 3);
    EXPECT_EQ(run_tool({"snapshot", dir_.path()}).exit_code, 3);
    EXPECT_EQ(first, 1U);
}

TEST_F(JournalToolDirectoryTest, ATornRotationGivesExitCodeOne) {
    builder_.append(100);
    const auto [first, next] = builder_.rotate();
    // A crash while the new segment's header was written leaves only its start.
    std::filesystem::resize_file(builder_.paths().segment(next), 10);
    const ToolRun run = run_tool({"verify", dir_.path()});
    EXPECT_EQ(run.exit_code, 1);
    EXPECT_TRUE(run.has("segment " + segment_name(next) +
                        ": header cut short while it was created (a torn rotation)"));
    EXPECT_EQ(run_tool({"replay", dir_.path()}).exit_code, 1);
    EXPECT_EQ(first, 1U);
}

TEST(JournalToolDirectoryArguments, EmptyDirectoriesAndWrongPathsAreRefused) {
    const TempDir dir;
    const ToolRun empty = run_tool({"verify", dir.path()});
    EXPECT_EQ(empty.exit_code, 3);
    EXPECT_TRUE(empty.has("chain empty, the directory holds no segment"));
    EXPECT_EQ(run_tool({"replay", dir.path()}).exit_code, 3);
    // snapshot needs a directory, and --from-start only means something for one.
    const std::string file = dir.file("one.journal");
    {
        const JournalWriter writer = JournalWriter::create(file, JournalWriterConfig{});
    }
    EXPECT_EQ(run_tool({"snapshot", file}).exit_code, 2);
    EXPECT_EQ(run_tool({"replay", file, "--from-start"}).exit_code, 2);
    EXPECT_EQ(run_tool({"snapshot"}).exit_code, 2);
    EXPECT_EQ(run_tool({"replay", dir.path(), "--sideways"}).exit_code, 2);
}

}  // namespace
}  // namespace litbook::journal
