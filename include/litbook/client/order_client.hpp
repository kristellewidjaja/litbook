#pragma once

// A blocking order entry client for one account, written for bots and scripts rather than
// for speed. The constructor connects and logs in. enter, replace and cancel each send one
// request and wait until it is answered, or until it can no longer be answered, or until
// the timeout. Replies that answer nothing the client waits on (executions, the remainder
// of an IOC order, the cancel after a failed replace, system cancels) are kept in order
// and handed out by events().
//
// It reuses the socket-free SoupBinTCP rules of the load generator (ClientSession,
// D-044) and keeps the account's open orders in an OrderTracker. A Cancel or Replace that
// names an order which filled on its way gets no reply at all (D-069). The client does
// not need a timeout to notice that, because the executions that filled the order always
// arrive first, and the tracker then says the order is gone (see order_tracker.hpp).
//
// The client picks every UserRefNum itself, starting at first_ref and always rising, as
// OUCH requires for the whole day. A second connection of the same account later in the
// day must start above the last number the first one used (next_ref()). The exchange
// silently ignores an Enter whose number is too low, and the client then times out.
//
// Only reads and writes inside a call move the session along. The gateway closes a
// session that sent nothing for its idle timeout (15 seconds by default), so a caller
// that pauses longer must call events() now and then, which also sends heartbeats.
//
// Invariants. Exactly one thread uses a client. Every reply read from the socket either
// answers the request being waited on or goes into the event queue, in the order it
// arrived, and the tracker has seen every one of them. After the session is Ended or
// Failed, every call returns Closed without touching the socket.

#include "litbook/client/order_tracker.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/loadgen/client_session.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/types.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace litbook::client {

struct OrderClientConfig {
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
    std::string username;
    std::string password;
    protocol::UserRefNum first_ref = 1;
    // How long each call waits, in milliseconds. The login waits this long too.
    int timeout_ms = 5000;
    loadgen::ClientSessionConfig session;
};

// What became of one request.
enum class Outcome : std::uint8_t {
    Answered,  // reply holds the answer.
    Gone,      // The order was not open any more when the exchange got the request.
    NotOpen,   // The client already knew the order was not open, so it sent nothing.
    TimedOut,  // No answer within the timeout. The request may still be answered later.
    Closed,    // The session ended or failed. See OrderClient::failure().
    Refused,   // The client could not send it (a price the wire cannot carry).
};

[[nodiscard]] const char* to_string(Outcome outcome) noexcept;

struct Response {
    Outcome outcome = Outcome::TimedOut;
    protocol::UserRefNum user_ref = 0;  // The number the request used, 0 if none was sent.
    std::optional<protocol::OutboundMessage> reply;
};

// The fields of an Enter Order apart from its UserRefNum, which the client picks.
struct NewOrder {
    Ticker symbol;
    Side side = Side::Buy;
    Qty qty = 0;
    Price price = 0;  // 0 for a market order.
    TimeInForce tif = TimeInForce::GoodTillCancel;
    OrderType type = OrderType::Limit;
    bool post_only = false;
};

class OrderClient {
public:
    // Connects and waits for the login to be accepted. Throws std::system_error if the
    // connection fails, std::runtime_error if the login is refused or times out, and
    // std::invalid_argument for a username or password too long for SoupBinTCP's fields,
    // a negative timeout or a first_ref of 0.
    explicit OrderClient(const OrderClientConfig& config);
    // Logs out if the session is still active, then closes the socket.
    ~OrderClient();

    OrderClient(const OrderClient&) = delete;
    OrderClient& operator=(const OrderClient&) = delete;
    OrderClient(OrderClient&&) = delete;
    OrderClient& operator=(OrderClient&&) = delete;

    // Sends an Enter Order and waits for Order Accepted or Rejected.
    Response enter(const NewOrder& order);
    // Sends a Replace Order for the open order `ref`. `qty` means what OUCH means by it,
    // the total shares the order may ever execute, counting those that already did. Waits for Order
    // Replaced or Rejected, or until the order is gone.
    Response replace(protocol::UserRefNum ref, Qty qty, Price price);
    // Sends a Cancel Order for everything still open of `ref` and waits for Order Canceled,
    // or until the order is gone.
    Response cancel(protocol::UserRefNum ref);

    // Waits up to timeout_ms (0 does not wait) for at least one queued event, then hands
    // out every queued event in arrival order.
    std::vector<protocol::OutboundMessage> events(int timeout_ms);
    // Events read but not yet handed out.
    [[nodiscard]] std::size_t pending_events() const noexcept { return events_.size(); }

    // Sends a Logout Request and waits (up to the timeout) until the server closes the
    // connection, so the account is free again when it returns. The session is Ended
    // afterwards, and replies that were still on their way are dropped.
    void logout();

    [[nodiscard]] const OrderTracker& orders() const noexcept { return tracker_; }
    [[nodiscard]] protocol::UserRefNum next_ref() const noexcept { return next_ref_; }
    [[nodiscard]] loadgen::SessionState state() const noexcept { return session_.state(); }
    [[nodiscard]] loadgen::SessionFailure failure() const noexcept { return session_.failure(); }
    [[nodiscard]] bool active() const noexcept { return session_.active(); }
    [[nodiscard]] const std::string& username() const noexcept { return username_; }

private:
    // Sends `message` and waits for what `request` describes.
    Response send_and_wait(const protocol::InboundMessage& message, const Awaited& request);
    // Moves the session along once. Writes what it can, then waits up to timeout_ms for the
    // socket and reads what is there. Replies go to the tracker and then to `awaited` if
    // they answer it, otherwise to the event queue.
    void step(int timeout_ms, const Awaited* awaited,
              std::optional<protocol::OutboundMessage>* answer);
    // Writes as much of the output as the socket takes now. Fails the session on an error.
    void flush() noexcept;

    std::string username_;
    int timeout_ms_;
    gateway::Fd socket_;
    loadgen::ClientSession session_;
    OrderTracker tracker_;
    std::deque<protocol::OutboundMessage> events_;
    protocol::UserRefNum next_ref_;
};

}  // namespace litbook::client
