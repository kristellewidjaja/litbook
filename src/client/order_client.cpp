#include "litbook/client/order_client.hpp"

#include "litbook/protocol/codec.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <system_error>

namespace litbook::client {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

// Milliseconds left until the deadline, rounded up so a wait never ends early, and never
// below 0.
int remaining_ms(Clock::time_point deadline) noexcept {
    const auto left = deadline - Clock::now();
    if (left <= Clock::duration::zero()) {
        return 0;
    }
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return static_cast<int>(std::min<std::chrono::milliseconds::rep>(ms, 1'000'000));
}

// True if the codec can put this message on the wire. It refuses a price below 0 or above
// what OUCH can carry, and the session would then queue an empty packet, so the client
// checks first.
bool encodable(const protocol::InboundMessage& message) noexcept {
    std::array<std::byte, protocol::kMaxMessageLength> frame{};
    return protocol::encode(message, frame) != 0;
}

}  // namespace

const char* to_string(Outcome outcome) noexcept {
    switch (outcome) {
        case Outcome::Answered:
            return "answered";
        case Outcome::Gone:
            return "gone";
        case Outcome::NotOpen:
            return "not open";
        case Outcome::TimedOut:
            return "timed out";
        case Outcome::Closed:
            return "closed";
        case Outcome::Refused:
            return "refused";
    }
    return "unknown";
}

OrderClient::OrderClient(const OrderClientConfig& config)
    : username_(config.username),
      timeout_ms_(config.timeout_ms),
      socket_(gateway::connect_tcp(config.address, config.port)),
      session_(config.username, config.password, config.session),
      next_ref_(config.first_ref) {
    if (config.timeout_ms < 0) {
        throw std::invalid_argument("timeout_ms must not be negative");
    }
    if (config.first_ref == 0) {
        throw std::invalid_argument("first_ref must be at least 1");
    }
    if (!gateway::set_nonblocking(socket_.get())) {
        throw std::system_error(errno, std::generic_category(), "set_nonblocking");
    }
    session_.begin(now_ns());
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
    while (session_.state() == loadgen::SessionState::LoggingIn) {
        const int left = remaining_ms(deadline);
        if (left == 0) {
            throw std::runtime_error("login timed out");
        }
        step(left, nullptr, nullptr);
    }
    if (!session_.active()) {
        throw std::runtime_error(std::string("login failed: ") +
                                 loadgen::to_string(session_.failure()));
    }
}

OrderClient::~OrderClient() {
    // A polite logout is best effort. The socket closes either way.
    if (session_.active()) {
        session_.logout(now_ns());
        flush();
    }
}

void OrderClient::logout() {
    if (session_.finished()) {
        return;
    }
    session_.logout(now_ns());
    flush();
    // The server closes the connection once it has read the Logout Request. Waiting for
    // that means the account is free when logout returns, so a new connection of the same
    // account cannot reach the server first and be refused. Anything still arriving is
    // dropped, since the session has ended.
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
    while (socket_.valid()) {
        const int left = remaining_ms(deadline);
        if (left == 0) {
            break;
        }
        if (!session_.output().empty()) {
            flush();
        }
        pollfd p{.fd = socket_.get(), .events = POLLIN, .revents = 0};
        if (::poll(&p, 1, left) <= 0) {
            continue;
        }
        std::array<std::byte, 4096> scratch{};
        const ssize_t n = ::recv(socket_.get(), scratch.data(), scratch.size(), 0);
        if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            break;
        }
    }
    socket_.reset();
}

Response OrderClient::enter(const NewOrder& order) {
    const protocol::UserRefNum ref = next_ref_;
    const protocol::InboundMessage message = protocol::EnterOrder{.user_ref = ref,
                                                                  .side = order.side,
                                                                  .qty = order.qty,
                                                                  .symbol = order.symbol,
                                                                  .price = order.price,
                                                                  .tif = order.tif,
                                                                  .type = order.type,
                                                                  .post_only = order.post_only};
    return send_and_wait(message, Awaited{.kind = RequestKind::Enter, .ref = ref, .orig = 0});
}

Response OrderClient::replace(protocol::UserRefNum ref, Qty qty, Price price) {
    if (session_.finished()) {
        return Response{.outcome = Outcome::Closed, .user_ref = 0, .reply = std::nullopt};
    }
    if (tracker_.find(ref) == nullptr) {
        return Response{.outcome = Outcome::NotOpen, .user_ref = 0, .reply = std::nullopt};
    }
    const protocol::UserRefNum new_ref = next_ref_;
    const protocol::InboundMessage message = protocol::ReplaceOrder{
        .orig_user_ref = ref, .user_ref = new_ref, .qty = qty, .price = price};
    return send_and_wait(message,
                         Awaited{.kind = RequestKind::Replace, .ref = new_ref, .orig = ref});
}

