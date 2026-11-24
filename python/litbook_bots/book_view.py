"""A live terminal view of one stock's book, read from the market data feed.

render_book turns the levels into lines of text and does nothing else, so the tests
can check the picture without a terminal. The curses loop redraws those lines each
time the feed book's version moves, and the plain mode prints them instead, for pipes
and for tests.

    python3 -m litbook_bots.book_view --symbol AAPL             live view, q quits
    python3 -m litbook_bots.book_view --symbol AAPL --once      print one picture
"""

import argparse
import curses
import time

import litbook

from .prices import format_price

HEADER = f"{'ORDERS':>7} {'QTY':>8} {'BID':>10} | {'ASK':<10} {'QTY':>8} {'ORDERS':>7}"


def _side_cells(level):
    """The orders, qty and price text of one level, or blanks for a missing level."""
    if level is None:
        return "", "", ""
    price, qty, orders = level
    return str(orders), str(qty), format_price(price)


def render_book(symbol, bids, asks, status="Live", trades=None, levels=10):
    """The picture of one book as a list of lines.

    bids and asks are (price, qty, orders) tuples, best first. Row i shows the i-th
    best bid on the left and the i-th best offer on the right, like most trading
    screens. The title line says whether the copy is live, the spread and the middle.
    """
    bids = list(bids)[:levels]
    asks = list(asks)[:levels]
    title = f"{symbol}  {status}"
    if bids and asks:
        spread = asks[0][0] - bids[0][0]
        mid = (bids[0][0] + asks[0][0]) // 2
        title += f"  spread {format_price(spread)}  mid {format_price(mid)}"
    if trades is not None:
        title += f"  trades {trades}"
    lines = [title, HEADER]
    if not bids and not asks:
        lines.append("(empty book)")
        return lines
    for i in range(max(len(bids), len(asks))):
        b_orders, b_qty, b_price = _side_cells(bids[i] if i < len(bids) else None)
        a_orders, a_qty, a_price = _side_cells(asks[i] if i < len(asks) else None)
        lines.append(f"{b_orders:>7} {b_qty:>8} {b_price:>10} | "
                     f"{a_price:<10} {a_qty:>8} {a_orders:>7}")
    return lines


def snapshot(feed, symbol, levels=10):
    """Reads one book from a FeedBook and renders it."""
    status = feed.status(symbol)
    bids = [(lv.price, lv.qty, lv.orders) for lv in feed.depth(symbol, litbook.Side.Buy, levels)]
    asks = [(lv.price, lv.qty, lv.orders) for lv in feed.depth(symbol, litbook.Side.Sell, levels)]
    return render_book(symbol, bids, asks, status.name, feed.stats()["trades"], levels)


def draw(screen, lines):
    """Puts the lines on a curses screen, cut to its size. Writing into the bottom
    right cell raises curses.error, so the last column is left free."""
    height, width = screen.getmaxyx()
    screen.erase()
    for row, line in enumerate(lines[:height]):
        screen.addstr(row, 0, line[:max(width - 1, 0)])
    screen.refresh()


def wait_live(feed, symbol, seconds):
    """Waits until the stock is live on the feed, or the time is up."""
    deadline = time.monotonic() + seconds
    seen = feed.version
    while feed.status(symbol) != litbook.SymbolStatus.Live and time.monotonic() < deadline:
        seen = feed.wait_update(seen, 50)
    return feed.status(symbol) == litbook.SymbolStatus.Live


def run_curses(feed, symbol, levels=10, until=None):
    """The live view. Returns when q is pressed or at the monotonic time until."""

    def loop(screen):
        screen.nodelay(True)
        curses.curs_set(0)
        seen = -1
        while until is None or time.monotonic() < until:
            if screen.getch() in (ord("q"), ord("Q")):
                return
            if feed.version != seen:
                seen = feed.version
                draw(screen, snapshot(feed, symbol, levels) + ["", "q quits"])
            feed.wait_update(seen, 100)

    curses.wrapper(loop)


def run_plain(feed, symbol, levels, seconds, out=print):
    """Prints the book after each change for the given time, at most ten times a second."""
    deadline = time.monotonic() + seconds
    seen = -1
    while time.monotonic() < deadline:
        if feed.version != seen:
            seen = feed.version
            for line in snapshot(feed, symbol, levels):
                out(line)
            out("")
        time.sleep(0.1)
        feed.wait_update(seen, 100)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Show one stock's book from the feed.")
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument("--levels", type=int, default=10)
    parser.add_argument("--interface", default="lo")
    parser.add_argument("--incremental", default="239.255.0.1:15100")
    parser.add_argument("--snapshot", default="239.255.0.2:15101")
    parser.add_argument("--session", default="1",
                        help="the feed session, as litbook_gateway prints it on its feed line")
    parser.add_argument("--once", action="store_true",
                        help="wait until the stock is live, print one picture and exit")
    parser.add_argument("--plain", type=float, metavar="SECONDS",
                        help="print the book after every change for this long, no curses")
    parser.add_argument("--wait", type=float, default=10.0,
                        help="seconds --once waits for the stock to be live")
    args = parser.parse_args(argv)
    inc_addr, inc_port = args.incremental.rsplit(":", 1)
    snap_addr, snap_port = args.snapshot.rsplit(":", 1)
    with litbook.FeedBook(args.interface, inc_addr, int(inc_port), snap_addr,
                          int(snap_port), args.session) as feed:
        if args.once:
            live = wait_live(feed, args.symbol, args.wait)
            print("\n".join(snapshot(feed, args.symbol, args.levels)))
            return 0 if live else 1
        if args.plain is not None:
            run_plain(feed, args.symbol, args.levels, args.plain)
            return 0
        try:
            run_curses(feed, args.symbol, args.levels)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
