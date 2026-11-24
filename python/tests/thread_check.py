"""Checks that a blocking call lets other Python threads run while it waits."""

import threading
import time


def counts_while(call, seconds):
    """How far another Python thread counts while call() runs, and while time.sleep runs
    for the same time. time.sleep always releases the interpreter lock, so its count is
    what a call that releases the lock should come close to."""
    counter = [0]
    stop = threading.Event()

    def count():
        while not stop.is_set():
            counter[0] += 1

    t = threading.Thread(target=count)
    t.start()
    try:
        time.sleep(0.05)  # Lets the counting thread get going.
        before = counter[0]
        time.sleep(seconds)
        baseline = counter[0] - before
        before = counter[0]
        call()
        during = counter[0] - before
    finally:
        stop.set()
        t.join()
    return during, baseline
