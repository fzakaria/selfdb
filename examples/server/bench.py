#!/usr/bin/env python3
"""Serving latency for self-httpd: how much does a b-tree cost per request?

One fresh TCP connection per request, because the server answers with
`Connection: close`. Everything is measured client-side, so the numbers
include connect, the per-request `sqlite3_open` of the executable, the
`SELECT` that fetches the row, the template pass, and the `INSERT` into
`visits` that every request performs.

    python3 examples/server/bench.py http://127.0.0.1:8080/ /style.css /api/stats
"""

import argparse
import http.client
import statistics
import sys
import time
from urllib.parse import urlsplit

WARMUP_REQUESTS = 20


def measure(host: str, port: int, path: str, count: int) -> list[float]:
    """Return per-request wall times in milliseconds."""
    samples = []
    for i in range(count + WARMUP_REQUESTS):
        started = time.perf_counter()
        conn = http.client.HTTPConnection(host, port, timeout=10)
        conn.request("GET", path)
        response = conn.getresponse()
        response.read()
        conn.close()
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        if response.status != 200:
            sys.exit(f"{path}: HTTP {response.status}")

        # Discard the warmup: the first requests pay for page cache misses on
        # the executable and for the kernel's first fork of the server.
        if i >= WARMUP_REQUESTS:
            samples.append(elapsed_ms)
    return samples


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("base", help="http://host:port/")
    ap.add_argument("paths", nargs="*", default=["/"], help="paths to time")
    ap.add_argument("-n", "--count", type=int, default=200)
    args = ap.parse_args()

    url = urlsplit(args.base)
    host, port = url.hostname, url.port or 80
    paths = args.paths or ["/"]

    print(f"{'path':<16} {'n':>5} {'mean':>8} {'p50':>8} {'p95':>8} {'p99':>8}  (ms)")
    for path in paths:
        samples = sorted(measure(host, port, path, args.count))
        n = len(samples)
        print(f"{path:<16} {n:>5} {statistics.mean(samples):>8.2f}"
              f" {samples[n // 2]:>8.2f} {samples[int(n * 0.95)]:>8.2f}"
              f" {samples[min(int(n * 0.99), n - 1)]:>8.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
