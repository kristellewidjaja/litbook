"""Tests of the market maker. The decision functions get plain values, the loop gets the
fakes from fakes.py, and the last test runs the maker against a real gateway."""

import random
import threading
import time
import unittest

import litbook
from fakes import FakeClient, FakeFeed, FakeOrder
from gateway_process import GatewayProcess
from litbook_bots.market_maker import (Action, MakerConfig, MarketMaker, Quote, fair_value,
                                       others_best, own_levels, plan_side, target_quotes)

BUY = litbook.Side.Buy
SELL = litbook.Side.Sell
FAIR = 1_000_000
CONFIG = MakerConfig(fair_value=FAIR, half_spread=200, size=100, max_position=500, skew=1)


class OthersBestTest(unittest.TestCase):
    def test_own_shares_are_taken_out_of_each_level(self):
        levels = [(1000, 100), (900, 300)]
        self.assertEqual(others_best(levels, {}), 1000)
        self.assertEqual(others_best(levels, {1000: 100}), 900)
        self.assertEqual(others_best(levels, {1000: 60}), 1000)

    def test_a_side_with_only_the_maker_has_no_other_price(self):
        self.assertIsNone(others_best([(1000, 100)], {1000: 100}))
        self.assertIsNone(others_best([], {}))

    def test_own_shares_the_feed_has_not_shown_yet_do_not_count_as_others(self):
        # The maker just raised its order to 150 and the feed still shows 100.
        self.assertIsNone(others_best([(1000, 100)], {1000: 150}))

    def test_own_levels_adds_up_one_side(self):
        orders = {1: FakeOrder("AAPL", BUY, 1000, 30), 2: FakeOrder("AAPL", BUY, 1000, 20),
                  3: FakeOrder("AAPL", SELL, 1100, 50)}
        self.assertEqual(own_levels(orders, BUY), {1000: 50})
        self.assertEqual(own_levels(orders, SELL), {1100: 50})


class FairValueTest(unittest.TestCase):
    def test_middle_of_the_others_quotes(self):
        self.assertEqual(fair_value(FAIR, 999_900, 1_000_300), 1_000_100)
        self.assertEqual(fair_value(FAIR, 999_900, 1_000_000), 999_950)

    def test_keeps_the_previous_value_when_a_side_is_missing(self):
        self.assertEqual(fair_value(FAIR, None, 1_000_300), FAIR)
        self.assertEqual(fair_value(FAIR, 999_900, None), FAIR)
        self.assertEqual(fair_value(FAIR, None, None), FAIR)