Response OrderClient::cancel(protocol::UserRefNum ref) {
    if (session_.finished()) {
        return Response{.outcome = Outcome::Closed, .user_ref = 0, .reply = std::nullopt};
    }
    if (tracker_.find(ref) == nullptr) {
        return Response{.outcome = Outcome::NotOpen, .user_ref = 0, .reply = std::nullopt};
    }
    const protocol::InboundMessage message = protocol::CancelOrder{.user_ref = ref, .qty = 0};
    return send_and_wait(message, Awaited{.kind = RequestKind::Cancel, .ref = ref, .orig = 0});
}

Response OrderClient::send_and_wait(const protocol::InboundMessage& message,
                                    const Awaited& request) {
    if (session_.finished()) {
        return Response{.outcome = Outcome::Closed, .user_ref = 0, .reply = std::nullopt};
    }
    if (!encodable(message)) {
        return Response{.outcome = Outcome::Refused, .user_ref = 0, .reply = std::nullopt};
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
    // The output buffer only fills if the gateway stops reading, so wait for room.
    while (!session_.can_send()) {
        if (session_.finished()) {
            return Response{.outcome = Outcome::Closed, .user_ref = 0, .reply = std::nullopt};
        }
        const int left = remaining_ms(deadline);
        if (left == 0) {
            return Response{.outcome = Outcome::TimedOut, .user_ref = 0, .reply = std::nullopt};
        }
        step(left, nullptr, nullptr);
    }
    (void)session_.send(message, now_ns());
    // Enter and Replace use up the next number. A Cancel names an old one.
    if (request.kind != RequestKind::Cancel) {
        ++next_ref_;
    }
    const protocol::UserRefNum used = request.ref;
    std::optional<protocol::OutboundMessage> answer;
    for (;;) {
        if (answer) {
            return Response{.outcome = Outcome::Answered, .user_ref = used, .reply = answer};
        }
        if (gone(request, tracker_)) {
            return Response{.outcome = Outcome::Gone, .user_ref = used, .reply = std::nullopt};
        }
        if (session_.finished()) {
            return Response{.outcome = Outcome::Closed, .user_ref = used, .reply = std::nullopt};
        }
        const int left = remaining_ms(deadline);
        if (left == 0) {
            return Response{.outcome = Outcome::TimedOut, .user_ref = used, .reply = std::nullopt};
        }
        step(left, &request, &answer);
    }
}

std::vector<protocol::OutboundMessage> OrderClient::events(int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    // Always read once, so a caller polling with 0 still sees what has arrived.
    step(0, nullptr, nullptr);
    while (events_.empty() && !session_.finished()) {
        const int left = remaining_ms(deadline);
        if (left == 0) {
            break;
        }
        step(left, nullptr, nullptr);
    }
    std::vector<protocol::OutboundMessage> out(events_.begin(), events_.end());
    events_.clear();
    return out;
}

void OrderClient::flush() noexcept {
    if (!socket_.valid()) {
        return;
    }
    gateway::ByteBuffer& out = session_.output();
    while (!out.empty()) {
        const auto bytes = out.readable();
        const ssize_t n = ::send(socket_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out.consume(static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            session_.fail(loadgen::SessionFailure::Closed);
            socket_.reset();
            return;
        }
    }
}

void OrderClient::step(int timeout_ms, const Awaited* awaited,
                       std::optional<protocol::OutboundMessage>* answer) {
    if (!socket_.valid()) {
        session_.fail(loadgen::SessionFailure::Closed);
        return;
    }
    session_.tick(now_ns());
    flush();
    if (!socket_.valid()) {
        return;
    }
    pollfd p{.fd = socket_.get(), .events = POLLIN, .revents = 0};
    if (!session_.output().empty()) {
        p.events = static_cast<short>(p.events | POLLOUT);
    }
    // Wake at least once a heartbeat interval, so a long wait still sends heartbeats.
    const int rc = ::poll(&p, 1, std::min(timeout_ms, 1000));
    if (rc <= 0) {
        return;
    }
    if ((p.revents & POLLOUT) != 0) {
        flush();
    }
    if ((p.revents & (POLLIN | POLLHUP | POLLERR)) == 0 || !socket_.valid()) {
        return;
    }
    gateway::ByteBuffer& in = session_.input();
    auto on_reply = [&](const protocol::OutboundMessage& reply) {
        tracker_.on_reply(reply);
        if (awaited != nullptr && answer != nullptr && !*answer && answers(*awaited, reply)) {
            *answer = reply;
        } else {
            events_.push_back(reply);
        }
    };
    bool closed = false;
    for (;;) {
        if (!in.make_room(1)) {
            session_.process_input(on_reply);
            if (session_.finished() || !in.make_room(1)) {
                session_.fail(loadgen::SessionFailure::InputFull);
                break;
            }
        }
        const auto space = in.writable();
        const ssize_t n = ::recv(socket_.get(), space.data(), space.size(), 0);
        if (n > 0) {
            in.commit(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        closed = true;  // 0 is an orderly close, anything else an error.
        break;
    }
    session_.process_input(on_reply);
    if (closed) {
        session_.fail(loadgen::SessionFailure::Closed);  // Does nothing after End of Session.
    }
    if (session_.finished()) {
        socket_.reset();
    }
}

}  // namespace litbook::client
