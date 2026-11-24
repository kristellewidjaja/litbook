"""A market maker that keeps one bid and one offer in a stock around its fair value.

The decisions are plain functions. others_best finds the best price other traders show
once the maker's own orders are taken out of the feed's levels, fair_value turns the
others' best bid and offer into an estimate, target_quotes picks the two quotes for a
fair value and a position, and plan_side says how to get from the order on the book
to the quote wanted. MarketMaker is the loop around them. It reads the feed and the
replies, calls the functions and sends what they say.

The maker leans against its position (inventory skew). When it is long it moves both
quotes down, so its offer is more likely to be hit and its bid less likely, and the
other way round when it is short. It also stops bidding at its position limit.

Run one against a gateway with
    python3 -m litbook_bots.market_maker --port PORT --user MM1 --password pw
"""

import argparse
import threading
from dataclasses import dataclass

import litbook

from .position import Position
from .replies import kind
from .prices import ceil_to_tick, floor_to_tick

BUY = litbook.Side.Buy
SELL = litbook.Side.Sell


@dataclass(frozen=True)
class MakerConfig:
    """How the maker quotes. Prices are in price units (100 is one cent)."""

    symbol: str = "AAPL"
    fair_value: int = 1_000_000  # Used until other traders show both a bid and an offer.
    half_spread: int = 200  # Distance from the center to each quote before rounding.
    size: int = 100  # Shares shown on each side.
    max_position: int = 500  # The maker never buys past this long or sells past this short.
    skew: int = 1  # Price units the center moves for each share of position.

    def __post_init__(self):
        if self.half_spread <= 0 or self.size <= 0 or self.max_position <= 0 or self.skew < 0:
            raise ValueError("half_spread, size and max_position must be positive")
        if self.fair_value <= 0:
            raise ValueError("fair_value must be positive")


@dataclass(frozen=True)
class Quote:
    price: int
    qty: int


@dataclass(frozen=True)
class Action:
    """One step toward a target quote. kind is keep, enter, replace or cancel. For a
    replace, qty is the new total in the OUCH sense, which counts the executed shares."""

    kind: str
    price: int = 0
    qty: int = 0


def others_best(levels, own):
    """The best price on one side of the book that has shares from somebody else.

    levels is the feed's side as (price, qty) pairs, best first, and own maps each
    price to the shares the maker itself shows there. The feed can lag the maker's own
    replies by a moment, so own can say more than a level holds. That level then
    counts as the maker's alone. Returns None when only the maker is on this side.
    """
    for price, qty in levels:
        if qty > own.get(price, 0):
            return price
    return None


def fair_value(previous, other_bid, other_ask):
    """The middle of the others' best bid and offer, or the previous value when one of
    them is missing. The maker's own quotes are left out on purpose. They already
    include its skew, and a maker that read them as the market would skew again on
    every requote and walk the price away by itself."""
    if other_bid is None or other_ask is None:
        return previous
    return (other_bid + other_ask) // 2


def target_quotes(fair, shares, config, other_bid=None, other_ask=None):
    """The bid and the offer the maker wants, each a Quote or None.

    The center is the fair value moved against the position by skew per share. The bid
    is rounded down to a tick and the offer up, so they stay at least half_spread from
    the center. Neither may reach the other side's best price, because the quotes are
    post only and would be rejected. Each side's size stops at the position limit, so
    a full fill can never take the maker past it.
    """
    center = fair - config.skew * shares
    bid = floor_to_tick(center - config.half_spread)
    ask = ceil_to_tick(center + config.half_spread)
    if other_ask is not None:
        bid = min(bid, floor_to_tick(other_ask - 1))  # The highest tick below their offer.
    if other_bid is not None:
        ask = max(ask, ceil_to_tick(other_bid + 1))  # The lowest tick above their bid.
    bid_qty = min(config.size, config.max_position - shares)
    ask_qty = min(config.size, config.max_position + shares)
    bid_quote = Quote(bid, bid_qty) if bid_qty > 0 and bid > 0 else None
    ask_quote = Quote(ask, ask_qty) if ask_qty > 0 else None
    return bid_quote, ask_quote


def plan_side(current, target):
    """How to move one side from the order on the book to the target quote.

    current is the open order on this side (anything with price, open and executed) or
    None, and target is a Quote or None. A new price always needs a replace. At the
    same price the maker leaves the order alone unless it shows more than the target
    (the position limit shrank) or fills took it below half the target. Small partial
    fills do not cause a message each, which keeps the maker from flooding the exchange.
    """
    if target is None:
        return Action("keep") if current is None else Action("cancel")
    if current is None:
        return Action("enter", target.price, target.qty)
    if current.price != target.price or current.open > target.qty or 2 * current.open < target.qty:
        return Action("replace", target.price, current.executed + target.qty)
    return Action("keep")


def own_levels(open_orders, side):
    """The maker's shares per price on one side, from the client's open orders."""
    levels = {}
    for order in open_orders.values():
        if order.side == side:
            levels[order.price] = levels.get(order.price, 0) + order.open
    return levels


