"""Stand ins for the order client and the feed book, so the bots' loops can be tested
one step at a time without an exchange. The reply classes have the module's names and
the fields the bots read (the bots tell replies apart by name, see replies.py)."""

from dataclasses import dataclass

import litbook


@dataclass
class Accepted:
    user_ref: int


@dataclass
class Replaced:
    orig_user_ref: int
    user_ref: int


@dataclass
class Rejected:
    user_ref: int
    reason: object


@dataclass
class Executed:
    user_ref: int
    qty: int
    price: int
    match: int


@dataclass
class Canceled:
    user_ref: int
    qty: int
    reason: object


@dataclass
class FakeOrder:
    symbol: str
    side: object
    price: int
    open: int
    executed: int = 0


class FakeClient:
    """Keeps open orders the way the real client's tracker does, and answers at once.

    Switches make it misbehave the way the real exchange can. reject_enters refuses
    every Enter (as a post-only order that would cross is refused), reject_replaces
    refuses a Replace and cancels the order (OUCH does that), and fill_on_replace fills
    the whole order just before a Replace arrives, so the Replace gets no answer.
    """

    def __init__(self):
        self.orders = {}
        self.next_ref = 1
        self.log = []
        self.pending = []
        self.reject_enters = False
        self.reject_replaces = False
        self.fill_on_replace = False
        self.next_match = 1

    def _new_ref(self):
        ref = self.next_ref
        self.next_ref += 1
        return ref

    def enter(self, symbol, side, qty, price, tif=litbook.TimeInForce.GoodTillCancel,
              type=litbook.OrderType.Limit, post_only=False):
        ref = self._new_ref()
        self.log.append(("enter", ref, side, qty, price, tif, post_only))
        if self.reject_enters:
            return Rejected(ref, litbook.RejectReason.PostOnlyWouldCross)
        if tif == litbook.TimeInForce.GoodTillCancel:
            self.orders[ref] = FakeOrder(symbol, side, price, qty)
        return Accepted(ref)

    def replace(self, user_ref, qty, price):
        order = self.orders.get(user_ref)
        if order is not None and self.fill_on_replace:
            self.fill(user_ref, order.open)
            order = None
        if order is None:
            return None
        ref = self._new_ref()
        self.log.append(("replace", user_ref, ref, qty, price))
        del self.orders[user_ref]
        if self.reject_replaces:
            self.pending.append(Canceled(user_ref, order.open,
                                         litbook.CancelReason.UserRequested))
            return Rejected(ref, litbook.RejectReason.PostOnlyWouldCross)
        self.orders[ref] = FakeOrder(order.symbol, order.side, price, qty - order.executed,
                                     order.executed)
        return Replaced(user_ref, ref)

    def cancel(self, user_ref):
        order = self.orders.pop(user_ref, None)
        if order is None:
            return None
        self.log.append(("cancel", user_ref))
        return Canceled(user_ref, order.open, litbook.CancelReason.UserRequested)

    def fill(self, user_ref, qty):
        """Trades qty shares of an open order, as if another trader took them."""
        order = self.orders[user_ref]
        order.open -= qty
        order.executed += qty
        if order.open == 0:
            del self.orders[user_ref]
        self.pending.append(Executed(user_ref, qty, order.price, self.next_match))
        self.next_match += 1

    def events(self, timeout_ms=0):
        got, self.pending = self.pending, []
        return got

    def order(self, user_ref):
        return self.orders.get(user_ref)

    def open_orders(self):
        return dict(self.orders)

    def sent(self, kind):
        return [entry for entry in self.log if entry[0] == kind]


@dataclass
class Level:
    price: int
    qty: int
    orders: int = 1


@dataclass
class Quote:
    bid: object
    ask: object
    live: bool = True


class FakeFeed:
    """A feed book that shows other traders' levels plus, when given a client, that
    client's own open orders, as the real feed would after a moment."""

    def __init__(self, client=None, status=litbook.SymbolStatus.Live):
        self.client = client
        self.state = status
        self.other_bids = []  # (price, qty) best first
        self.other_asks = []
        self.version = 0
        self.trades = 0

    def status(self, symbol):
        return self.state

    def _side(self, side):
        levels = {}
        others = self.other_bids if side == litbook.Side.Buy else self.other_asks
        for price, qty in others:
            levels[price] = levels.get(price, 0) + qty
        if self.client is not None:
            for order in self.client.open_orders().values():
                if order.side == side:
                    levels[order.price] = levels.get(order.price, 0) + order.open
        best_first = sorted(levels, reverse=(side == litbook.Side.Buy))
        return [Level(price, levels[price]) for price in best_first]

    def depth(self, symbol, side, levels=10):
        return self._side(side)[:levels]

    def best(self, symbol):
        if self.state == litbook.SymbolStatus.Unknown:
            return None
        bids = self._side(litbook.Side.Buy)
        asks = self._side(litbook.Side.Sell)
        return Quote(bids[0] if bids else None, asks[0] if asks else None,
                     self.state == litbook.SymbolStatus.Live)

    def wait_update(self, seen, timeout_ms):
        return self.version

    def stats(self):
        return {"trades": self.trades}