class TargetQuotesTest(unittest.TestCase):
    def test_flat_maker_quotes_around_the_fair_value(self):
        bid, ask = target_quotes(FAIR, 0, CONFIG)
        self.assertEqual(bid, Quote(999_800, 100))
        self.assertEqual(ask, Quote(1_000_200, 100))

    def test_prices_round_away_from_the_center_onto_ticks(self):
        bid, ask = target_quotes(1_000_050, 0, CONFIG)
        self.assertEqual(bid.price, 999_800)
        self.assertEqual(ask.price, 1_000_300)

    def test_a_long_position_moves_both_quotes_down(self):
        bid, ask = target_quotes(FAIR, 300, CONFIG)
        self.assertEqual(bid.price, 999_500)
        self.assertEqual(ask.price, 999_900)
        bid, ask = target_quotes(FAIR, -300, CONFIG)
        self.assertEqual(bid.price, 1_000_100)
        self.assertEqual(ask.price, 1_000_500)

    def test_sizes_stop_at_the_position_limit(self):
        bid, ask = target_quotes(FAIR, 450, CONFIG)
        self.assertEqual(bid.qty, 50)
        self.assertEqual(ask.qty, 100)
        bid, ask = target_quotes(FAIR, 500, CONFIG)
        self.assertIsNone(bid)
        self.assertEqual(ask.qty, 100)
        bid, ask = target_quotes(FAIR, -500, CONFIG)
        self.assertEqual(bid.qty, 100)
        self.assertIsNone(ask)

    def test_quotes_stay_a_tick_off_the_others_best_prices(self):
        # A big short position pushes the bid up past the others' offer.
        bid, ask = target_quotes(FAIR, -400, CONFIG, other_bid=999_900, other_ask=1_000_100)
        self.assertEqual(bid.price, 1_000_000)
        bid, ask = target_quotes(FAIR, 400, CONFIG, other_bid=999_900, other_ask=1_000_100)
        self.assertEqual(ask.price, 1_000_000)

    def test_a_clamp_against_a_price_between_ticks_stays_on_a_tick(self):
        bid, ask = target_quotes(FAIR, -400, CONFIG, other_bid=999_950, other_ask=1_000_050)
        self.assertEqual(bid.price, 1_000_000)
        bid, ask = target_quotes(FAIR, 400, CONFIG, other_bid=999_950, other_ask=1_000_050)
        self.assertEqual(ask.price, 1_000_000)

    def test_no_bid_at_a_price_of_zero_or_less(self):
        bid, ask = target_quotes(100, 0, CONFIG)
        self.assertIsNone(bid)
        self.assertEqual(ask.price, 300)

    def test_the_bid_is_always_below_the_offer(self):
        rng = random.Random(5)
        for _ in range(5_000):
            fair = rng.randint(10_000, 2_000_000)
            shares = rng.randint(-499, 499)
            other_bid = rng.choice([None, fair - rng.randint(0, 50) * 100])
            other_ask = rng.choice([None, fair + rng.randint(1, 50) * 100])
            bid, ask = target_quotes(fair, shares, CONFIG, other_bid, other_ask)
            if bid is not None:
                self.assertEqual(bid.price % 100, 0)
                if other_ask is not None:
                    self.assertLess(bid.price, other_ask)
            if ask is not None:
                self.assertEqual(ask.price % 100, 0)
                if other_bid is not None:
                    self.assertGreater(ask.price, other_bid)
            if bid is not None and ask is not None:
                self.assertLess(bid.price, ask.price)

    def test_bad_configs_are_refused(self):
        for kwargs in ({"half_spread": 0}, {"size": 0}, {"max_position": 0}, {"skew": -1},
                       {"fair_value": 0}):
            with self.assertRaises(ValueError):
                MakerConfig(**kwargs)


class PlanSideTest(unittest.TestCase):
    def test_nothing_wanted_and_nothing_there(self):
        self.assertEqual(plan_side(None, None), Action("keep"))

    def test_an_order_that_is_no_longer_wanted_is_canceled(self):
        self.assertEqual(plan_side(FakeOrder("AAPL", BUY, 1000, 100), None), Action("cancel"))

    def test_a_missing_order_is_entered(self):
        self.assertEqual(plan_side(None, Quote(1000, 100)), Action("enter", 1000, 100))

    def test_the_same_order_is_kept(self):
        self.assertEqual(plan_side(FakeOrder("AAPL", BUY, 1000, 100), Quote(1000, 100)),
                         Action("keep"))

    def test_a_new_price_is_a_replace_whose_size_counts_the_executed_shares(self):
        current = FakeOrder("AAPL", BUY, 1000, 70, executed=30)
        self.assertEqual(plan_side(current, Quote(900, 100)), Action("replace", 900, 130))

    def test_small_fills_do_not_cause_a_message_but_big_ones_refill(self):
        self.assertEqual(plan_side(FakeOrder("AAPL", BUY, 1000, 50, 50), Quote(1000, 100)),
                         Action("keep"))
        self.assertEqual(plan_side(FakeOrder("AAPL", BUY, 1000, 49, 51), Quote(1000, 100)),
                         Action("replace", 1000, 151))

    def test_an_order_bigger_than_the_target_is_cut(self):
        self.assertEqual(plan_side(FakeOrder("AAPL", BUY, 1000, 100), Quote(1000, 40)),
                         Action("replace", 1000, 40))