class MarketMaker:
    """The quoting loop for one account and one stock.

    It owns the client (one thread per client) and only reads the feed book. Every
    reference it ever used is kept with its side, because an execution can arrive
    after the order is gone from the client's open orders.

    refs holds the last UserRefNum used on each side. That order may have filled or
    been canceled since, so the maker never trusts refs alone. It always asks the
    client (client.order) whether the order is still open, and the client's tracker
    has seen every reply.

    Invariants. Each side has at most one open order. position changes only from
    Executed replies, once per execution.
    """

    def __init__(self, client, feed, config=MakerConfig()):
        self.client = client
        self.feed = feed
        self.config = config
        self.refs = {BUY: None, SELL: None}
        self.side_of = {}
        self.position = Position()
        self.fair = config.fair_value
        self.fills = []  # (match, side, qty, price) of every execution.
        self.sent = 0
        self.rejects = 0
        self.events = 0

    def handle_events(self, events):
        for event in events:
            self.events += 1
            if kind(event) == "Executed":
                side = self.side_of[event.user_ref]
                self.position.apply_fill(side, event.qty, event.price)
                self.fills.append((event.match, side, event.qty, event.price))

    def step(self):
        """Reads new replies and the book once, then moves both quotes."""
        self.handle_events(self.client.events())
        symbol = self.config.symbol
        if self.feed.status(symbol) != litbook.SymbolStatus.Live:
            # The book may be wrong or unknown, so quoting from it is guessing. Pull the
            # quotes until the feed is live again.
            for side in (BUY, SELL):
                self._do(side, plan_side(self._current(side), None))
            return
        open_orders = self.client.open_orders()
        bids = [(lv.price, lv.qty) for lv in self.feed.depth(symbol, BUY, 10)]
        asks = [(lv.price, lv.qty) for lv in self.feed.depth(symbol, SELL, 10)]
        other_bid = others_best(bids, own_levels(open_orders, BUY))
        other_ask = others_best(asks, own_levels(open_orders, SELL))
        self.fair = fair_value(self.fair, other_bid, other_ask)
        bid, ask = target_quotes(self.fair, self.position.shares, self.config, other_bid,
                                 other_ask)
        self._do(BUY, plan_side(self._current(BUY), bid))
        self._do(SELL, plan_side(self._current(SELL), ask))

    def run(self, stop, poll_ms=100):
        """Steps until stop is set. It wakes on every feed update and at least every
        poll_ms, which also keeps the session's heartbeats going."""
        seen = self.feed.version
        while not stop.is_set():
            self.step()
            seen = self.feed.wait_update(seen, poll_ms)

    def cancel_all(self):
        for side in (BUY, SELL):
            self._do(side, Action("cancel") if self._current(side) is not None else Action("keep"))

    def _current(self, side):
        ref = self.refs[side]
        return None if ref is None else self.client.order(ref)

    def _remember(self, side, ref):
        self.refs[side] = ref
        self.side_of[ref] = side

    def _do(self, side, action):
        if action.kind == "keep":
            return
        self.sent += 1
        if action.kind == "enter":
            reply = self.client.enter(self.config.symbol, side, action.qty, action.price,
                                      post_only=True)
            if kind(reply) == "Accepted":
                self._remember(side, reply.user_ref)
            else:
                self.rejects += 1
        elif action.kind == "replace":
            reply = self.client.replace(self.refs[side], action.qty, action.price)
            if kind(reply) == "Replaced":
                self._remember(side, reply.user_ref)
            elif kind(reply) == "Rejected":
                # OUCH cancels an order whose replace was refused. If the order filled
                # on its way instead, the reply is None. Either way the next step asks
                # the client whether the order is still open.
                self.rejects += 1
        else:
            self.client.cancel(self.refs[side])


def main(argv=None):
    parser = argparse.ArgumentParser(description="Quote both sides of one stock.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=15000)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument("--fair-value", type=int, default=1_000_000)
    parser.add_argument("--half-spread", type=int, default=200)
    parser.add_argument("--size", type=int, default=100)
    parser.add_argument("--max-position", type=int, default=500)
    parser.add_argument("--skew", type=int, default=1)
    parser.add_argument("--interface", default="lo")
    parser.add_argument("--incremental", default="239.255.0.1:15100")
    parser.add_argument("--snapshot", default="239.255.0.2:15101")
    args = parser.parse_args(argv)
    config = MakerConfig(args.symbol, args.fair_value, args.half_spread, args.size,
                         args.max_position, args.skew)
    inc_addr, inc_port = args.incremental.rsplit(":", 1)
    snap_addr, snap_port = args.snapshot.rsplit(":", 1)
    stop = threading.Event()
    with litbook.FeedBook(args.interface, inc_addr, int(inc_port), snap_addr,
                          int(snap_port)) as feed, \
            litbook.OrderClient(args.host, args.port, args.user, args.password) as client:
        maker = MarketMaker(client, feed, config)
        try:
            maker.run(stop)
        except KeyboardInterrupt:
            maker.cancel_all()
        print(f"position {maker.position.shares} shares, {maker.position.fills} fills, "
              f"{maker.sent} messages")


if __name__ == "__main__":
    main()
