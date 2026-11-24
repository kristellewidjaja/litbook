"""Telling the order client's replies apart."""


def kind(reply):
    """The name of a reply's type, such as "Accepted", "Executed" or "NoneType".

    The bots check replies by this name rather than with isinstance. The module's reply
    classes cannot be built from Python, and checking the name lets the tests hand the
    bots small stand in classes with the same names and fields.
    """
    return type(reply).__name__
