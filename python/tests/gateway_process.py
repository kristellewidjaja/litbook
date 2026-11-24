"""Starts a real litbook_gateway for the Python tests and stops it afterwards.

ctest passes the path of the gateway binary in LITBOOK_GATEWAY. The gateway listens on a
port the kernel picks and prints it on its first line, which is how the tests find it.
"""

import os
import re
import signal
import subprocess

READY = re.compile(r"litbook_gateway listening on 127\.0\.0\.1:(\d+)")


class GatewayProcess:
    """One gateway with the accounts ALICE and BOB (password pw) and the stock AAPL."""

    def __init__(self, extra_args=()):
        binary = os.environ.get("LITBOOK_GATEWAY")
        if not binary:
            raise RuntimeError("LITBOOK_GATEWAY is not set (run these tests through ctest)")
        args = [binary, "--port", "0", "--account", "ALICE:pw", "--account", "BOB:pw",
                "--symbol", "AAPL", *extra_args]
        self.process = subprocess.Popen(args, stdout=subprocess.PIPE, text=True)
        line = self.process.stdout.readline()
        match = READY.search(line)
        if not match:
            self.process.kill()
            self.process.wait()
            raise RuntimeError("the gateway did not start: " + line)
        self.port = int(match.group(1))

    def stop(self):
        """Stops the gateway the way Ctrl-C would, so it ends the session first."""
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdout.close()
        return self.process.returncode
