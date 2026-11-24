"""Tests of the terminal book view. The picture is checked as text, curses drawing with
a stand in screen, and --once as a separate process reading a real gateway's feed."""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

import litbook
from fakes import FakeClient, FakeFeed
from gateway_process import GatewayProcess
from litbook_bots.book_view import HEADER, draw, render_book, run_plain, snapshot

PRICE = 1_000_000


class RenderBookTest(unittest.TestCase):
    def test_bids_and_offers_side_by_side(self):
        lines = render_book("AAPL", [(PRICE, 300, 2), (PRICE - 100, 50, 1)],
                            [(PRICE + 200, 100, 1)], "Live", trades=7)
        self.assertEqual(lines[0], "AAPL  Live  spread 0.02  mid 100.01  trades 7")
        self.assertEqual(lines[1], HEADER)
        self.assertEqual(lines[2], "      2      300     100.00 | 100.02          100       1")
        self.assertEqual(lines[3], "      1       50      99.99 |                            ")
        self.assertEqual(len(lines), 4)
        # Every row lines up under the header's divider.
        for line in lines[1:]:
            self.assertEqual(line.index("|"), HEADER.index("|"))

    def test_an_empty_book_says_so(self):
        self.assertEqual(render_book("AAPL", [], [], "Unknown"),
                         ["AAPL  Unknown", HEADER, "(empty book)"])

    def test_one_sided_book_has_no_spread(self):
        lines = render_book("AAPL", [], [(PRICE, 10, 1)], "Stale")
        self.assertEqual(lines[0], "AAPL  Stale")
        self.assertEqual(lines[2].split("|")[0].strip(), "")
        self.assertIn("100.00", lines[2].split("|")[1])

    def test_levels_cut_the_depth(self):
        bids = [(PRICE - i * 100, 10, 1) for i in range(20)]
        self.assertEqual(len(render_book("AAPL", bids, [], levels=5)), 2 + 5)

    def test_snapshot_reads_a_feed(self):
        client = FakeClient()
        client.enter("AAPL", litbook.Side.Buy, 40, PRICE)
        feed = FakeFeed(client)
        feed.trades = 3
        lines = snapshot(feed, "AAPL")
        self.assertEqual(lines[0], "AAPL  Live  trades 3")
        self.assertIn("40     100.00", lines[2])


class FakeScreen:
    def __init__(self, height, width):
        self.size = (height, width)
        self.rows = {}
        self.refreshed = 0

    def getmaxyx(self):
        return self.size

    def erase(self):
        self.rows = {}

    def addstr(self, row, col, text):
        self.rows[row] = text

    def refresh(self):
        self.refreshed += 1


class DrawTest(unittest.TestCase):
    def test_lines_are_cut_to_the_screen_and_the_last_column_stays_free(self):
        screen = FakeScreen(3, 11)
        draw(screen, ["0123456789ABC", "short", "third", "fourth"])
        self.assertEqual(screen.rows, {0: "0123456789", 1: "short", 2: "third"})
        self.assertEqual(screen.refreshed, 1)


class RunPlainTest(unittest.TestCase):
    def test_prints_the_book_once_while_nothing_changes(self):
        out = []
        run_plain(FakeFeed(), "AAPL", 5, 0.3, out=out.append)
        self.assertEqual(out, ["AAPL  Live  trades 0", HEADER, "(empty book)", ""])


class BookViewProcessTest(unittest.TestCase):
    INCREMENTAL = "239.255.83.1"
    SNAPSHOT = "239.255.83.2"

    def test_once_prints_the_live_book_of_a_real_feed(self):
        # This process holds a receiver on ports the kernel picked. The view's own
        # receiver joins the same groups and ports, which SO_REUSEADDR allows.
        feed = litbook.FeedBook(incremental_address=self.INCREMENTAL, incremental_port=0,
                                snapshot_address=self.SNAPSHOT, snapshot_port=0)
        self.addCleanup(feed.stop)
        incremental = f"{self.INCREMENTAL}:{feed.incremental_port}"
        snapshot_group = f"{self.SNAPSHOT}:{feed.snapshot_port}"
        gateway = GatewayProcess(["--feed", "lo", "--feed-incremental", incremental,
                                  "--feed-snapshot", snapshot_group])
        self.addCleanup(gateway.stop)
        with litbook.OrderClient("127.0.0.1", gateway.port, "ALICE", "pw") as alice:
            alice.enter("AAPL", litbook.Side.Buy, 300, PRICE)
            alice.enter("AAPL", litbook.Side.Sell, 100, PRICE + 200)
            deadline = time.monotonic() + 5
            seen = feed.version
            while feed.depth("AAPL", litbook.Side.Sell) == [] and time.monotonic() < deadline:
                seen = feed.wait_update(seen, 50)
            # The new process learns the book from the snapshot stream.
            result = subprocess.run(
                [sys.executable, "-m", "litbook_bots.book_view", "--once",
                 "--incremental", incremental, "--snapshot", snapshot_group, "--wait", "10"],
                capture_output=True, text=True, timeout=30, env=os.environ)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(lines[0], "AAPL  Live  spread 0.02  mid 100.01  trades 0")
        self.assertEqual(lines[2], "      1      300     100.00 | 100.02          100       1")

    def test_once_follows_an_exchange_that_restarted_from_its_journal(self):
        # A restart names a new feed session after the journal's Restart record. The
        # gateway prints the name on its feed line, and --session tells the view which
        # session to read, so the view sees the book that recovery rebuilt.
        feed = litbook.FeedBook(incremental_address="239.255.84.1", incremental_port=0,
                                snapshot_address="239.255.84.2", snapshot_port=0)
        self.addCleanup(feed.stop)
        incremental = f"239.255.84.1:{feed.incremental_port}"
        snapshot_group = f"239.255.84.2:{feed.snapshot_port}"
        journal = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, journal, True)
        args = ["--feed", "lo", "--feed-incremental", incremental, "--feed-snapshot",
                snapshot_group, "--journal", journal]
        first = GatewayProcess(args)
        with litbook.OrderClient("127.0.0.1", first.port, "ALICE", "pw") as alice:
            alice.enter("AAPL", litbook.Side.Buy, 300, PRICE)
        self.assertEqual(first.stop(), 0)

        again = GatewayProcess(args)
        self.addCleanup(again.stop)
        feed_line = again.process.stdout.readline()
        match = re.search(r", session (\S+)$", feed_line.rstrip())
        self.assertIsNotNone(match, feed_line)
        self.assertNotEqual(match.group(1), "1")
        result = subprocess.run(
            [sys.executable, "-m", "litbook_bots.book_view", "--once",
             "--incremental", incremental, "--snapshot", snapshot_group,
             "--session", match.group(1), "--wait", "10"],
            capture_output=True, text=True, timeout=30, env=os.environ)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(lines[0], "AAPL  Live  trades 0")
        self.assertEqual(lines[2].rstrip(), "      1      300     100.00 |")


if __name__ == "__main__":
    unittest.main()