class MarketMakerLoopTest(unittest.TestCase):
    def setUp(self):
        self.client = FakeClient()
        self.feed = FakeFeed(self.client)
        self.maker = MarketMaker(self.client, self.feed, CONFIG)

    def quotes(self):
        """The maker's open orders as {side: (price, open)}."""
        return {o.side: (o.price, o.open) for o in self.client.open_orders().values()}

    def test_first_step_enters_two_post_only_quotes(self):
        self.maker.step()
        self.assertEqual(self.quotes(), {BUY: (999_800, 100), SELL: (1_000_200, 100)})
        self.assertTrue(all(entry[6] for entry in self.client.sent("enter")))
        self.maker.step()
        self.assertEqual(len(self.client.log), 2)  # Nothing changed, so nothing was sent.

    def test_no_quotes_until_the_feed_is_live_and_none_once_it_is_stale(self):
        self.feed.state = litbook.SymbolStatus.Unknown
        self.maker.step()
        self.assertEqual(self.client.log, [])
        self.feed.state = litbook.SymbolStatus.Live
        self.maker.step()
        self.assertEqual(len(self.quotes()), 2)
        self.feed.state = litbook.SymbolStatus.Stale
        self.maker.step()
        self.assertEqual(self.quotes(), {})
        self.assertEqual(len(self.client.sent("cancel")), 2)

    def test_a_full_fill_moves_the_position_and_both_quotes(self):
        self.maker.step()
        bid_ref = next(r for r, o in self.client.orders.items() if o.side == BUY)
        self.client.fill(bid_ref, 100)
        self.maker.step()
        self.assertEqual(self.maker.position.shares, 100)
        self.assertEqual(self.maker.position.cash, -100 * 999_800)
        self.assertEqual(self.maker.fills, [(1, BUY, 100, 999_800)])
        # The center moved down by 100 units, one cent.
        self.assertEqual(self.quotes(), {BUY: (999_700, 100), SELL: (1_000_100, 100)})
        self.assertEqual(len(self.client.sent("replace")), 1)  # the offer
        self.assertEqual(len(self.client.sent("enter")), 3)  # two first quotes and a new bid

    def test_a_partial_fill_is_replaced_with_the_executed_shares_counted(self):
        self.maker.step()
        ask_ref = next(r for r, o in self.client.orders.items() if o.side == SELL)
        self.client.fill(ask_ref, 60)
        self.maker.step()
        self.assertEqual(self.maker.position.shares, -60)
        replace = self.client.sent("replace")[-1]
        self.assertEqual(replace[1], ask_ref)
        self.assertEqual(replace[3], 160)  # 60 executed and 100 to show
        self.assertEqual(self.quotes()[SELL], (1_000_300, 100))

    def test_at_the_position_limit_the_bid_is_pulled(self):
        maker = MarketMaker(self.client, self.feed, MakerConfig(fair_value=FAIR, size=100,
                                                                max_position=100))
        maker.step()
        bid_ref = next(r for r, o in self.client.orders.items() if o.side == BUY)
        self.client.fill(bid_ref, 100)
        maker.step()
        self.assertEqual(maker.position.shares, 100)
        self.assertNotIn(BUY, self.quotes())
        self.assertEqual(self.quotes()[SELL][1], 100)

    def test_fair_value_follows_other_traders_but_not_the_makers_own_quotes(self):
        self.feed.other_bids = [(999_900, 500)]
        self.feed.other_asks = [(1_000_300, 500)]
        self.maker.step()
        self.assertEqual(self.maker.fair, 1_000_100)
        # With the maker's own (skewed) quotes on the feed, more steps change nothing.
        for _ in range(3):
            self.maker.step()
        self.assertEqual(self.maker.fair, 1_000_100)
        self.assertEqual(self.quotes(), {BUY: (999_900, 100), SELL: (1_000_300, 100)})

    def test_a_replace_that_finds_the_order_filled_enters_a_new_one(self):
        self.maker.step()
        self.feed.other_bids = [(999_900, 100)]
        self.feed.other_asks = [(1_000_500, 100)]  # Fair value moves up to 1_000_200.
        self.client.fill_on_replace = True
        self.maker.step()
        self.client.fill_on_replace = False
        self.maker.step()
        self.assertEqual(len(self.maker.fills), 2)
        self.assertEqual(self.maker.position.shares, 0)
        self.assertEqual(len(self.quotes()), 2)

    def test_a_refused_replace_counts_a_reject_and_the_next_step_enters_again(self):
        self.maker.step()
        self.feed.other_bids = [(999_900, 100)]
        self.feed.other_asks = [(1_000_500, 100)]
        self.client.reject_replaces = True
        self.maker.step()
        self.assertEqual(self.maker.rejects, 2)
        self.assertEqual(self.quotes(), {})
        self.client.reject_replaces = False
        self.maker.step()
        self.assertEqual(len(self.quotes()), 2)

    def test_a_refused_enter_is_counted(self):
        self.client.reject_enters = True
        self.maker.step()
        self.assertEqual(self.maker.rejects, 2)
        self.assertEqual(self.client.open_orders(), {})
        self.maker.step()
        self.assertEqual(self.maker.rejects, 4)  # It tries again on the next step.

    def test_cancel_all_and_run_until_stopped(self):
        stop = threading.Event()
        stop.set()
        self.maker.run(stop)
        self.assertEqual(self.client.log, [])
        self.maker.step()
        self.maker.cancel_all()
        self.assertEqual(self.quotes(), {})


