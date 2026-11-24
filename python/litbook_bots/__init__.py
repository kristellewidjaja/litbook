"""Python trading bots and tools for litbook.

market_maker quotes both sides of one stock, noise_trader sends random orders,
book_view shows the feed's book in a terminal, and simulate runs a small market of
them against a real litbook_gateway. Each module keeps its decisions in plain
functions that take values and return values, and keeps the waiting and the network
in one loop class, so the decisions can be tested without an exchange.
"""
