"""Integer price helpers shared by the bots and the book view.

Prices are integer ticks with 4 implied decimals, so 1_000_000 is $100.00, the same
as on the wire. The gateway's books use a tick of 100 ($0.01). Nothing here uses
floating point, so a price never picks up a rounding error.
"""

SCALE = 10_000  # Price units per dollar.
TICK = 100  # One cent, the gateway's tick size.


def floor_to_tick(price, tick=TICK):
    """The highest multiple of tick that is not above price."""
    return (price // tick) * tick


def ceil_to_tick(price, tick=TICK):
    """The lowest multiple of tick that is not below price."""
    return -((-price) // tick) * tick


def format_price(price):
    """Turns integer ticks into dollars, like 1_000_100 into "100.01"."""
    sign = "-" if price < 0 else ""
    whole, frac = divmod(abs(price), SCALE)
    return f"{sign}{whole}.{frac // 100:02d}" if frac % 100 == 0 else f"{sign}{whole}.{frac:04d}"


def format_money(amount):
    """Turns a cash amount in price units (shares times ticks) into dollars and cents.
    Fractions of a cent are cut off, so printed amounts that add up to zero exactly can
    be a cent off when they are added up by hand."""
    sign = "-" if amount < 0 else ""
    cents = abs(amount) // 100
    return f"{sign}${cents // 100:,}.{cents % 100:02d}"
