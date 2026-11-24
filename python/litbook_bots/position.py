"""What one account owns after its fills, kept from the Executed replies alone."""

from dataclasses import dataclass

import litbook


@dataclass
class Position:
    """Shares held (negative when short) and the cash the fills moved, in price units.

    A buy of q shares at p adds q shares and takes q * p of cash, and a sell does the
    opposite. Every trade has a buyer and a seller, so over all accounts the shares and
    the cash both add up to zero. The value at a mark price is cash + shares * mark,
    which is the profit or loss if the shares could be sold at that price.
    """

    shares: int = 0
    cash: int = 0
    bought: int = 0
    sold: int = 0
    fills: int = 0

    def apply_fill(self, side, qty, price):
        if side == litbook.Side.Buy:
            self.shares += qty
            self.cash -= qty * price
            self.bought += qty
        else:
            self.shares -= qty
            self.cash += qty * price
            self.sold += qty
        self.fills += 1

    def value(self, mark):
        return self.cash + self.shares * mark
