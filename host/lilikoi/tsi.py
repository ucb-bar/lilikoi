"""Own one uart_tsi child: it loads the ELF, starts hart 0, proxies the console, and
exits with the program's tohost code.  The daemon keeps it alive across CLI calls."""

import asyncio
import os
import signal
from pathlib import Path


def build_argv(uart_tsi: str, tty: str, baud: int, elf: Path, selfcheck: bool) -> list[str]:
    argv = [uart_tsi, f"+tty={tty}", f"+baudrate={baud}"]
    if selfcheck:
        argv.append("+selfcheck")
    argv.append(str(elf))
    return argv


class TsiRun:
    def __init__(self, argv: list[str], log_path: Path) -> None:
        self.argv = argv
        self.log_path = log_path
        self.process: asyncio.subprocess.Process | None = None
        self._buffer = bytearray()
        self._pump: asyncio.Task[None] | None = None

    async def start(self) -> None:
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.process = await asyncio.create_subprocess_exec(
            *self.argv,
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            start_new_session=True,
        )
        self._pump = asyncio.create_task(self._read_output())

    async def _read_output(self) -> None:
        assert self.process is not None and self.process.stdout is not None
        with self.log_path.open("ab") as log:
            while True:
                chunk = await self.process.stdout.read(4096)
                if not chunk:
                    return
                self._buffer.extend(chunk)
                log.write(chunk)
                log.flush()

    @property
    def running(self) -> bool:
        return self.process is not None and self.process.returncode is None

    @property
    def returncode(self) -> int | None:
        return None if self.process is None else self.process.returncode

    def take_output(self) -> bytes:
        data = bytes(self._buffer)
        self._buffer.clear()
        return data

    async def wait(self) -> int:
        assert self.process is not None
        await self.process.wait()
        if self._pump is not None:
            await self._pump
        return self.process.returncode or 0

    async def stop(self) -> None:
        if self.process is None:
            return
        if self.process.returncode is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                await asyncio.wait_for(self.process.wait(), timeout=2)
            except TimeoutError:
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        await self.wait()
