"""Tests of the integer price helpers and of Position."""

import unittest

import litbook
from litbook_bots.position import Position
from litbook_bots.prices import ceil_to_tick, floor_to_tick, format_money, format_price
from litbook_bots.replies import kind


class TickTest(unittest.TestCase):
    def test_rounding_to_ticks(self):
        self.assertEqual(floor_to_tick(1_000_050), 1_000_000)
        self.assertEqual(ceil_to_tick(1_000_050), 1_000_100)
        self.assertEqual(floor_to_tick(1_000_000), 1_000_000)
        self.assertEqual(ceil_to_tick(1_000_000), 1_000_000)
        self.assertEqual(floor_to_tick(-50), -100)
        self.assertEqual(ceil_to_tick(-50), 0)
        self.assertEqual(floor_to_tick(1_234, tick=500), 1_000)
        self.assertEqual(ceil_to_tick(1_234, tick=500), 1_500)

    def test_prices_print_as_dollars(self):
        self.assertEqual(format_price(1_000_000), "100.00")
        self.assertEqual(format_price(1_000_100), "100.01")
        self.assertEqual(format_price(1_000_050), "100.0050")
        self.assertEqual(format_price(100), "0.01")
        self.assertEqual(format_price(0), "0.00")
        self.assertEqual(format_price(-100), "-0.01")

    def test_money_prints_with_cents_and_commas(self):
        self.assertEqual(format_money(123_456_700), "$12,345.67")
        self.assertEqual(format_money(-150), "-$0.01")
        self.assertEqual(format_money(99), "$0.00")
        self.assertEqual(format_money(0), "$0.00")


class PositionTest(unittest.TestCase):
    def test_fills_move_shares_and_cash(self):
        p = Position()
        p.apply_fill(litbook.Side.Buy, 100, 1_000_000)
        p.apply_fill(litbook.Side.Sell, 40, 1_000_200)
        self.assertEqual(p.shares, 60)
        self.assertEqual(p.cash, -100 * 1_000_000 + 40 * 1_000_200)
        self.assertEqual((p.bought, p.sold, p.fills), (100, 40, 2))
        # Selling the other 60 at 100.00 would leave the 0.02 earned on 40 shares.
        self.assertEqual(p.value(1_000_000), 40 * 200)

    def test_a_trade_adds_up_to_zero_over_both_sides(self):
        buyer, seller = Position(), Position()
        buyer.apply_fill(litbook.Side.Buy, 7, 999_900)
        seller.apply_fill(litbook.Side.Sell, 7, 999_900)
        self.assertEqual(buyer.shares + seller.shares, 0)
        self.assertEqual(buyer.cash + seller.cash, 0)
        self.assertEqual(buyer.value(123) + seller.value(123), 0)


class KindTest(unittest.TestCase):
    def test_kind_is_the_class_name(self):
        class Accepted:
            pass

        self.assertEqual(kind(Accepted()), "Accepted")
        self.assertEqual(kind(None), "NoneType")
        self.assertEqual(kind(litbook.Side.Buy), "Side")


if __name__ == "__main__":
    unittest.main()
