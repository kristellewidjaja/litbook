#include "litbook/pipeline/messages.hpp"

#include "litbook/concurrency/message_queue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace litbook::pipeline {
namespace {

static_assert(concurrency::QueueElement<Request>);
static_assert(concurrency::QueueElement<Reply>);
static_assert(concurrency::QueueElement<MarketEvent>);

std::vector<std::byte> bytes_of(std::size_t n) {
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>(i + 1);
    }
    return out;
}

TEST(PipelineMessagesTest, EachMessageFitsInOneCacheLine) {
    EXPECT_LE(sizeof(Request), 64U);
    EXPECT_LE(sizeof(Reply), 64U);
    EXPECT_LE(sizeof(MarketEvent), 64U);
    EXPECT_EQ(kMaxFrame, 47U);
}

TEST(PipelineMessagesTest, AFrameRequestCarriesTheBytesOwnerAndTime) {
    const auto frame = bytes_of(29);
    const Request r = make_frame_request(7, frame, 123);
    EXPECT_EQ(r.kind, RequestKind::Frame);
    EXPECT_EQ(r.owner, 7U);
    EXPECT_EQ(r.now, 123U);
    ASSERT_EQ(r.bytes().size(), 29U);
    EXPECT_TRUE(std::equal(frame.begin(), frame.end(), r.bytes().begin()));
}

TEST(PipelineMessagesTest, TheLongestFrameFitsAndLongerOnesAreCut) {
    const auto longest = bytes_of(kMaxFrame);
    EXPECT_EQ(make_frame_request(1, longest, 0).bytes().size(), kMaxFrame);
    EXPECT_EQ(make_message_reply(1, longest).bytes().size(), kMaxFrame);
    const auto too_long = bytes_of(kMaxFrame + 5);
    EXPECT_EQ(make_frame_request(1, too_long, 0).bytes().size(), kMaxFrame);
    EXPECT_EQ(make_message_reply(1, too_long).bytes().size(), kMaxFrame);
}

TEST(PipelineMessagesTest, ControlMessagesCarryNoBytes) {
    const Request cancel = make_cancel_all_request(3, 9);
    EXPECT_EQ(cancel.kind, RequestKind::CancelAll);
    EXPECT_EQ(cancel.owner, 3U);
    EXPECT_EQ(cancel.now, 9U);
    EXPECT_TRUE(cancel.bytes().empty());

    const Request stop = make_stop_request(5);
    EXPECT_EQ(stop.kind, RequestKind::Stop);
    EXPECT_TRUE(stop.bytes().empty());

    const Reply done = make_done_reply(4);
    EXPECT_EQ(done.kind, ReplyKind::RequestDone);
    EXPECT_EQ(done.owner, 4U);
    EXPECT_TRUE(done.bytes().empty());

    EXPECT_EQ(make_stopped_reply().kind, ReplyKind::Stopped);
}

TEST(PipelineMessagesTest, AMessageReplyCarriesTheOwnerAndBytes) {
    const auto frame = bytes_of(45);
    const Reply r = make_message_reply(2, frame);
    EXPECT_EQ(r.kind, ReplyKind::Message);
    EXPECT_EQ(r.owner, 2U);
    EXPECT_TRUE(std::equal(frame.begin(), frame.end(), r.bytes().begin(), r.bytes().end()));
}

}  // namespace
}  // namespace litbook::pipeline
