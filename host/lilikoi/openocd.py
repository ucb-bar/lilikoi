"""Program the FPGA with OpenOCD.  The cfg stays static; the bitstream is a command-line
argument so trusted configuration never has to be rewritten per job."""

import asyncio
from pathlib import Path

from .errors import LilikoiError


def build_argv(openocd: str, cfg: Path, bitstream: Path, jtag_serial: str | None) -> list[str]:
    path = str(bitstream)
    if any(c in path for c in "{}\n\r"):
        raise LilikoiError("BAD_PATH", "bitstream path may not contain braces or newlines")
    argv = [openocd, "-f", str(cfg)]
    if jtag_serial:
        # Must follow the cfg (which selects the adapter driver) and precede init.
        argv += ["-c", f"adapter serial {jtag_serial}"]
    argv += ["-c", f"init; pld load 0 {{{path}}}; shutdown"]
    return argv


async def program(argv: list[str], timeout: float) -> str:
    try:
        process = await asyncio.create_subprocess_exec(
            *argv,
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            start_new_session=True,
        )
    except OSError as error:
        raise LilikoiError("OPENOCD_START_FAILED", str(error), argv=argv) from error
    try:
        output, _ = await asyncio.wait_for(process.communicate(), timeout)
    except TimeoutError:
        process.kill()
        await process.wait()
        raise LilikoiError("OPENOCD_TIMED_OUT", f"openocd exceeded {timeout}s", argv=argv) from None
    text = output.decode(errors="replace")
    if process.returncode:
        raise LilikoiError(
            "OPENOCD_FAILED", f"openocd exited {process.returncode}", argv=argv, output=text[-4000:]
        )
    return text


async def version(openocd: str) -> str:
    try:
        process = await asyncio.create_subprocess_exec(
            openocd, "--version", stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT
        )
        output, _ = await asyncio.wait_for(process.communicate(), 10)
    except (OSError, TimeoutError):
        return "missing"
    first = output.decode(errors="replace").strip().splitlines()
    return first[0] if first else "unknown"
