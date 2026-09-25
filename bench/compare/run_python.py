#!/usr/bin/env python3
"""Python ITCH parsers.  run_python.py FILE (itchfeed|meatpy|meatpy_book)

  itchfeed     bbalouki/itch pure-Python MessageParser.parse_stream (ITCH_NO_CPP=1)
  meatpy       vgreg/MeatPy ITCH50MessageReader, every message decoded
  meatpy_book  MeatPy reader + ITCH50MarketProcessor; MeatPy books ONE symbol (AAPL)

Set PYTHONPATH to the two repos' source roots. Same RESULT line as the C++ runners.
"""
import datetime
import os
import resource
import sys
import time

os.environ.setdefault("ITCH_NO_CPP", "1")


def result(lib, mode, msgs, secs, nbytes, checksum, extra=""):
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // (1024 * 1024)
    print(f"RESULT lib={lib} mode={mode} arch=arm64 msgs={msgs} secs={secs:.6f} "
          f"ns_per_msg={secs * 1e9 / msgs:.3f} mmsg_s={msgs / secs / 1e6:.3f} "
          f"gib_s={nbytes / secs / 2**30:.3f} checksum={checksum} peak_rss_mib={rss}"
          + (f" {extra}" if extra else ""), flush=True)


def main():
    path, mode = sys.argv[1], sys.argv[2]
    nbytes = os.path.getsize(path)
    n = s = 0
    if mode == "itchfeed":
        import itch
        from itch.parser import MessageParser
        assert not itch.USING_CPP_BACKEND
        data = open(path, "rb").read()
        t0 = time.perf_counter()
        for m in MessageParser().parse_stream(data):
            n += 1
            s += m.stock_locate + m.timestamp
        result("itchfeed", "parse", n, time.perf_counter() - t0, nbytes, s % 2**64)
    elif mode == "meatpy":
        from meatpy.itch50 import ITCH50MessageReader
        t0 = time.perf_counter()
        for m in ITCH50MessageReader().read_file(path):
            n += 1
            s += m.timestamp
        result("meatpy", "parse", n, time.perf_counter() - t0, nbytes, 0)
    elif mode == "meatpy_book":
        from meatpy.itch50 import ITCH50MarketProcessor, ITCH50MessageReader
        proc = ITCH50MarketProcessor("AAPL", datetime.datetime(2019, 1, 30))
        t0 = time.perf_counter()
        for m in ITCH50MessageReader().read_file(path):
            n += 1
            proc.process_message(m)
        result("meatpy", "book_aapl_only", n, time.perf_counter() - t0, nbytes, 0)
        lob = proc.current_lob
        if lob is not None and lob.bid_levels and lob.ask_levels:
            b, a = lob.bid_levels[0], lob.ask_levels[0]
            print(f"BBO lib=meatpy sym=AAPL bid={b.volume()}@{b.price / 1e4:.4f} "
                  f"ask={a.volume()}@{a.price / 1e4:.4f}")
    else:
        sys.exit(f"unknown mode {mode}")


if __name__ == "__main__":
    main()