class MarketMakerAgainstTheGatewayTest(unittest.TestCase):
    INCREMENTAL = "239.255.82.1"
    SNAPSHOT = "239.255.82.2"

    def setUp(self):
        self.feed = litbook.FeedBook(incremental_address=self.INCREMENTAL, incremental_port=0,
                                     snapshot_address=self.SNAPSHOT, snapshot_port=0)
        self.addCleanup(self.feed.stop)
        self.gateway = GatewayProcess(
            ["--feed", "lo", "--feed-incremental", f"{self.INCREMENTAL}:{self.feed.incremental_port}",
             "--feed-snapshot", f"{self.SNAPSHOT}:{self.feed.snapshot_port}"])
        self.addCleanup(self.gateway.stop)

    def client(self, user):
        c = litbook.OrderClient("127.0.0.1", self.gateway.port, user, "pw")
        self.addCleanup(c.logout)
        return c

    def step_until(self, maker, done, seconds=5):
        deadline = time.monotonic() + seconds
        seen = self.feed.version
        while time.monotonic() < deadline:
            maker.step()
            if done():
                return True
            seen = self.feed.wait_update(seen, 50)
        return done()

    def best(self):
        quote = self.feed.best("AAPL")
        if quote is None or quote.bid is None or quote.ask is None:
            return None
        return quote.bid.price, quote.ask.price

    def test_the_maker_quotes_and_leans_against_a_fill(self):
        maker = MarketMaker(self.client("ALICE"), self.feed, CONFIG)
        self.assertTrue(self.step_until(maker, lambda: self.best() == (999_800, 1_000_200)))
        # Another trader buys 100 shares from the maker's offer.
        bob = self.client("BOB")
        taken = bob.enter("AAPL", BUY, 100, 1_000_200,
                          tif=litbook.TimeInForce.ImmediateOrCancel)
        self.assertEqual(type(taken).__name__, "Accepted")
        self.assertTrue(self.step_until(maker, lambda: maker.position.shares == -100
                                        and self.best() == (999_900, 1_000_300)))
        self.assertEqual(maker.position.cash, 100 * 1_000_200)
        self.assertEqual(len(maker.fills), 1)
        self.assertEqual(maker.rejects, 0)
        maker.cancel_all()
        self.assertEqual(maker.client.open_orders(), {})
        deadline = time.monotonic() + 5
        seen = self.feed.version
        while self.feed.depth("AAPL", BUY) or self.feed.depth("AAPL", SELL):
            self.assertLess(time.monotonic(), deadline, "the quotes stayed on the feed")
            seen = self.feed.wait_update(seen, 50)


if __name__ == "__main__":
    unittest.main()
