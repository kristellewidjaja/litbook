"""Tests of the simulated market's checks and of whole runs against a real gateway."""

import io
import os
import unittest
from contextlib import redirect_stderr, redirect_stdout

import litbook
from fakes import FakeOrder
from litbook_bots.position import Position
from litbook_bots.simulate import (MarketConfig, Result, check_matches, check_positions,
                                   expected_levels, main, run_market, summary)

BUY = litbook.Side.Buy
SELL = litbook.Side.Sell


class CheckMatchesTest(unittest.TestCase):
    def test_both_sides_of_each_match_count_once(self):
        fills = {"A": [(1, BUY, 100, 1000), (2, SELL, 5, 1100)],
                 "B": [(1, SELL, 100, 1000)], "C": [(2, BUY, 5, 1100)]}
        self.assertEqual(check_matches(fills), ([], 2, 105))

    def test_a_match_seen_once_or_three_times_is_a_problem(self):
        problems, matches, _ = check_matches({"A": [(1, BUY, 10, 1000)]})
        self.assertEqual((problems, matches), (["match 1 seen 1 times"], 1))
        problems, _, _ = check_matches({"A": [(1, BUY, 10, 1000)] * 2,
                                        "B": [(1, SELL, 10, 1000)]})
        self.assertEqual(problems, ["match 1 seen 3 times"])

    def test_sides_that_disagree_are_a_problem(self):
        for other in [(1, BUY, 10, 1000), (1, SELL, 9, 1000), (1, SELL, 10, 1100)]:
            problems, _, volume = check_matches({"A": [(1, BUY, 10, 1000)], "B": [other]})
            self.assertEqual(len(problems), 1, other)
            self.assertIn("sides disagree", problems[0])
            self.assertEqual(volume, 0)


class CheckPositionsTest(unittest.TestCase):
    def test_positions_must_add_up_to_zero(self):
        a, b = Position(), Position()
        a.apply_fill(BUY, 10, 1000)
        b.apply_fill(SELL, 10, 1000)
        self.assertEqual(check_positions({"A": a, "B": b}), [])
        b.apply_fill(SELL, 1, 1000)
        self.assertEqual(len(check_positions({"A": a, "B": b})), 2)
        c = Position(shares=0, cash=5)
        self.assertEqual(check_positions({"C": c}), ["net cash is 5, not 0"])


class ExpectedLevelsTest(unittest.TestCase):
    def test_open_orders_add_up_per_price_best_first(self):
        orders = {"A": {1: FakeOrder("AAPL", BUY, 1000, 10), 2: FakeOrder("AAPL", SELL, 1200, 5)},
                  "B": {7: FakeOrder("AAPL", BUY, 1000, 3), 8: FakeOrder("AAPL", BUY, 1100, 4),
                        9: FakeOrder("AAPL", SELL, 1300, 6)}}
        self.assertEqual(expected_levels(orders, BUY), [(1100, 4, 1), (1000, 13, 2)])
        self.assertEqual(expected_levels(orders, SELL), [(1200, 5, 1), (1300, 6, 1)])
        self.assertEqual(expected_levels({}, BUY), [])


class SummaryTest(unittest.TestCase):
    def result(self, problems):
        rows = [{"name": "MM1", "shares": 5, "bought": 10, "sold": 5, "fills": 2,
                 "value": 12_345, "cash": 0}]
        return Result(rows, 3, 15, 3, {}, 1_000_000, ["BOOK"], problems, 2.04)

    def test_summary_lists_bots_book_and_checks(self):
        lines = summary(self.result([]))
        self.assertEqual(lines[0], "ran 2.0 s: 3 trades, 15 shares, feed trades 3, mark 100.00")
        self.assertTrue(lines[2].startswith("MM1"))
        self.assertTrue(lines[2].endswith("$1.23"))
        self.assertIn("BOOK", lines)
        self.assertTrue(lines[-1].startswith("checks passed"))

    def test_summary_lists_failed_checks(self):
        lines = summary(self.result(["net position is 1 shares, not 0"]))
        self.assertIn("CHECKS FAILED", lines)
        self.assertEqual(lines[-1], "  net position is 1 shares, not 0")


class RunMarketTest(unittest.TestCase):
    """Whole runs. Each test has its own multicast groups (see test_feed_book.py)."""

    def test_a_short_market_trades_and_every_check_passes(self):
        config = MarketConfig(seconds=3, makers=1, noise=3, seed=2, noise_rate=20.0,
                              incremental="239.255.81.1", snapshot="239.255.81.2")
        result = run_market(config)
        self.assertEqual(result.problems, [])
        self.assertGreater(result.matches, 0)
        self.assertEqual(result.feed_trades, result.matches)
        self.assertEqual([row["name"] for row in result.bots], ["MM1", "NT1", "NT2", "NT3"])
        self.assertEqual(sum(row["shares"] for row in result.bots), 0)
        self.assertEqual(sum(row["value"] for row in result.bots), 0)
        self.assertEqual(sum(row["bought"] for row in result.bots), result.volume)
        self.assertEqual(sum(row["sold"] for row in result.bots), result.volume)
        self.assertGreater(result.bots[0]["fills"], 0, "the maker never traded")

    def test_two_makers_and_no_noise_quote_without_trading(self):
        # Both makers start from the same fair value and quote post only, so neither can
        # take the other's quote.
        config = MarketConfig(seconds=1, makers=2, noise=0, incremental="239.255.81.3",
                              snapshot="239.255.81.4")
        result = run_market(config)
        self.assertEqual(result.problems, [])
        self.assertEqual(result.matches, 0)
        self.assertNotIn("(empty book)", result.final_book)

    def test_without_a_gateway_binary_the_run_refuses(self):
        with self.assertRaises(RuntimeError):
            run_market(MarketConfig(gateway=""))


class MainTest(unittest.TestCase):
    def test_main_runs_a_market_and_prints_the_summary(self):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = main(["--seconds", "1", "--noise", "2", "--rate", "20", "--seed", "4",
                       "--gateway", os.environ["LITBOOK_GATEWAY"]])
        self.assertEqual(rc, 0, out.getvalue())
        self.assertIn("checks passed", out.getvalue())

    def test_bad_arguments_exit_with_usage(self):
        for args in (["--makers", "0", "--noise", "0"], ["--noise", "61"], ["--makers", "-1"]):
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                main(args)
            self.assertEqual(caught.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
