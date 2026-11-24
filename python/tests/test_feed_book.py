"""Tests of the litbook module's feed book against a real gateway's market data feed."""

import time
import unittest

import litbook
from gateway_process import GatewayProcess
from thread_check import counts_while

PRICE = 1_000_000


class FeedBookTest(unittest.TestCase):
    # Each test file uses its own multicast groups, and every receiver binds a port the
    # kernel picks, so tests running in parallel never hear each other.
    INCREMENTAL = "239.255.80.1"
    SNAPSHOT = "239.255.80.2"

    def setUp(self):
        self.book = litbook.FeedBook(incremental_address=self.INCREMENTAL, incremental_port=0,
                                     snapshot_address=self.SNAPSHOT, snapshot_port=0)
        self.addCleanup(self.book.stop)
        feed_args = ["--feed", "lo",
                     "--feed-incremental", f"{self.INCREMENTAL}:{self.book.incremental_port}",
                     "--feed-snapshot", f"{self.SNAPSHOT}:{self.book.snapshot_port}"]
        self.gateway = GatewayProcess(feed_args)
        self.addCleanup(self.gateway.stop)

    def client(self, user):
        c = litbook.OrderClient("127.0.0.1", self.gateway.port, user, "pw")
        self.addCleanup(c.logout)
        return c

    def wait_for(self, done, seconds=5):
        deadline = time.monotonic() + seconds
        seen = self.book.version
        while time.monotonic() < deadline:
            if done():
                return True
            seen = self.book.wait_update(seen, 50)
        return done()

    def test_the_book_follows_the_orders(self):
        self.assertTrue(self.wait_for(lambda: self.book.status("AAPL") == litbook.SymbolStatus.Live))
        self.assertEqual(self.book.symbols(), ["AAPL"])
        self.assertEqual(self.book.status("MSFT"), litbook.SymbolStatus.Unknown)
        self.assertIsNone(self.book.best("MSFT"))

        alice = self.client("ALICE")
        alice.enter("AAPL", litbook.Side.Buy, 100, PRICE)
        alice.enter("AAPL", litbook.Side.Buy, 200, PRICE - 100)
        alice.enter("AAPL", litbook.Side.Sell, 50, PRICE + 100)
        self.assertTrue(self.wait_for(lambda: len(self.book.depth("AAPL", litbook.Side.Buy)) == 2
                                      and self.book.best("AAPL").ask is not None))
        quote = self.book.best("AAPL")
        self.assertTrue(quote.live)
        self.assertEqual((quote.bid.price, quote.bid.qty, quote.bid.orders), (PRICE, 100, 1))
        self.assertEqual((quote.ask.price, quote.ask.qty), (PRICE + 100, 50))
        bids = self.book.depth("AAPL", litbook.Side.Buy, levels=5)
        self.assertEqual([level.price for level in bids], [PRICE, PRICE - 100])

        # A trade takes the best bid away.
        bob = self.client("BOB")
        bob.enter("AAPL", litbook.Side.Sell, 100, PRICE)
        self.assertTrue(self.wait_for(lambda: self.book.best("AAPL").bid.price == PRICE - 100))
        self.assertTrue(self.wait_for(lambda: self.book.stats()["trades"] == 1))
        stats = self.book.stats()
        self.assertTrue(stats["started"])
        self.assertEqual(stats["gaps"], 0)

    def test_end_of_session_is_seen_when_the_gateway_stops(self):
        self.assertTrue(self.wait_for(lambda: self.book.stats()["started"]))
        self.gateway.stop()
        self.assertTrue(self.wait_for(lambda: self.book.stats()["ended"]))

    def test_waiting_for_an_update_lets_other_threads_run(self):
        self.assertTrue(self.wait_for(lambda: self.book.stats()["started"]))
        # A version far ahead is never reached, so the call waits the whole 300 ms.
        far = self.book.version + 1_000_000
        during, baseline = counts_while(lambda: self.book.wait_update(far, 300), 0.3)
        self.assertGreater(during, baseline // 4)

    def test_bad_groups_are_refused(self):
        with self.assertRaises(ValueError):
            litbook.FeedBook(incremental_address="10.0.0.1", incremental_port=0)


if __name__ == "__main__":
    unittest.main()
