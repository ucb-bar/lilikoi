#!/usr/bin/env python3
"""Stand-in for openocd.  Accepts `-f cfg` and `-c '...; pld load 0 {path}; ...'`, checks both
files exist, and creates FAKE_FPGA_DONE_FILE so the fake MCU reports DONE.  A bitstream whose
name contains 'bad' fails like a programming error."""

import os
import re
import sys


def main() -> int:
    args = sys.argv[1:]
    if "--version" in args:
        print("Open On-Chip Debugger 0.0.0-fake", file=sys.stderr)
        return 0
    cfg = None
    commands = []
    i = 0
    while i < len(args):
        if args[i] == "-f" and i + 1 < len(args):
            cfg = args[i + 1]
            i += 2
        elif args[i] == "-c" and i + 1 < len(args):
            commands.append(args[i + 1])
            i += 2
        else:
            i += 1
    if not cfg or not os.path.exists(cfg):
        print(f"fake-openocd: missing cfg {cfg}", file=sys.stderr)
        return 1
    joined = "; ".join(commands)
    match = re.search(r"pld load 0 \{([^}]*)\}", joined)
    if not match:
        print("fake-openocd: no `pld load 0 {path}` command", file=sys.stderr)
        return 1
    bitstream = match.group(1)
    if not os.path.exists(bitstream):
        print(f"fake-openocd: bitstream not found {bitstream}", file=sys.stderr)
        return 1
    if "bad" in os.path.basename(bitstream):
        print("fake-openocd: Error: programming failed", file=sys.stderr)
        return 1
    print(f"fake-openocd: loaded {bitstream}")
    done = os.environ.get("FAKE_FPGA_DONE_FILE")
    if done:
        with open(done, "w") as handle:
            handle.write("1\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
