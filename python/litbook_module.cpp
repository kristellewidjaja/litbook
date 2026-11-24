// The Python module `litbook`. It wraps the blocking order client and the feed book from
// include/litbook/client/ and adds nothing of its own beyond turning C++ values into
// Python ones (DECISIONS.md D-071).
//
// Prices are integer ticks with 4 implied decimals (1'000'000 is $100.0000) and quantities
// are whole shares, the same units as the exchange, so no float ever reaches it. pybind11
// refuses a Python int that does not fit the C++ type (a negative quantity, a price beyond
// 64 bits) with a TypeError.
//
// Every call that can wait lets go of the Python interpreter lock while it waits, so other
// Python threads keep running. The C++ code never touches a Python object while the lock
// is released. The answer is turned into Python objects only after the lock is taken back.

#include "litbook/client/feed_book.hpp"
#include "litbook/client/order_client.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/feed/level_book.hpp"
#include "litbook/feed/multicast.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/types.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace py = pybind11;

namespace {

using litbook::Ticker;
using litbook::client::FeedBook;
using litbook::client::OrderClient;
using litbook::client::Outcome;
using litbook::client::Response;

Ticker to_ticker(const std::string& symbol) {
    const std::optional<Ticker> t = litbook::make_ticker(symbol);
    if (!t) {
        throw py::value_error("not a valid symbol: '" + symbol + "'");
    }
    return *t;
}

// Raises the Python error for an outcome that is not an answer, or returns the answer (or
// None when the order was not open).
py::object to_python(const Response& r) {
    switch (r.outcome) {
        case Outcome::Answered:
            if (r.reply) {
                return py::cast(*r.reply);
            }
            return py::none();
        case Outcome::Gone:
        case Outcome::NotOpen:
            return py::none();
        case Outcome::TimedOut:
            py::set_error(PyExc_TimeoutError, "no answer from the exchange within the timeout");
            throw py::error_already_set();
        case Outcome::Closed:
            py::set_error(PyExc_ConnectionError, "the session is closed");
            throw py::error_already_set();
        case Outcome::Refused:
            throw py::value_error("the exchange cannot take this price");
    }
    return py::none();
}

std::string repr_ticker(const Ticker& t) {
    return "'" + t.name() + "'";
}

}  // namespace

