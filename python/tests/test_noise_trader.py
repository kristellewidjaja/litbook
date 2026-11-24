"""Tests of the noise trader's random choices and of its loop with the fakes."""

import random
import threading
import time
import unittest

import litbook
from fakes import Canceled, FakeClient, FakeFeed
from litbook_bots.noise_trader import (NoiseConfig, NoiseTrader, choose_action, middle,
                                       next_delay)

BUY = litbook.Side.Buy
SELL = litbook.Side.Sell
IOC = litbook.TimeInForce.ImmediateOrCancel
GTC = litbook.TimeInForce.GoodTillCancel
CONFIG = NoiseConfig(fair_value=1_000_000)
BID = 999_900
ASK = 1_000_100


def draws(n, seed=3, config=CONFIG, bid=BID, ask=ASK, open_refs=()):
    rng = random.Random(seed)
    return [choose_action(rng, config, bid, ask, open_refs) for _ in range(n)]


class MiddleTest(unittest.TestCase):
    def test_middle_falls_back_to_one_side_then_to_the_fair_value(self):
        self.assertEqual(middle(BID, ASK, 5), 1_000_000)
        self.assertEqual(middle(BID, None, 5), BID)
        self.assertEqual(middle(None, ASK, 5), ASK)
        self.assertEqual(middle(None, None, 5), 5)


class ChooseActionTest(unittest.TestCase):
    def test_the_same_seed_gives_the_same_actions(self):
        self.assertEqual(draws(200, seed=11, open_refs=[1, 2]),
                         draws(200, seed=11, open_refs=[1, 2]))
        self.assertNotEqual(draws(200, seed=11), draws(200, seed=12))

    def test_kinds_follow_the_weights(self):
        actions = draws(10_000, open_refs=[1])
        counts = {k: sum(a.kind == k for a in actions) for k in ("take", "rest", "cancel")}
        # Weights 3, 5 and 2 out of 10. The seed is fixed, so this cannot be flaky.
        self.assertTrue(2_700 < counts["take"] < 3_300, counts)
        self.assertTrue(4_700 < counts["rest"] < 5_300, counts)
        self.assertTrue(1_700 < counts["cancel"] < 2_300, counts)
        sides = sum(a.side == BUY for a in actions if a.kind != "cancel")
        self.assertTrue(0.45 < sides / (10_000 - counts["cancel"]) < 0.55)

    def test_nothing_open_means_no_cancel(self):
        self.assertFalse(any(a.kind == "cancel" for a in draws(2_000)))

    def test_at_the_open_order_limit_every_action_is_a_cancel_of_an_open_order(self):
        refs = list(range(10, 20))
        actions = draws(500, open_refs=refs)
        self.assertTrue(all(a.kind == "cancel" and a.ref in refs for a in actions))
        self.assertEqual({a.ref for a in actions}, set(refs))

    def test_takes_go_through_the_other_sides_best_price(self):
        for a in draws(2_000):
            if a.kind == "take":
                self.assertEqual(a.price, ASK + 200 if a.side == BUY else BID - 200)

    def test_takes_without_another_side_aim_through_the_middle(self):
        for a in draws(500, bid=None, ask=None):
            if a.kind == "take":
                self.assertEqual(a.price, 1_000_200 if a.side == BUY else 999_800)

    def test_resting_orders_sit_behind_the_middle_on_ticks_and_never_cross(self):
        # A one tick spread: the middle is half way between two ticks.
        bid, ask = 1_000_000, 1_000_100
        seen = set()
        for a in draws(3_000, bid=bid, ask=ask):
            if a.kind != "rest":
                continue
            self.assertEqual(a.price % 100, 0)
            if a.side == BUY:
                self.assertTrue(bid - 500 <= a.price <= bid, a)
            else:
                self.assertTrue(ask <= a.price <= ask + 500, a)
            seen.add((a.side, a.price))
        self.assertEqual(len(seen), 12)  # 6 prices a side, 0 to 5 ticks behind.

    def test_sizes_cover_one_to_max_qty(self):
        sizes = [a.qty for a in draws(5_000) if a.kind != "cancel"]
        self.assertEqual(min(sizes), 1)
        self.assertEqual(max(sizes), CONFIG.max_qty)

    def test_prices_never_go_below_one_tick(self):
        config = NoiseConfig(fair_value=100)
        for a in draws(1_000, config=config, bid=None, ask=None):
            self.assertGreaterEqual(a.price, 100)

    def test_delays_average_one_over_the_rate_and_are_capped(self):
        rng = random.Random(1)
        delays = [next_delay(rng, CONFIG) for _ in range(20_000)]
        self.assertLessEqual(max(delays), CONFIG.max_delay)
        self.assertTrue(0.19 < sum(delays) / len(delays) < 0.21)
        slow = NoiseConfig(rate=0.1, max_delay=1.0)
        self.assertEqual(max(next_delay(rng, slow) for _ in range(100)), 1.0)

    def test_bad_configs_are_refused(self):
        for kwargs in ({"rate": 0}, {"max_qty": 0}, {"max_open": 0}, {"cancel_weight": -1},
                       {"take_weight": 0, "rest_weight": 0}):
            with self.assertRaises(ValueError):
                NoiseConfig(**kwargs)


