"""A small simulated market. It starts a litbook_gateway with its feed on loopback,
runs market makers and noise traders against it for a while, checks that everything
adds up, and prints a summary with the final book.

    python3 -m litbook_bots.simulate --seconds 20 --makers 1 --noise 4 --seed 1 --view

Each bot is a thread with its own order client, so to the exchange it is a separate
TCP client with its own account, as a real participant would be. They share one feed
book, which is safe because the feed book copies what it hands out under its mutex.
Every wait in the module releases the interpreter lock, so the threads do not hold
each other up while they wait on the network.

After the bots stop, the run checks the market from the outside.
  1. Every match number was seen exactly twice, once by a buyer and once by a seller,
     for the same shares at the same price.
  2. The positions add up to zero shares and zero cash over all accounts.
  3. The feed printed one trade per match.
  4. The book on the feed is exactly the bots' open orders added up per price.
  5. After the bots log out (their accounts cancel on disconnect) the book is empty.
"""

import argparse
import os
import re
import signal
import subprocess
import threading
import time
from dataclasses import dataclass, field

import litbook

from . import book_view
from .market_maker import MakerConfig, MarketMaker
from .noise_trader import NoiseConfig, NoiseTrader
from .prices import format_money, format_price

READY = re.compile(r"litbook_gateway listening on 127\.0\.0\.1:(\d+)")
PASSWORD = "pw"


class Gateway:
    """A litbook_gateway child process. Its output after the first line is read by a
    thread, so the pipe never fills up and stops the gateway."""

    def __init__(self, binary, args):
        self.process = subprocess.Popen([binary, "--port", "0", *args],
                                        stdout=subprocess.PIPE, text=True)
        line = self.process.stdout.readline()
        match = READY.search(line)
        if not match:
            self.process.kill()
            self.process.wait()
            raise RuntimeError("the gateway did not start: " + line.strip())
        self.port = int(match.group(1))
        self.output = []
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.output.append(line.rstrip("\n"))

    def stop(self):
        """Stops the gateway as Ctrl-C would, so it sends End of Session first."""
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=5)
        return self.process.returncode


@dataclass
class MarketConfig:
    seconds: float = 10.0
    makers: int = 1
    noise: int = 4
    seed: int = 1
    symbol: str = "AAPL"
    fair_value: int = 1_000_000
    noise_rate: float = 5.0
    interface: str = "lo"
    incremental: str = "239.255.0.1"
    snapshot: str = "239.255.0.2"
    gateway: str = field(default_factory=lambda: os.environ.get("LITBOOK_GATEWAY", ""))


@dataclass
class Result:
    """What a run did and what its checks found. problems is empty when all passed."""

    bots: list
    matches: int
    volume: int
    feed_trades: int
    feed_stats: dict
    mark: int
    final_book: list
    problems: list
    seconds: float


def check_matches(fills_by_bot):
    """Check 1. fills_by_bot maps a bot name to its (match, side, qty, price) fills.
    Returns the problems found, the number of matches and the shares traded."""
    seen = {}
    for name, fills in fills_by_bot.items():
        for match, side, qty, price in fills:
            seen.setdefault(match, []).append((name, side, qty, price))
    problems = []
    volume = 0
    for match, sides in sorted(seen.items()):
        if len(sides) != 2:
            problems.append(f"match {match} seen {len(sides)} times")
            continue
        (_, side_a, qty_a, price_a), (_, side_b, qty_b, price_b) = sides
        if side_a == side_b or qty_a != qty_b or price_a != price_b:
            problems.append(f"match {match} sides disagree: {sides}")
            continue
        volume += qty_a
    return problems, len(seen), volume


def check_positions(positions):
    """Check 2. positions maps a bot name to its Position."""
    shares = sum(p.shares for p in positions.values())
    cash = sum(p.cash for p in positions.values())
    problems = []
    if shares != 0:
        problems.append(f"net position is {shares} shares, not 0")
    if cash != 0:
        problems.append(f"net cash is {cash}, not 0")
    return problems