PYBIND11_MODULE(litbook, m) {
    m.doc() = "Clients for the litbook exchange: a blocking order entry client and a feed book.";

    py::enum_<litbook::Side>(m, "Side")
        .value("Buy", litbook::Side::Buy)
        .value("Sell", litbook::Side::Sell);
    py::enum_<litbook::TimeInForce>(m, "TimeInForce")
        .value("GoodTillCancel", litbook::TimeInForce::GoodTillCancel)
        .value("ImmediateOrCancel", litbook::TimeInForce::ImmediateOrCancel)
        .value("FillOrKill", litbook::TimeInForce::FillOrKill);
    py::enum_<litbook::OrderType>(m, "OrderType")
        .value("Limit", litbook::OrderType::Limit)
        .value("Market", litbook::OrderType::Market);
    py::enum_<litbook::protocol::WireCancelReason>(m, "CancelReason")
        .value("UserRequested", litbook::protocol::WireCancelReason::UserRequested)
        .value("ImmediateOrCancel", litbook::protocol::WireCancelReason::ImmediateOrCancel)
        .value("SelfMatchPrevention", litbook::protocol::WireCancelReason::SelfMatchPrevention)
        .value("System", litbook::protocol::WireCancelReason::System);
    py::enum_<litbook::protocol::WireRejectReason>(m, "RejectReason")
        .value("Other", litbook::protocol::WireRejectReason::Other)
        .value("InvalidQuantity", litbook::protocol::WireRejectReason::InvalidQuantity)
        .value("InvalidSymbol", litbook::protocol::WireRejectReason::InvalidSymbol)
        .value("InvalidPrice", litbook::protocol::WireRejectReason::InvalidPrice)
        .value("MaxQuantityExceeded", litbook::protocol::WireRejectReason::MaxQuantityExceeded)
        .value("PostOnlyWouldCross", litbook::protocol::WireRejectReason::PostOnlyWouldCross)
        .value("InvalidOrderType", litbook::protocol::WireRejectReason::InvalidOrderType)
        .value("MaxOpenOrdersExceeded", litbook::protocol::WireRejectReason::MaxOpenOrdersExceeded);
    py::enum_<litbook::protocol::Liquidity>(m, "Liquidity")
        .value("Added", litbook::protocol::Liquidity::Added)
        .value("Removed", litbook::protocol::Liquidity::Removed);
    py::enum_<litbook::client::SymbolStatus>(m, "SymbolStatus")
        .value("Unknown", litbook::client::SymbolStatus::Unknown)
        .value("Live", litbook::client::SymbolStatus::Live)
        .value("Stale", litbook::client::SymbolStatus::Stale);

    // The replies. Their fields are read only, because they describe what the exchange
    // said.
    using litbook::protocol::AcceptedMessage;
    py::class_<AcceptedMessage>(m, "Accepted")
        .def_readonly("timestamp", &AcceptedMessage::timestamp)
        .def_readonly("user_ref", &AcceptedMessage::user_ref)
        .def_readonly("side", &AcceptedMessage::side)
        .def_readonly("qty", &AcceptedMessage::qty)
        .def_property_readonly("symbol", [](const AcceptedMessage& a) { return a.symbol.name(); })
        .def_readonly("price", &AcceptedMessage::price)
        .def_readonly("tif", &AcceptedMessage::tif)
        .def_readonly("type", &AcceptedMessage::type)
        .def_readonly("post_only", &AcceptedMessage::post_only)
        .def_readonly("order_ref", &AcceptedMessage::order_ref)
        .def("__repr__", [](const AcceptedMessage& a) {
            return "Accepted(user_ref=" + std::to_string(a.user_ref) +
                   ", symbol=" + repr_ticker(a.symbol) + ", qty=" + std::to_string(a.qty) +
                   ", price=" + std::to_string(a.price) + ")";
        });

    using litbook::protocol::ReplacedMessage;
    py::class_<ReplacedMessage>(m, "Replaced")
        .def_readonly("timestamp", &ReplacedMessage::timestamp)
        .def_readonly("orig_user_ref", &ReplacedMessage::orig_user_ref)
        .def_readonly("user_ref", &ReplacedMessage::user_ref)
        .def_readonly("side", &ReplacedMessage::side)
        .def_readonly("qty", &ReplacedMessage::qty)
        .def_property_readonly("symbol", [](const ReplacedMessage& r) { return r.symbol.name(); })
        .def_readonly("price", &ReplacedMessage::price)
        .def_readonly("order_ref", &ReplacedMessage::order_ref)
        .def_readonly("kept_priority", &ReplacedMessage::kept_priority)
        .def("__repr__", [](const ReplacedMessage& r) {
            return "Replaced(orig_user_ref=" + std::to_string(r.orig_user_ref) +
                   ", user_ref=" + std::to_string(r.user_ref) + ", qty=" + std::to_string(r.qty) +
                   ", price=" + std::to_string(r.price) + ")";
        });

    using litbook::protocol::CanceledMessage;
    py::class_<CanceledMessage>(m, "Canceled")
        .def_readonly("timestamp", &CanceledMessage::timestamp)
        .def_readonly("user_ref", &CanceledMessage::user_ref)
        .def_readonly("qty", &CanceledMessage::qty)
        .def_readonly("reason", &CanceledMessage::reason)
        .def("__repr__", [](const CanceledMessage& c) {
            return "Canceled(user_ref=" + std::to_string(c.user_ref) +
                   ", qty=" + std::to_string(c.qty) + ")";
        });

    using litbook::protocol::ExecutedMessage;
    py::class_<ExecutedMessage>(m, "Executed")
        .def_readonly("timestamp", &ExecutedMessage::timestamp)
        .def_readonly("user_ref", &ExecutedMessage::user_ref)
        .def_readonly("qty", &ExecutedMessage::qty)
        .def_readonly("price", &ExecutedMessage::price)
        .def_readonly("liquidity", &ExecutedMessage::liquidity)
        .def_readonly("match", &ExecutedMessage::match)
        .def("__repr__", [](const ExecutedMessage& e) {
            return "Executed(user_ref=" + std::to_string(e.user_ref) +
                   ", qty=" + std::to_string(e.qty) + ", price=" + std::to_string(e.price) +
                   ", match=" + std::to_string(e.match) + ")";
        });

    using litbook::protocol::RejectedMessage;
    py::class_<RejectedMessage>(m, "Rejected")
        .def_readonly("timestamp", &RejectedMessage::timestamp)
        .def_readonly("user_ref", &RejectedMessage::user_ref)
        .def_readonly("reason", &RejectedMessage::reason)
        .def("__repr__", [](const RejectedMessage& r) {
            return "Rejected(user_ref=" + std::to_string(r.user_ref) +
                   ", reason=" + std::to_string(static_cast<unsigned>(r.reason)) + ")";
        });

    using litbook::client::OpenOrder;
    py::class_<OpenOrder>(m, "OpenOrder")
        .def_property_readonly("symbol", [](const OpenOrder& o) { return o.symbol.name(); })
        .def_readonly("side", &OpenOrder::side)
        .def_readonly("price", &OpenOrder::price)
        .def_readonly("open", &OpenOrder::open)
        .def_readonly("executed", &OpenOrder::executed)
        .def_readonly("order_ref", &OpenOrder::order_ref)
        .def("__repr__", [](const OpenOrder& o) {
            return "OpenOrder(symbol=" + repr_ticker(o.symbol) +
                   ", price=" + std::to_string(o.price) + ", open=" + std::to_string(o.open) + ")";
        });

    py::class_<OrderClient>(m, "OrderClient",
                            "A blocking order entry client for one account. Each request "
                            "waits for its answer. Other replies, such as executions, are "
                            "read with events().")
        .def(py::init([](const std::string& host, std::uint16_t port, const std::string& username,
                         const std::string& password, litbook::protocol::UserRefNum first_ref,
                         int timeout_ms) {
                 litbook::client::OrderClientConfig c;
                 c.address = host;
                 c.port = port;
                 c.username = username;
                 c.password = password;
                 c.first_ref = first_ref;
                 c.timeout_ms = timeout_ms;
                 const py::gil_scoped_release release;
                 return std::make_unique<OrderClient>(c);
             }),
             py::arg("host"), py::arg("port"), py::arg("username"), py::arg("password"),
             py::arg("first_ref") = 1, py::arg("timeout_ms") = 5000)
        .def(
            "enter",
            [](OrderClient& c, const std::string& symbol, litbook::Side side, litbook::Qty qty,
               litbook::Price price, litbook::TimeInForce tif, litbook::OrderType type,
               bool post_only) {
                const litbook::client::NewOrder order{.symbol = to_ticker(symbol),
                                                      .side = side,
                                                      .qty = qty,
                                                      .price = price,
                                                      .tif = tif,
                                                      .type = type,
                                                      .post_only = post_only};
                Response r;
                {
                    const py::gil_scoped_release release;
                    r = c.enter(order);
                }
                return to_python(r);
            },
            py::arg("symbol"), py::arg("side"), py::arg("qty"), py::arg("price"),
            py::arg("tif") = litbook::TimeInForce::GoodTillCancel,
            py::arg("type") = litbook::OrderType::Limit, py::arg("post_only") = false,
            "Enters an order and returns Accepted or Rejected.")
        .def(
            "replace",
            [](OrderClient& c, litbook::protocol::UserRefNum user_ref, litbook::Qty qty,
               litbook::Price price) {
                Response r;
                {
                    const py::gil_scoped_release release;
                    r = c.replace(user_ref, qty, price);
                }
                return to_python(r);
            },
            py::arg("user_ref"), py::arg("qty"), py::arg("price"),
            "Replaces an open order. qty is the total the order may ever execute, counting "
            "shares that already did. Returns Replaced or Rejected, or None if the order "
            "was not open any more.")
        .def(
            "cancel",
            [](OrderClient& c, litbook::protocol::UserRefNum user_ref) {
                Response r;
                {
                    const py::gil_scoped_release release;
                    r = c.cancel(user_ref);
                }
                return to_python(r);
            },
            py::arg("user_ref"),
            "Cancels what is open of an order. Returns Canceled, or None if the order was "
            "not open any more.")
        .def(
            "events",
            [](OrderClient& c, int timeout_ms) {
                std::vector<litbook::protocol::OutboundMessage> e;
                {
                    const py::gil_scoped_release release;
                    e = c.events(timeout_ms);
                }
                return e;
            },
            py::arg("timeout_ms") = 0,
            "Waits up to timeout_ms for a reply that answered no request, then returns all "
            "of them in arrival order.")
        .def("logout",
             [](OrderClient& c) {
                 const py::gil_scoped_release release;
                 c.logout();
             })
        .def(
            "order",
            [](const OrderClient& c, litbook::protocol::UserRefNum user_ref) -> py::object {
                const OpenOrder* o = c.orders().find(user_ref);
                return o == nullptr ? py::none() : py::cast(*o);
            },
            py::arg("user_ref"), "The open order with this UserRefNum, or None.")
        .def("open_orders",
             [](const OrderClient& c) {
                 py::dict out;
                 for (const auto& [ref, order] : c.orders().orders()) {
                     out[py::int_(ref)] = py::cast(order);
                 }
                 return out;
             })
        .def_property_readonly("next_ref", &OrderClient::next_ref)
        .def_property_readonly("active", &OrderClient::active)
        .def_property_readonly("username", &OrderClient::username)
        .def_property_readonly(
            "failure",
            [](const OrderClient& c) { return litbook::loadgen::to_string(c.failure()); })
        .def(
            "__enter__", [](OrderClient& c) -> OrderClient& { return c; },
            py::return_value_policy::reference)
        .def("__exit__", [](OrderClient& c, const py::args&) {
            const py::gil_scoped_release release;
            c.logout();
        });

    using litbook::feed::PriceLevel;
    py::class_<PriceLevel>(m, "Level")
        .def_readonly("price", &PriceLevel::price)
        .def_readonly("qty", &PriceLevel::total_qty)
        .def_readonly("orders", &PriceLevel::order_count)
        .def("__repr__", [](const PriceLevel& l) {
            return "Level(price=" + std::to_string(l.price) +
                   ", qty=" + std::to_string(l.total_qty) +
                   ", orders=" + std::to_string(l.order_count) + ")";
        });

    using litbook::client::Quote;
    py::class_<Quote>(m, "Quote")
        .def_readonly("bid", &Quote::bid)
        .def_readonly("ask", &Quote::ask)
        .def_readonly("live", &Quote::live);

    py::class_<FeedBook>(m, "FeedBook",
                         "The market data feed's books, kept current by a reader thread.")
        .def(
            py::init([](const std::string& interface, const std::string& incremental_address,
                        std::uint16_t incremental_port, const std::string& snapshot_address,
                        std::uint16_t snapshot_port, const std::string& session,
                        std::size_t max_symbols, std::size_t max_levels,
                        std::uint64_t silence_limit_ms) {
                litbook::client::FeedBookConfig c;
                c.receiver.interface_name = interface;
                c.receiver.incremental = {.address = incremental_address, .port = incremental_port};
                c.receiver.snapshot = {.address = snapshot_address, .port = snapshot_port};
                c.receiver.silence_limit = silence_limit_ms * 1'000'000;
                c.receiver.handler.session = session;
                c.receiver.handler.max_symbols = max_symbols;
                c.receiver.handler.max_levels_per_side = max_levels;
                return std::make_unique<FeedBook>(c);
            }),
            py::arg("interface") = "lo", py::arg("incremental_address") = "239.255.0.1",
            py::arg("incremental_port") = 15100, py::arg("snapshot_address") = "239.255.0.2",
            py::arg("snapshot_port") = 15101, py::arg("session") = "1", py::arg("max_symbols") = 64,
            py::arg("max_levels") = 1024, py::arg("silence_limit_ms") = 3000)
        .def_property_readonly("incremental_port", &FeedBook::incremental_port)
        .def_property_readonly("snapshot_port", &FeedBook::snapshot_port)
        .def(
            "status", [](const FeedBook& b, const std::string& s) { return b.status(s); },
            py::arg("symbol"))
        .def(
            "best", [](const FeedBook& b, const std::string& s) { return b.best(s); },
            py::arg("symbol"), "The best bid and ask, or None for an unknown symbol.")
        .def(
            "depth",
            [](const FeedBook& b, const std::string& s, litbook::Side side, std::size_t levels) {
                return b.depth(s, side, levels);
            },
            py::arg("symbol"), py::arg("side"), py::arg("levels") = 10)
        .def("symbols", &FeedBook::symbols)
        .def_property_readonly("version", &FeedBook::version)
        .def(
            "wait_update",
            [](const FeedBook& b, std::uint64_t seen, int timeout_ms) {
                const py::gil_scoped_release release;
                return b.wait_update(seen, timeout_ms);
            },
            py::arg("seen"), py::arg("timeout_ms"),
            "Waits up to timeout_ms for the version to pass `seen` and returns the version.")
        .def("stats",
             [](const FeedBook& b) {
                 const litbook::client::FeedBookStats s = b.stats();
                 py::dict d;
                 d["version"] = s.version;
                 d["started"] = s.started;
                 d["ended"] = s.ended;
                 d["recovering"] = s.recovering;
                 d["silent"] = s.silent;
                 d["incremental_packets"] = s.handler.incremental_packets;
                 d["snapshot_packets"] = s.handler.snapshot_packets;
                 d["applied"] = s.handler.applied;
                 d["gaps"] = s.handler.gaps;
                 d["recoveries"] = s.handler.recoveries;
                 d["trades"] = s.handler.trades;
                 d["silences"] = s.receiver.silences;
                 return d;
             })
        .def("stop",
             [](FeedBook& b) {
                 const py::gil_scoped_release release;
                 b.stop();
             })
        .def(
            "__enter__", [](FeedBook& b) -> FeedBook& { return b; },
            py::return_value_policy::reference)
        .def("__exit__", [](FeedBook& b, const py::args&) {
            const py::gil_scoped_release release;
            b.stop();
        });
}
