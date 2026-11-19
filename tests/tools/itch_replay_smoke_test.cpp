// End-to-end tests for litbook_itch_replay. Each test writes a tiny ITCH day and a sizes
// file to a temporary folder, runs the real executable on them, and checks what it prints.
// The day is a repeated story for one stock that uses every message kind the replay
// handles (add, execute, partial cancel, replace, delete) and ends with an empty book, so
// every mode should print RESULT OK at the end.

#include <gtest/gtest.h>

#include <sys/wait.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::uint16_t kLocate = 1;
constexpr int kStories = 50;

// Builds one ITCH message by writing fields at the byte offsets in the specification,
// big-endian like the wire format.
class Message {
public:
    Message(char type, std::size_t size) : bytes_(size, 0) {
        bytes_[0] = static_cast<unsigned char>(type);
        uint(1, 2, kLocate);  // stock locate; tracking number and timestamp stay 0
    }
    Message& uint(std::size_t offset, std::size_t width, std::uint64_t value) {
        for (std::size_t i = 0; i < width; ++i) {
            bytes_.at(offset + width - 1 - i) = static_cast<unsigned char>(value & 0xFFU);
            value >>= 8U;
        }
        return *this;
    }
    Message& text(std::size_t offset, std::string_view s) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            bytes_.at(offset + i) = static_cast<unsigned char>(s[i]);
        }
        return *this;
    }
    [[nodiscard]] const std::vector<unsigned char>& bytes() const { return bytes_; }

private:
    std::vector<unsigned char> bytes_;
};

Message add(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price) {
    Message m('A', 36);
    m.uint(11, 8, ref).text(19, std::string_view(&side, 1)).uint(20, 4, shares);
    m.text(24, "TEST    ").uint(32, 4, price);
    return m;
}
Message executed(std::uint64_t ref, std::uint32_t shares) {
    Message m('E', 31);
    m.uint(11, 8, ref).uint(19, 4, shares).uint(23, 8, ref);
    return m;
}
Message partial_cancel(std::uint64_t ref, std::uint32_t shares) {
    Message m('X', 23);
    m.uint(11, 8, ref).uint(19, 4, shares);
    return m;
}
Message replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares,
                std::uint32_t price) {
    Message m('U', 35);
    m.uint(11, 8, old_ref).uint(19, 8, new_ref).uint(27, 4, shares).uint(31, 4, price);
    return m;
}
Message delete_order(std::uint64_t ref) {
    Message m('D', 19);
    m.uint(11, 8, ref);
    return m;
}

// Writes the day in BinaryFILE framing, a 2-byte length before each message and a zero
// length at the end.
void write_day(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    const auto put = [&out](const Message& m) {
        const std::size_t n = m.bytes().size();
        out.put(static_cast<char>(n >> 8U)).put(static_cast<char>(n & 0xFFU));
        for (const unsigned char b : m.bytes()) {
            out.put(static_cast<char>(b));
        }
    };
    for (std::uint64_t k = 0; k < kStories; ++k) {
        const std::uint64_t buy = (3 * k) + 1;
        const std::uint64_t sell = buy + 1;
        const std::uint64_t moved = buy + 2;
        put(add(buy, 'B', 100, 10'000));
        put(add(sell, 'S', 50, 10'100));
        put(executed(buy, 30));
        put(partial_cancel(sell, 20));
        put(replace(buy, moved, 70, 10'050));
        put(delete_order(sell));
        put(delete_order(moved));
    }
    out.put(0).put(0);
}

struct RunResult {
    int exit_code;
    std::string output;
};

class ItchReplaySmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dir_ = fs::temp_directory_path() / ("litbook_replay_smoke_" + name);
        fs::create_directories(dir_);
        write_day(dir_ / "day.itch");
        // locate, symbol, peak resting orders, lowest and highest price, whole cents
        std::ofstream(dir_ / "sizes.csv") << kLocate << ",TEST,4,10000,10100,0\n";
    }
    void TearDown() override { fs::remove_all(dir_); }

    [[nodiscard]] RunResult run(const std::string& args) const {
        const std::string command = std::string("\"") + LITBOOK_ITCH_REPLAY + "\" \"" +
                                    (dir_ / "sizes.csv").string() + "\" " + args + " < \"" +
                                    (dir_ / "day.itch").string() + "\" 2>&1";
        std::FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr) {
            return {.exit_code = -1, .output = {}};
        }
        std::string output;
        std::array<char, 512> chunk{};
        std::size_t n = 0;
        while ((n = std::fread(chunk.data(), 1, chunk.size(), pipe)) > 0) {
            output.append(chunk.data(), n);
        }
        const int status = pclose(pipe);
        return {.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1, .output = output};
    }

private:
    fs::path dir_;
};

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

TEST_F(ItchReplaySmokeTest, EveryModeReplaysTheDayCleanly) {
    for (const char* mode : {"map", "sorted", "fast", "null", "verify"}) {
        SCOPED_TRACE(mode);
        const RunResult r = run(mode);
        EXPECT_EQ(r.exit_code, 0) << r.output;
        EXPECT_TRUE(contains(r.output, "RESULT: OK")) << r.output;
        EXPECT_TRUE(contains(r.output, "messages sent to a book: 350")) << r.output;
    }
}

TEST_F(ItchReplaySmokeTest, VerifyModeFindsNoDifferences) {
    const RunResult r = run("verify");
    EXPECT_TRUE(contains(r.output, "messages with different events: 0")) << r.output;
}

TEST_F(ItchReplaySmokeTest, SamplesOneMessageInN) {
    // 350 book messages timed one in 5 gives exactly 70 samples.
    const RunResult r = run("sorted --sample-every 5");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_TRUE(contains(r.output, "timed one message in 5: 70 samples")) << r.output;
    EXPECT_TRUE(contains(r.output, "timer floor")) << r.output;
}

TEST_F(ItchReplaySmokeTest, SampleEveryOneTimesEveryMessage) {
    const RunResult r = run("map --sample-every 1");
    EXPECT_TRUE(contains(r.output, "timed one message in 1: 350 samples")) << r.output;
}

TEST_F(ItchReplaySmokeTest, UntimedRunReadsNoTimer) {
    const RunResult r = run("sorted --untimed");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_TRUE(contains(r.output, "untimed: no per-message timer reads")) << r.output;
    EXPECT_FALSE(contains(r.output, "timed one message in")) << r.output;
}

TEST_F(ItchReplaySmokeTest, LadderStocksFlagKeepsAStockThatFits) {
    const RunResult r = run("map --ladder-stocks");
    EXPECT_TRUE(contains(r.output, "(ladder stocks only): 1 books")) << r.output;
    EXPECT_TRUE(contains(r.output, "RESULT: OK")) << r.output;
}

TEST_F(ItchReplaySmokeTest, RejectsAZeroSampleInterval) {
    const RunResult r = run("sorted --sample-every 0");
    EXPECT_EQ(r.exit_code, 2) << r.output;
}

TEST_F(ItchReplaySmokeTest, RejectsAMissingSampleInterval) {
    const RunResult r = run("sorted --sample-every");
    EXPECT_EQ(r.exit_code, 2) << r.output;
}

TEST_F(ItchReplaySmokeTest, RejectsAnUnknownMode) {
    const RunResult r = run("nonsense");
    EXPECT_EQ(r.exit_code, 2) << r.output;
}

}  // namespace
