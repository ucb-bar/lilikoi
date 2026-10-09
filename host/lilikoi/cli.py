"""Stateless CLI.  One subcommand per BoardBackend operation, emitting exactly what
fl-agent's LilikoiBoardBackend parses: a JSON string map for discover, a number for sensor
reads, raw bytes for read-uart, a JSON RunResult for wait-for-completion, and exit status
for everything else.  Errors go to stderr as `CODE: message` with exit status 1."""

import argparse
import base64
import json
import os
import sys
from pathlib import Path
from typing import Any

from . import client
from .errors import LilikoiError

DEFAULT_RUN_DIR = Path.home() / ".lilikoi" / "run"

SETTING_FLAGS: dict[str, dict[str, Any]] = {
    "mcu_baud": {"type": int, "help": "RP2350 CDC baud (ignored by USB, kept for completeness)"},
    "tsi_tty": {"help": "serial device of the FPGA's UART-TSI port"},
    "tsi_baud": {"type": int, "help": "UART-TSI baud; must match the bitstream (default 921600)"},
    "uart_tsi": {"help": "path to the uart_tsi binary (default: found on PATH)"},
    "openocd": {"help": "path to openocd (default: found on PATH)"},
    "openocd_cfg": {"help": "OpenOCD cfg describing adapter and tap (no init/pld load)"},
    "jtag_serial": {"help": "adapter serial number, for Macs with several JTAG cables"},
    "openocd_timeout": {"type": float, "help": "seconds allowed for bitstream programming"},
    "fpga_done_timeout": {"type": float, "help": "seconds to wait for FPGA DONE after programming"},
    "rail_limits": {"help": "chip voltage limits per rail in mV, e.g. 650-900,650-900,650-900"},
}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="lilikoi", description=__doc__.split("\n\n")[0])
    parser.add_argument("--mcu", default=os.environ.get("LILIKOI_MCU"), help="RP2350 USB serial number or device path")
    parser.add_argument("--run-dir", type=Path, default=Path(os.environ.get("LILIKOI_RUN_DIR", DEFAULT_RUN_DIR)))
    for name, spec in SETTING_FLAGS.items():
        parser.add_argument(f"--{name.replace('_', '-')}", dest=name, default=None, **spec)
    parser.add_argument("--no-selfcheck", action="store_true", help="skip uart_tsi readback verification")

    sub = parser.add_subparsers(dest="op", required=True)
    sub.add_parser("discover")
    sub.add_parser("power-on")
    sub.add_parser("power-off")
    p = sub.add_parser("set-voltage")
    p.add_argument("rail", type=int)
    p.add_argument("volts", type=float)
    p = sub.add_parser("read-voltage")
    p.add_argument("channel", type=int)
    p = sub.add_parser("read-current")
    p.add_argument("channel", type=int)
    p = sub.add_parser("set-frequency")
    p.add_argument("hz", type=float)
    p = sub.add_parser("program-fpga")
    p.add_argument("bitstream", type=Path)
    p = sub.add_parser("program-soc")
    p.add_argument("elf", type=Path)
    sub.add_parser("start")
    sub.add_parser("stop")
    sub.add_parser("read-uart")
    sub.add_parser("wait-for-completion")
    sub.add_parser("shutdown", help="stop the board's daemon")
    p = sub.add_parser("mcu", help="send a raw firmware command, e.g. `mcu rail get 0`")
    p.add_argument("words", nargs="+")
    return parser


def settings_from(options: argparse.Namespace) -> dict[str, Any]:
    settings = {name: getattr(options, name) for name in SETTING_FLAGS if getattr(options, name) is not None}
    if options.no_selfcheck:
        settings["selfcheck"] = False
    for key in ("openocd_cfg",):
        if key in settings:
            settings[key] = str(Path(settings[key]).expanduser().resolve())
    return settings


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    options = parser.parse_args(argv)
    if not options.mcu:
        parser.error("--mcu (or LILIKOI_MCU) is required")
    op = options.op.replace("-", "_")
    args: dict[str, Any] = {}
    if op == "set_voltage":
        args = {"rail": options.rail, "volts": options.volts}
    elif op in ("read_voltage", "read_current"):
        args = {"channel": options.channel}
    elif op == "set_frequency":
        args = {"hz": options.hz}
    elif op == "program_fpga":
        args = {"bitstream": str(options.bitstream.expanduser().resolve())}
    elif op == "program_soc":
        args = {"elf": str(options.elf.expanduser().resolve())}
    elif op == "mcu":
        args = {"words": options.words}

    try:
        result = client.request(options.mcu, options.run_dir, op, args, settings_from(options))
    except LilikoiError as error:
        detail = f" {json.dumps(error.details)}" if error.details else ""
        print(f"{error.code}: {error.message}{detail}", file=sys.stderr)
        return 1

    if op == "discover":
        print(json.dumps(result, sort_keys=True))
    elif op in ("read_voltage", "read_current"):
        print(result)
    elif op == "read_uart":
        sys.stdout.buffer.write(base64.b64decode(result or ""))
        sys.stdout.buffer.flush()
    elif op == "wait_for_completion":
        print(json.dumps(result))
    elif op == "mcu":
        print(" ".join(f"{k}={v}" for k, v in (result or {}).items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
