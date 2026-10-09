"""Per-board daemon.  Holds the two things that cannot survive a process boundary: the
open RP2350 serial port and the running uart_tsi child.  Every CLI call is one JSON
request over a Unix socket; the first call spawns the daemon if it is not running."""

import argparse
import asyncio
import base64
import fcntl
import hashlib
import json
import logging
import os
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

from . import openocd
from .errors import LilikoiError
from .mcu import McuLink, resolve_port
from .settings import Settings
from .tsi import TsiRun, build_argv

logger = logging.getLogger("lilikoi.daemon")

ELF_MAGIC = b"\x7fELF"
MAX_SOCKET_PATH = 100
VCORE_RAILS = ("vcore0", "vcore1", "vcore2")
RESET_PULSE_MS = "50"


def safe_name(identifier: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", identifier)


def paths_for(mcu_id: str, run_dir: Path) -> dict[str, Path]:
    base = run_dir / safe_name(mcu_id)
    socket_path = base.with_suffix(".sock")
    if len(str(socket_path).encode()) > MAX_SOCKET_PATH:
        # macOS caps AF_UNIX paths at 104 bytes; deep run dirs (pytest, nested app support)
        # exceed it.  Keep the socket short and deterministic; everything else stays put.
        digest = hashlib.sha256(str(socket_path).encode()).hexdigest()[:16]
        socket_path = Path(tempfile.gettempdir()) / f"lilikoi-{digest}.sock"
    return {
        "socket": socket_path,
        "lock": base.with_suffix(".lock"),
        "log": base.with_suffix(".log"),
        "tsi_log": base.with_suffix(".uart_tsi.log"),
    }


class Board:
    def __init__(self, mcu_id: str, run_dir: Path) -> None:
        self.mcu_id = mcu_id
        self.paths = paths_for(mcu_id, run_dir)
        self._mcu: McuLink | None = None
        self._mcu_port: str | None = None
        self._mcu_lock = asyncio.Lock()
        self._op_lock = asyncio.Lock()
        self._run: TsiRun | None = None
        self._staged_elf: Path | None = None
        self.shutdown = asyncio.Event()

    # ---- RP2350 link --------------------------------------------------------

    async def mcu(self, settings: Settings, command: str, *args: object, timeout: float = 2.0) -> dict[str, str]:
        async with self._mcu_lock:
            if self._mcu is None:
                port = await asyncio.to_thread(resolve_port, self.mcu_id)
                link = McuLink(port, settings.mcu_baud)
                await asyncio.to_thread(link.open)
                self._mcu, self._mcu_port = link, port
            try:
                return await asyncio.to_thread(self._mcu.request, command, *args, timeout=timeout)
            except LilikoiError as error:
                if error.code in ("MCU_IO", "MCU_TIMEOUT"):
                    # Reopen on the next call; the board may have re-enumerated.
                    self._mcu.close()
                    self._mcu = None
                raise

    # ---- BoardBackend operations -------------------------------------------

    async def discover(self, settings: Settings, args: dict[str, Any]) -> dict[str, str]:
        async with self._op_lock:
            info = await self.mcu(settings, "info")
            scan = await self.mcu(settings, "scan")
            done = await self.mcu(settings, "fpga", "done")
        result: dict[str, str] = {"backend": "lilikoi", "mcu_port": self._mcu_port or ""}
        result.update(info)
        result.update({f"i2c_{k}": v for k, v in scan.items()})
        result["fpga_done"] = done.get("done", "?")
        result["openocd"] = await openocd.version(settings.openocd)
        result["uart_tsi"] = "found" if shutil.which(settings.uart_tsi) else "missing"
        result["tsi_tty"] = settings.tsi_tty or ""
        return {k: str(v) for k, v in result.items()}

    async def power_on(self, settings: Settings, args: dict[str, Any]) -> None:
        """Must leave an already-programmed FPGA alone: only release resets and check rails."""
        async with self._op_lock:
            await self.mcu(settings, "ping")
            await self.mcu(settings, "reset", "fpga", "release")
            await self.mcu(settings, "reset", "chip", "release")
            await self.mcu(settings, "rail", "snapshot")
            scan = await self.mcu(settings, "scan")
            missing = [rail for rail in VCORE_RAILS if scan.get(rail) != "1"]
            if missing:
                raise LilikoiError("PMIC_MISSING", f"regulators not responding: {', '.join(missing)}", scan=scan)
            # STANDIN(power-enable): no rail-enable GPIOs are known, so nothing is switched on
            # here.  When they are, add `power on` in the firmware and call it before the scan.
            logger.info("power_on: resets released, %s rails snapshotted", scan.get("found"))

    async def power_off(self, settings: Settings, args: dict[str, Any]) -> None:
        """Reset the platform: stop execution, restore rails, pulse both resets."""
        async with self._op_lock:
            await self._stop_run()
            errors: list[str] = []
            try:
                await self.mcu(settings, "rail", "restore", "all")
            except LilikoiError as error:
                errors.append(f"rail restore: {error.code} {error.message}")
            await self.mcu(settings, "reset", "chip", "pulse", RESET_PULSE_MS)
            await self.mcu(settings, "reset", "fpga", "pulse", RESET_PULSE_MS)
            # STANDIN(power-enable): rails stay energised; add `power off` once enables are known.
            self._staged_elf = None
            if errors:
                raise LilikoiError("POWER_OFF_INCOMPLETE", "; ".join(errors))

    async def set_voltage(self, settings: Settings, args: dict[str, Any]) -> dict[str, str]:
        rail = int(args["rail"])
        millivolts = round(float(args["volts"]) * 1000)
        limits = settings.rail_limits[rail] if rail < len(settings.rail_limits) else None
        if limits is None:
            raise LilikoiError("NO_RAIL_LIMITS", f"no chip voltage limits configured for rail {rail}")
        async with self._op_lock:
            return await self.mcu(settings, "rail", "set", rail, millivolts, limits[0], limits[1])

    async def read_voltage(self, settings: Settings, args: dict[str, Any]) -> float:
        # STANDIN(sense-parts): firmware answers UNSUPPORTED until the VSENSE parts are known.
        async with self._op_lock:
            values = await self.mcu(settings, "sense", "v", int(args["channel"]))
        return float(values["mv"]) / 1000

    async def read_current(self, settings: Settings, args: dict[str, Any]) -> float:
        # STANDIN(sense-parts): firmware answers UNSUPPORTED until the ISENSE parts are known.
        async with self._op_lock:
            values = await self.mcu(settings, "sense", "i", int(args["channel"]))
        return float(values["ma"]) / 1000

    async def set_frequency(self, settings: Settings, args: dict[str, Any]) -> None:
        # STANDIN(clock-source): no frequency control path exists yet (FPGA PLL or external
        # clock generator).  Shmoo frequency axes will fail explicitly until this lands.
        raise LilikoiError("UNSUPPORTED", "set_frequency has no hardware path yet")

    async def program_fpga(self, settings: Settings, args: dict[str, Any]) -> dict[str, str]:
        bitstream = Path(args["bitstream"])
        if not bitstream.is_file():
            raise LilikoiError("NO_BITSTREAM", f"bitstream not found: {bitstream}")
        cfg = settings.require_cfg()
        async with self._op_lock:
            await self._stop_run()
            argv = openocd.build_argv(settings.openocd, cfg, bitstream, settings.jtag_serial)
            await openocd.program(argv, settings.openocd_timeout)
            deadline = time.monotonic() + settings.fpga_done_timeout
            while True:
                done = (await self.mcu(settings, "fpga", "done")).get("done")
                if done == "1":
                    break
                if time.monotonic() >= deadline:
                    raise LilikoiError("FPGA_NOT_DONE", "openocd finished but FPGA DONE never rose")
                await asyncio.sleep(0.1)
            await self.mcu(settings, "reset", "chip", "release")
        return {"done": "1"}

    async def program_soc(self, settings: Settings, args: dict[str, Any]) -> None:
        """uart_tsi loads and starts in one step, so this stages the ELF for `start`."""
        elf = Path(args["elf"])
        if not elf.is_file():
            raise LilikoiError("NO_ELF", f"program not found: {elf}")
        with elf.open("rb") as handle:
            if handle.read(4) != ELF_MAGIC:
                raise LilikoiError("NOT_ELF", f"{elf} is not an ELF file")
        settings.require_tsi_tty()
        if not shutil.which(settings.uart_tsi):
            raise LilikoiError("NO_UART_TSI", f"uart_tsi not found: {settings.uart_tsi}")
        self._staged_elf = elf

    async def start(self, settings: Settings, args: dict[str, Any]) -> None:
        async with self._op_lock:
            if self._staged_elf is None:
                raise LilikoiError("NO_PROGRAM", "program_soc has not staged an ELF")
            if self._run is not None and self._run.running:
                raise LilikoiError("BUSY", "a run is already active; stop it first")
            argv = build_argv(
                settings.uart_tsi, settings.require_tsi_tty(), settings.tsi_baud, self._staged_elf, settings.selfcheck
            )
            run = TsiRun(argv, self.paths["tsi_log"])
            await run.start()
            self._run = run

    async def stop(self, settings: Settings, args: dict[str, Any]) -> None:
        """Idempotent: kill the host side, then pulse chip reset so the next load is clean."""
        async with self._op_lock:
            await self._stop_run()
            await self.mcu(settings, "reset", "chip", "pulse", RESET_PULSE_MS)

    async def read_uart(self, settings: Settings, args: dict[str, Any]) -> str:
        data = self._run.take_output() if self._run is not None else b""
        return base64.b64encode(data).decode()

    async def wait_for_completion(self, settings: Settings, args: dict[str, Any]) -> dict[str, Any]:
        run = self._run
        if run is None:
            raise LilikoiError("NO_RUN", "nothing is running")
        code = await run.wait()
        return {
            "passed": code == 0,
            "exit_code": code,
            "detail": f"uart_tsi exited {code}; console log at {self.paths['tsi_log']}",
        }

    async def mcu_raw(self, settings: Settings, args: dict[str, Any]) -> dict[str, str]:
        """Bench passthrough: `lilikoi mcu rail get 0`."""
        words = list(args["words"])
        async with self._op_lock:
            return await self.mcu(settings, words[0], *words[1:], timeout=float(args.get("timeout", 12)))

    async def shutdown_op(self, settings: Settings, args: dict[str, Any]) -> None:
        await self._stop_run()
        self.shutdown.set()

    async def _stop_run(self) -> None:
        if self._run is not None:
            await self._run.stop()
            self._run = None

    def close(self) -> None:
        if self._mcu is not None:
            self._mcu.close()


OPERATIONS = {
    "discover": Board.discover,
    "power_on": Board.power_on,
    "power_off": Board.power_off,
    "set_voltage": Board.set_voltage,
    "read_voltage": Board.read_voltage,
    "read_current": Board.read_current,
    "set_frequency": Board.set_frequency,
    "program_fpga": Board.program_fpga,
    "program_soc": Board.program_soc,
    "start": Board.start,
    "stop": Board.stop,
    "read_uart": Board.read_uart,
    "wait_for_completion": Board.wait_for_completion,
    "mcu": Board.mcu_raw,
    "shutdown": Board.shutdown_op,
}


async def serve(board: Board) -> None:
    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            line = await reader.readline()
            if not line:
                return
            request = json.loads(line)
            operation = OPERATIONS.get(request.get("op"))
            if operation is None:
                raise LilikoiError("UNKNOWN_OPERATION", f"unknown operation {request.get('op')!r}")
            settings = Settings.from_dict(request.get("settings") or {})
            result = await operation(board, settings, request.get("args") or {})
            reply: dict[str, Any] = {"ok": True, "result": result}
        except LilikoiError as error:
            reply = error.as_dict()
        except Exception as error:  # noqa: BLE001 -- every failure must reach the CLI as JSON
            logger.exception("operation failed")
            reply = {"ok": False, "code": "INTERNAL", "message": f"{type(error).__name__}: {error}"}
        try:
            writer.write((json.dumps(reply) + "\n").encode())
            await writer.drain()
        finally:
            writer.close()

    socket_path = board.paths["socket"]
    if socket_path.exists():
        socket_path.unlink()
    server = await asyncio.start_unix_server(handle, path=str(socket_path))
    os.chmod(socket_path, 0o600)
    logger.info("serving %s", socket_path)
    async with server:
        await board.shutdown.wait()
    board.close()
    if socket_path.exists():
        socket_path.unlink()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Lilikoi per-board daemon")
    parser.add_argument("--mcu", required=True, help="RP2350 USB serial number or device path")
    parser.add_argument("--run-dir", required=True, type=Path)
    options = parser.parse_args(argv)
    options.run_dir.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s", stream=sys.stderr)

    paths = paths_for(options.mcu, options.run_dir)
    lock = os.open(paths["lock"], os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        logger.info("another daemon already owns %s", options.mcu)
        return 0
    try:
        asyncio.run(serve(Board(options.mcu, options.run_dir)))
    finally:
        os.close(lock)
    return 0


if __name__ == "__main__":
    sys.exit(main())