def expected_levels(open_orders_by_bot, side):
    """Check 4's expectation. Every open order of every bot added up per price, as
    (price, qty, orders) best first, the way the feed reports a side."""
    levels = {}
    for orders in open_orders_by_bot.values():
        for order in orders.values():
            if order.side == side:
                qty, count = levels.get(order.price, (0, 0))
                levels[order.price] = (qty + order.open, count + 1)
    best_first = sorted(levels, reverse=(side == litbook.Side.Buy))
    return [(price, *levels[price]) for price in best_first]


def feed_levels(feed, symbol, side):
    return [(lv.price, lv.qty, lv.orders) for lv in feed.depth(symbol, side, 1024)]


def wait_until(feed, done, seconds):
    """Waits on the feed until done() is true or the time is up, and says which."""
    deadline = time.monotonic() + seconds
    seen = feed.version
    while not done():
        if time.monotonic() >= deadline:
            return False
        seen = feed.wait_update(seen, 50)
    return True


def drain(client):
    """Reads replies until none arrive for 200 ms. Nothing is sending by now, so this
    collects the executions and cancels still on their way."""
    events = []
    while True:
        got = client.events(timeout_ms=200)
        if not got:
            return events
        events += got


def run_market(config, view=False, out=print):
    """Runs one simulated market and returns its Result."""
    if not config.gateway:
        raise RuntimeError("no gateway binary (pass --gateway or set LITBOOK_GATEWAY)")
    names = [f"MM{i + 1}" for i in range(config.makers)] + \
            [f"NT{i + 1}" for i in range(config.noise)]
    feed = litbook.FeedBook(config.interface, config.incremental, 0, config.snapshot, 0)
    gateway = None
    clients = {}
    try:
        args = ["--symbol", config.symbol, "--feed", config.interface,
                "--feed-incremental", f"{config.incremental}:{feed.incremental_port}",
                "--feed-snapshot", f"{config.snapshot}:{feed.snapshot_port}"]
        for name in names:
            args += ["--account", f"{name}:{PASSWORD}:cod"]
        gateway = Gateway(config.gateway, args)
        if not book_view.wait_live(feed, config.symbol, 10):
            raise RuntimeError("the feed never showed the stock as live")
        bots = {}
        for i, name in enumerate(names):
            clients[name] = litbook.OrderClient("127.0.0.1", gateway.port, name, PASSWORD)
            if name.startswith("MM"):
                maker_config = MakerConfig(symbol=config.symbol, fair_value=config.fair_value)
                bots[name] = MarketMaker(clients[name], feed, maker_config)
            else:
                noise_config = NoiseConfig(symbol=config.symbol, fair_value=config.fair_value,
                                           rate=config.noise_rate)
                bots[name] = NoiseTrader(clients[name], feed, noise_config,
                                         seed=config.seed * 1000 + i)

        stop = threading.Event()
        errors = {}

        def run_bot(name, bot):
            try:
                bot.run(stop)
            except Exception as error:  # Reported by the checks below.
                errors[name] = repr(error)
                stop.set()

        threads = [threading.Thread(target=run_bot, args=(n, b), name=n) for n, b in bots.items()]
        begin = time.monotonic()
        for t in threads:
            t.start()
        until = begin + config.seconds
        if view:
            book_view.run_curses(feed, config.symbol, until=until)
        while not stop.is_set() and time.monotonic() < until:
            stop.wait(min(1.0, max(until - time.monotonic(), 0.0)))
        stop.set()
        for t in threads:
            t.join(timeout=10)
        elapsed = time.monotonic() - begin
        problems = [f"{name} failed: {error}" for name, error in sorted(errors.items())]
        problems += [f"{t.name} did not stop" for t in threads if t.is_alive()]
        if problems:
            raise RuntimeError("; ".join(problems))

        # The bots are stopped, so this thread may use their clients now.
        for name, bot in bots.items():
            bot.handle_events(drain(clients[name]))
        fills = {name: bot.fills for name, bot in bots.items()}
        positions = {name: bot.position for name, bot in bots.items()}
        problems, matches, volume = check_matches(fills)
        problems += check_positions(positions)

        symbol = config.symbol
        # The feed may still be on its way, so wait until it printed at least as many
        # trades as there were matches, then demand exactly as many.
        wait_until(feed, lambda: feed.stats()["trades"] >= matches, 5)
        open_orders = {name: clients[name].open_orders() for name in names}
        want_bids = expected_levels(open_orders, litbook.Side.Buy)
        want_asks = expected_levels(open_orders, litbook.Side.Sell)
        if not wait_until(feed, lambda: feed_levels(feed, symbol, litbook.Side.Buy) == want_bids
                          and feed_levels(feed, symbol, litbook.Side.Sell) == want_asks, 5):
            problems.append("the feed's book differs from the bots' open orders")
        final_book = book_view.snapshot(feed, symbol)
        if feed.stats()["trades"] != matches:
            problems.append(f"the feed printed {feed.stats()['trades']} trades for "
                            f"{matches} matches")
        quote = feed.best(symbol)
        mark = config.fair_value
        if quote is not None and quote.bid is not None and quote.ask is not None:
            mark = (quote.bid.price + quote.ask.price) // 2

        for name in names:
            clients[name].logout()
        if not wait_until(feed, lambda: not feed_levels(feed, symbol, litbook.Side.Buy)
                          and not feed_levels(feed, symbol, litbook.Side.Sell), 5):
            problems.append("orders were left on the book after every bot logged out")

        rows = []
        for name, bot in bots.items():
            p = bot.position
            rows.append({"name": name, "shares": p.shares, "bought": p.bought, "sold": p.sold,
                         "fills": p.fills, "value": p.value(mark), "cash": p.cash})
        stats = feed.stats()
        if stats["gaps"] != 0:
            problems.append(f"the feed had {stats['gaps']} gaps")
        return Result(rows, matches, volume, stats["trades"], stats, mark, final_book,
                      problems, elapsed)
    finally:
        for client in clients.values():
            if client.active:
                client.logout()
        if gateway is not None:
            gateway.stop()
        feed.stop()


