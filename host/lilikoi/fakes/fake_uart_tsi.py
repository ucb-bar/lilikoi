#!/usr/bin/env python3
"""Stand-in for uart_tsi.  Same argument shape: +tty=... +baudrate=... [+selfcheck] <elf>.

The ELF's basename steers behaviour: 'fail' exits 1, 'hang' never exits (until killed),
'exit<N>' exits N.  FAKE_TSI_SECONDS (default 0.2) sets the run time."""

import os
import re
import sys
import time


def main() -> int:
    plusargs = [a for a in sys.argv[1:] if a.startswith("+")]
    targets = [a for a in sys.argv[1:] if not a.startswith("+")]
    tty = next((a[5:] for a in plusargs if a.startswith("+tty=")), None)
    if not tty:
        print("UART-TSI: ERROR: Must use +tty=/dev/ttyxx to specify a tty", flush=True)
        return 1
    if not targets:
        print("UART-TSI: no target", flush=True)
        return 1
    elf = targets[-1]
    if not os.path.exists(elf):
        print(f"UART-TSI: cannot open {elf}", flush=True)
        return 1
    print("UART-TSI: Starting UART-based TSI", flush=True)
    print(f"UART-TSI: Attempting to open TTY at {tty}", flush=True)
    print("UART-TSI: Connection succeeded", flush=True)
    print("UART-TSI: Loading program", flush=True)
    if "+selfcheck" in plusargs:
        print("UART-TSI: Performing self check of region 80000000-80001000 ... Done", flush=True)
    print("UART-TSI: Done loading program", flush=True)
    sys.stdout.write(os.environ.get("FAKE_TSI_OUTPUT", "hello from fake soc\n"))
    sys.stdout.flush()
    name = os.path.basename(elf)
    if "hang" in name:
        while True:
            time.sleep(3600)
    time.sleep(float(os.environ.get("FAKE_TSI_SECONDS", "0.2")))
    print("UART-TSI: Done, shutting down, flushing UART", flush=True)
    match = re.search(r"exit(\d+)", name)
    if match:
        return int(match.group(1))
    return 1 if "fail" in name else 0


if __name__ == "__main__":
    sys.exit(main())