class NoiseTraderLoopTest(unittest.TestCase):
    def setUp(self):
        self.client = FakeClient()
        self.feed = FakeFeed(self.client)
        self.feed.other_bids = [(BID, 100)]
        self.feed.other_asks = [(ASK, 100)]

    def test_actions_are_sent_with_the_right_time_in_force(self):
        trader = NoiseTrader(self.client, self.feed, CONFIG, seed=4)
        actions = [trader.act() for _ in range(200)]
        enters = self.client.sent("enter")
        self.assertEqual(len(enters), sum(a.kind != "cancel" for a in actions))
        for entry, action in zip(enters, [a for a in actions if a.kind != "cancel"]):
            self.assertEqual(entry[5], IOC if action.kind == "take" else GTC)
            self.assertEqual((entry[2], entry[3], entry[4]), (action.side, action.qty, action.price))
            self.assertFalse(entry[6])
        self.assertEqual(sum(trader.counts.values()), 200)
        self.assertLessEqual(len(self.client.orders), CONFIG.max_open)
        self.assertEqual(len(self.client.sent("cancel")), trader.counts["cancel"])

    def test_the_same_seed_and_book_send_the_same_orders(self):
        other = FakeClient()
        other_feed = FakeFeed(other)
        other_feed.other_bids, other_feed.other_asks = self.feed.other_bids, self.feed.other_asks
        a = NoiseTrader(self.client, self.feed, CONFIG, seed=9)
        b = NoiseTrader(other, other_feed, CONFIG, seed=9)
        for _ in range(100):
            a.act()
            b.act()
        self.assertEqual(self.client.log, other.log)

    def test_fills_and_self_match_cancels_are_counted(self):
        trader = NoiseTrader(self.client, self.feed, NoiseConfig(take_weight=0), seed=1)
        trader.act()
        (ref, order), = self.client.orders.items()
        self.client.fill(ref, 1)
        self.client.pending.append(Canceled(ref, order.open,
                                            litbook.CancelReason.SelfMatchPrevention))
        trader.handle_events(self.client.events())
        self.assertEqual(trader.position.fills, 1)
        self.assertEqual(trader.position.shares, 1 if order.side == BUY else -1)
        self.assertEqual(trader.self_match_cancels, 1)
        self.assertEqual(trader.fills, [(1, order.side, 1, order.price)])

    def test_a_refused_enter_is_counted(self):
        self.client.reject_enters = True
        trader = NoiseTrader(self.client, self.feed, CONFIG, seed=2)
        trader.act()
        self.assertEqual(trader.rejects, 1)
        self.assertEqual(trader.side_of, {})

    def test_an_empty_book_uses_the_fair_value(self):
        feed = FakeFeed(None, status=litbook.SymbolStatus.Unknown)
        trader = NoiseTrader(self.client, feed, NoiseConfig(take_weight=0), seed=2)
        action = trader.act()
        self.assertTrue(abs(action.price - 1_000_000) <= 500)

    def test_run_acts_at_about_the_rate_and_stops_quickly(self):
        trader = NoiseTrader(self.client, self.feed, NoiseConfig(rate=50.0), seed=6)
        stop = threading.Event()
        thread = threading.Thread(target=trader.run, args=(stop,))
        thread.start()
        time.sleep(1.0)
        stop.set()
        begin = time.monotonic()
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        self.assertLess(time.monotonic() - begin, 0.5)
        # About 50 actions in a second. The bounds are wide because the timing is real.
        self.assertTrue(15 <= sum(trader.counts.values()) <= 80, trader.counts)


if __name__ == "__main__":
    unittest.main()