def summary(result):
    """The lines a run prints at the end."""
    lines = [f"ran {result.seconds:.1f} s: {result.matches} trades, {result.volume} shares, "
             f"feed trades {result.feed_trades}, mark {format_price(result.mark)}",
             f"{'BOT':<6} {'SHARES':>7} {'BOUGHT':>7} {'SOLD':>7} {'FILLS':>6} {'P&L AT MARK':>14}"]
    for row in result.bots:
        lines.append(f"{row['name']:<6} {row['shares']:>7} {row['bought']:>7} {row['sold']:>7} "
                     f"{row['fills']:>6} {format_money(row['value']):>14}")
    lines.append("")
    lines += result.final_book
    lines.append("")
    if result.problems:
        lines += ["CHECKS FAILED"] + [f"  {p}" for p in result.problems]
    else:
        lines.append("checks passed: every match seen by both sides, net position 0, "
                     "one feed trade per match, feed book equals open orders, empty after logout")
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run a small simulated market.")
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--makers", type=int, default=1)
    parser.add_argument("--noise", type=int, default=4)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--rate", type=float, default=5.0, help="actions per second per noise trader")
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument("--gateway", default=os.environ.get("LITBOOK_GATEWAY", ""),
                        help="path of litbook_gateway (default $LITBOOK_GATEWAY)")
    parser.add_argument("--view", action="store_true", help="show the live book while it runs")
    args = parser.parse_args(argv)
    if args.makers < 0 or args.noise < 0 or args.makers + args.noise == 0:
        parser.error("the market needs at least one bot")
    if args.makers + args.noise > 60:
        parser.error("at most 60 bots (the gateway takes 64 connections)")
    config = MarketConfig(seconds=args.seconds, makers=args.makers, noise=args.noise,
                          seed=args.seed, symbol=args.symbol, noise_rate=args.rate,
                          gateway=args.gateway)
    result = run_market(config, view=args.view)
    print("\n".join(summary(result)))
    return 1 if result.problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
