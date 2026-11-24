"""A noise trader that sends random orders at random times.

Noise traders stand for everybody who trades for reasons of their own and not because
of the price, and they are what a market maker earns its spread from. This one waits
a random time between actions, drawn from an exponential distribution, so its actions
arrive like a Poisson process (no rhythm another bot could line up with). Each action
is one of three kinds.
  take    an IOC order a few ticks through the other side's best price
  rest    a limit order on its own side, a few ticks behind the middle of the book
  cancel  one of its own open orders, picked at random

All randomness comes from one random.Random with a seed, so a run's decisions repeat
for the same book states. The waiting time is a float, but it only sets when to act.
Prices are worked out in integer ticks.

Run one against a gateway with
    python3 -m litbook_bots.noise_trader --port PORT --user NT1 --password pw --seed 7
"""

import argparse
import random
import threading
import time
from dataclasses import dataclass

import litbook

from .position import Position
from .replies import kind
from .prices import TICK, ceil_to_tick, floor_to_tick

BUY = litbook.Side.Buy
SELL = litbook.Side.Sell


@dataclass(frozen=True)
class NoiseConfig:
    symbol: str = "AAPL"
    fair_value: int = 1_000_000  # The middle to use while the book has no bid or no offer.
    rate: float = 5.0  # Actions per second on average.
    take_weight: int = 3
    rest_weight: int = 5
    cancel_weight: int = 2
    max_qty: int = 200  # Each order is for 1 to max_qty shares.
    depth_ticks: int = 5  # Resting orders go 0 to depth_ticks ticks behind the middle.
    through_ticks: int = 2  # Taking orders go this many ticks past the best price.
    max_open: int = 10  # At this many open orders, the next action is always a cancel.
    max_delay: float = 1.0  # Waits are capped, so a stop is noticed within this time.

    def __post_init__(self):
        if self.rate <= 0 or self.max_qty <= 0 or self.max_open <= 0:
            raise ValueError("rate, max_qty and max_open must be positive")
        if min(self.take_weight, self.rest_weight, self.cancel_weight) < 0:
            raise ValueError("weights must not be negative")
        if self.take_weight + self.rest_weight == 0:
            raise ValueError("the trader must be able to send orders")


@dataclass(frozen=True)
class NoiseAction:
    kind: str  # take, rest or cancel
    side: object = None
    qty: int = 0
    price: int = 0
    ref: int = 0


def middle(bid, ask, fair):
    """The middle of the book, or the one side that exists, or the fair value."""
    if bid is not None and ask is not None:
        return (bid + ask) // 2
    if bid is not None:
        return bid
    if ask is not None:
        return ask
    return fair


def choose_action(rng, config, bid, ask, open_refs):
    """Draws the next action from the best bid and offer (None when a side is empty)
    and the trader's own open UserRefNums."""
    open_refs = sorted(open_refs)
    if len(open_refs) >= config.max_open:
        kind = "cancel"
    else:
        weights = [config.take_weight, config.rest_weight,
                   config.cancel_weight if open_refs else 0]
        kind = rng.choices(["take", "rest", "cancel"], weights)[0]
    if kind == "cancel":
        return NoiseAction("cancel", ref=rng.choice(open_refs))
    side = rng.choice([BUY, SELL])
    qty = rng.randint(1, config.max_qty)
    mid = middle(bid, ask, config.fair_value)
    if kind == "take":
        # Through the other side's best price, or through the middle when that side
        # is empty (the IOC order then just ends without a trade).
        far = ask if side == BUY else bid
        base = floor_to_tick(mid) if far is None else far
        through = config.through_ticks * TICK
        price = base + through if side == BUY else base - through
    else:
        # The middle is rounded away from the other side, so an order 0 ticks behind
        # it still rests when the spread is one tick wide.
        behind = rng.randint(0, config.depth_ticks) * TICK
        price = floor_to_tick(mid) - behind if side == BUY else ceil_to_tick(mid) + behind
    return NoiseAction(kind, side, qty, max(price, TICK))


def next_delay(rng, config):
    """Seconds until the next action, exponential with mean 1 / rate, capped."""
    return min(rng.expovariate(config.rate), config.max_delay)


class NoiseTrader:
    """The loop for one noise trading account.

    Invariants. Every UserRefNum it sent is in side_of, and position changes only from
    Executed replies, once per execution.
    """

    def __init__(self, client, feed, config=NoiseConfig(), seed=0):
        self.client = client
        self.feed = feed
        self.config = config
        self.rng = random.Random(seed)
        self.side_of = {}
        self.position = Position()
        self.fills = []
        self.counts = {"take": 0, "rest": 0, "cancel": 0}
        self.rejects = 0
        self.self_match_cancels = 0

    def handle_events(self, events):
        for event in events:
            if kind(event) == "Executed":
                side = self.side_of[event.user_ref]
                self.position.apply_fill(side, event.qty, event.price)
                self.fills.append((event.match, side, event.qty, event.price))
            elif (kind(event) == "Canceled"
                  and event.reason == litbook.CancelReason.SelfMatchPrevention):
                self.self_match_cancels += 1

    def act(self):
        """Draws one action from the current book and sends it."""
        quote = self.feed.best(self.config.symbol)
        bid = quote.bid.price if quote is not None and quote.bid is not None else None
        ask = quote.ask.price if quote is not None and quote.ask is not None else None
        action = choose_action(self.rng, self.config, bid, ask, self.client.open_orders())
        self.counts[action.kind] += 1
        if action.kind == "cancel":
            self.client.cancel(action.ref)
            return action
        tif = (litbook.TimeInForce.ImmediateOrCancel if action.kind == "take"
               else litbook.TimeInForce.GoodTillCancel)
        reply = self.client.enter(self.config.symbol, action.side, action.qty, action.price,
                                  tif=tif)
        if kind(reply) == "Accepted":
            self.side_of[reply.user_ref] = action.side
        else:
            self.rejects += 1
        return action

    def run(self, stop):
        """Acts at random times until stop is set, reading replies while it waits."""
        while not stop.is_set():
            due = time.monotonic() + next_delay(self.rng, self.config)
            while not stop.is_set():
                left_ms = int((due - time.monotonic()) * 1000)
                if left_ms <= 0:
                    break
                self.handle_events(self.client.events(timeout_ms=min(left_ms, 100)))
            if not stop.is_set():
                self.act()
        self.handle_events(self.client.events())


def main(argv=None):
    parser = argparse.ArgumentParser(description="Send random orders to one stock.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=15000)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument("--rate", type=float, default=5.0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--interface", default="lo")
    parser.add_argument("--incremental", default="239.255.0.1:15100")
    parser.add_argument("--snapshot", default="239.255.0.2:15101")
    args = parser.parse_args(argv)
    inc_addr, inc_port = args.incremental.rsplit(":", 1)
    snap_addr, snap_port = args.snapshot.rsplit(":", 1)
    config = NoiseConfig(symbol=args.symbol, rate=args.rate)
    stop = threading.Event()
    with litbook.FeedBook(args.interface, inc_addr, int(inc_port), snap_addr,
                          int(snap_port)) as feed, \
            litbook.OrderClient(args.host, args.port, args.user, args.password) as client:
        trader = NoiseTrader(client, feed, config, args.seed)
        try:
            trader.run(stop)
        except KeyboardInterrupt:
            pass
        print(f"position {trader.position.shares} shares, {trader.position.fills} fills, "
              f"actions {trader.counts}")


if __name__ == "__main__":
    main()
