#pragma once

// Writes a journal directory by hand for tests: the setup records, then a random request
// stream, split into segments wherever the test rotates. Everything written is also
// applied to a reference order entry layer, so a test knows the true state and every
// reply after any record.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/journal/snapshot_format.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "journal/replay_test_support.hpp"

namespace litbook::journal::testing {

class JournalBuilder {
public:
    using Book = SortedOrderBook;

    explicit JournalBuilder(const std::string& directory) : paths_(directory) {
        JournalWriterConfig config;
        config.buffer_capacity = std::size_t{1} << 20;
        writer_ = std::make_unique<JournalWriter>(JournalWriter::create(paths_.segment(1), config));
        for (const JournalRecord& record : replay_setup_records()) {
            EXPECT_EQ(append_setup_record(*writer_, 1, record), JournalStatus::Ok);
        }
        EXPECT_EQ(writer_->flush(), JournalStatus::Ok);
        set_up_exchange(reference_.orders);
    }

    // Appends `count` requests of the stream to the open segment.
    void append(std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const pipeline::Request& request = stream_.at(next_request_);
            ++next_request_;
            EXPECT_EQ(writer_->append(request), JournalStatus::Ok);
            reference_.now = request.now;
            apply_request(reference_.orders, request);
        }
        EXPECT_EQ(writer_->flush(), JournalStatus::Ok);
    }

    // Closes the open segment and returns {first, next}, as the journal thread reports.
    std::pair<Sequence, Sequence> rotate() {
        const Sequence first = writer_->file_first_sequence();
        const Sequence next = writer_->next_sequence();
        EXPECT_EQ(writer_->rotate(paths_.segment(next), paths_.directory()), JournalStatus::Ok);
        return {first, next};
    }

    // A snapshot of the reference as of the last record written.
    [[nodiscard]] Snapshot snapshot() const {
        Snapshot s;
        s.as_of = writer_->next_sequence() - 1;
        s.as_of_time = reference_.now;
        for (JournalRecord record : replay_setup_records()) {
            record.sequence = 0;
            record.now = 0;
            s.setup.push_back(record);
        }
        s.replies = reply_counts();
        reference_.orders.capture(s.state);
        return s;
    }

    // Every reply the reference produced so far, in order.
    [[nodiscard]] const std::vector<RecordedReply>& replies() const noexcept {
        return reference_.replies.replies;
    }

    // Each account's reply count so far (entry 0 unused).
    [[nodiscard]] std::vector<std::uint64_t> reply_counts() const {
        std::vector<std::uint64_t> counts(kReplayOwners + 1, 0);
        for (const RecordedReply& reply : reference_.replies.replies) {
            ++counts.at(reply.owner);
        }
        return counts;
    }

    [[nodiscard]] const protocol::OrderEntry<Book>& reference() const noexcept {
        return reference_.orders;
    }
    [[nodiscard]] Sequence next_sequence() const noexcept { return writer_->next_sequence(); }
    [[nodiscard]] PathBuffer& paths() noexcept { return paths_; }

private:
    struct Reference {
        RecordingReplies replies;
        pipeline::Timestamp now = 0;
        protocol::OrderEntry<Book> orders{replies, replay_order_entry_config()};
    };

    PathBuffer paths_;
    std::unique_ptr<JournalWriter> writer_;
    std::vector<pipeline::Request> stream_ = replay_stream(5, 5000);
    std::size_t next_request_ = 0;
    Reference reference_;
};

// The bytes of a snapshot file.
inline std::vector<std::byte> encoded(const Snapshot& s) {
    std::vector<std::byte> out(snapshot_size(s));
    EXPECT_EQ(encode_snapshot(s, out), out.size());
    return out;
}

}  // namespace litbook::journal::testing
