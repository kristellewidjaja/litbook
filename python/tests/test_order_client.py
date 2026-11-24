"""Tests of the litbook module's blocking order client against a real gateway."""

import time
import unittest

import litbook
from gateway_process import GatewayProcess
from thread_check import counts_while

PRICE = 1_000_000  # $100.00 in ticks with 4 implied decimals.


class OrderClientTest(unittest.TestCase):
    def setUp(self):
        self.gateway = GatewayProcess()
        self.addCleanup(self.gateway.stop)

    def client(self, user="ALICE", **kwargs):
        c = litbook.OrderClient("127.0.0.1", self.gateway.port, user, "pw", **kwargs)
        self.addCleanup(c.logout)
        return c

    def wait_events(self, client, kind, count):
        """Reads events until `count` of type `kind` arrived or two seconds pass."""
        got = []
        deadline = time.monotonic() + 2
        while len(got) < count and time.monotonic() < deadline:
            got += [e for e in client.events(timeout_ms=50) if isinstance(e, kind)]
        return got

    def test_enter_and_cancel(self):
        alice = self.client()
        self.assertTrue(alice.active)
        accepted = alice.enter("AAPL", litbook.Side.Buy, 100, PRICE)
        self.assertIsInstance(accepted, litbook.Accepted)
        self.assertEqual(accepted.user_ref, 1)
        self.assertEqual(accepted.symbol, "AAPL")
        self.assertEqual(accepted.qty, 100)
        self.assertEqual(accepted.price, PRICE)
        self.assertEqual(alice.order(1).open, 100)
        self.assertEqual(list(alice.open_orders()), [1])
        self.assertEqual(alice.next_ref, 2)

        canceled = alice.cancel(1)
        self.assertIsInstance(canceled, litbook.Canceled)
        self.assertEqual(canceled.qty, 100)
        self.assertEqual(canceled.reason, litbook.CancelReason.UserRequested)
        self.assertIsNone(alice.order(1))
        self.assertEqual(alice.open_orders(), {})
        # Nothing is open any more, so a second cancel sends nothing and returns None.
        self.assertIsNone(alice.cancel(1))

    def test_a_trade_reaches_both_sides(self):
        alice = self.client("ALICE")
        bob = self.client("BOB")
        alice.enter("AAPL", litbook.Side.Sell, 100, PRICE)
        taken = bob.enter("AAPL", litbook.Side.Buy, 60, PRICE,
                          tif=litbook.TimeInForce.ImmediateOrCancel)
        self.assertIsInstance(taken, litbook.Accepted)
        bob_fills = self.wait_events(bob, litbook.Executed, 1)
        alice_fills = self.wait_events(alice, litbook.Executed, 1)
        self.assertEqual(len(bob_fills), 1)
        self.assertEqual(len(alice_fills), 1)
        self.assertEqual(bob_fills[0].qty, 60)
        self.assertEqual(bob_fills[0].match, alice_fills[0].match)
        self.assertEqual(alice_fills[0].liquidity, litbook.Liquidity.Added)
        self.assertEqual(alice.order(1).open, 40)
        self.assertEqual(alice.order(1).executed, 60)

    def test_cancel_and_replace_of_a_filled_order_return_none(self):
        alice = self.client("ALICE", timeout_ms=10_000)
        bob = self.client("BOB")
        alice.enter("AAPL", litbook.Side.Sell, 100, PRICE)
        alice.enter("AAPL", litbook.Side.Sell, 100, PRICE)
        bob.enter("AAPL", litbook.Side.Buy, 200, PRICE)
        self.assertEqual(len(self.wait_events(bob, litbook.Executed, 2)), 2)
        # Alice has not read her fills yet, and still gets an answer at once.
        begin = time.monotonic()
        self.assertIsNone(alice.cancel(1))
        self.assertIsNone(alice.replace(2, 100, PRICE + 100))
        self.assertLess(time.monotonic() - begin, 5)
        self.assertEqual(len(self.wait_events(alice, litbook.Executed, 2)), 2)

    def test_replace(self):
        alice = self.client()
        alice.enter("AAPL", litbook.Side.Buy, 100, PRICE)
        replaced = alice.replace(1, 150, PRICE - 100)
        self.assertIsInstance(replaced, litbook.Replaced)
        self.assertEqual(replaced.orig_user_ref, 1)
        self.assertEqual(replaced.user_ref, 2)
        self.assertEqual(replaced.qty, 150)
        self.assertIsNone(alice.order(1))
        self.assertEqual(alice.order(2).price, PRICE - 100)

    def test_rejects(self):
        alice = self.client()
        rejected = alice.enter("MSFT", litbook.Side.Buy, 10, PRICE)
        self.assertIsInstance(rejected, litbook.Rejected)
        self.assertEqual(rejected.reason, litbook.RejectReason.InvalidSymbol)
        rejected = alice.enter("AAPL", litbook.Side.Buy, 10, PRICE + 1)
        self.assertEqual(rejected.reason, litbook.RejectReason.InvalidPrice)

    def test_values_that_cannot_be_sent_raise(self):
        alice = self.client()
        with self.assertRaises(ValueError):
            alice.enter("NOT A TICKER", litbook.Side.Buy, 10, PRICE)
        with self.assertRaises(ValueError):
            alice.enter("AAPL", litbook.Side.Buy, 10, -100)
        # Shares and prices are integers. A float or a negative quantity is refused.
        with self.assertRaises(TypeError):
            alice.enter("AAPL", litbook.Side.Buy, 10, 100.5)
        with self.assertRaises(TypeError):
            alice.enter("AAPL", litbook.Side.Buy, -10, PRICE)
        with self.assertRaises(TypeError):
            alice.enter("AAPL", litbook.Side.Buy, 10, 2**64)
        self.assertEqual(alice.next_ref, 1)

    def test_login_failures_raise(self):
        with self.assertRaises(RuntimeError):
            litbook.OrderClient("127.0.0.1", self.gateway.port, "ALICE", "wrong")
        with self.assertRaises(ValueError):
            litbook.OrderClient("127.0.0.1", self.gateway.port, "TOOLONGNAME", "pw")

    def test_an_ignored_enter_times_out(self):
        with self.client() as first:
            first.enter("AAPL", litbook.Side.Buy, 10, PRICE)
        self.assertFalse(first.active)
        # A new connection that starts its numbers from 1 again is ignored by the exchange.
        again = self.client(timeout_ms=200)
        with self.assertRaises(TimeoutError):
            again.enter("AAPL", litbook.Side.Buy, 10, PRICE)

    def test_calls_raise_once_the_gateway_is_gone(self):
        alice = self.client(timeout_ms=2000)
        alice.enter("AAPL", litbook.Side.Buy, 10, PRICE)
        self.gateway.stop()
        with self.assertRaises(ConnectionError):
            alice.enter("AAPL", litbook.Side.Buy, 10, PRICE)
        self.assertFalse(alice.active)

    def test_a_blocking_call_lets_other_threads_run(self):
        alice = self.client()
        # Nothing arrives, so events waits the whole 300 ms.
        during, baseline = counts_while(lambda: alice.events(timeout_ms=300), 0.3)
        # Holding the interpreter lock would let the other thread run for at most one
        # switch interval (5 ms) of the 300.
        self.assertGreater(during, baseline // 4)

if __name__ == "__main__":
    unittest.main()
