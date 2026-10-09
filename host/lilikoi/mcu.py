"""Blocking serial link to the RP2350; the daemon runs it in a worker thread."""

import itertools
import time

import serial
from serial.tools import list_ports

from .errors import LilikoiError
from .protocol import encode_request, parse_response


def resolve_port(identifier: str) -> str:
    """Accept a device path or a USB serial number (the RP2350's flash unique ID)."""
    if identifier.startswith("/"):
        return identifier
    wanted = identifier.lower()
    matches = sorted(
        port.device
        for port in list_ports.comports()
        if (port.serial_number or "").lower() == wanted
    )
    # macOS lists both tty.* and cu.*; the callout device is the one to open.
    callout = [m for m in matches if "/cu." in m]
    if callout:
        matches = callout
    if not matches:
        raise LilikoiError("MCU_NOT_FOUND", f"no USB serial device with serial number {identifier}")
    if len(matches) > 1:
        raise LilikoiError("MCU_AMBIGUOUS", f"{len(matches)} devices match {identifier}", ports=matches)
    return matches[0]


class McuLink:
    def __init__(self, port: str, baudrate: int = 115200) -> None:
        self.port = port
        self.baudrate = baudrate
        self._serial: serial.Serial | None = None
        self._tokens = itertools.count(1)

    def open(self) -> None:
        try:
            self._serial = serial.Serial(self.port, self.baudrate, timeout=0.05)
        except (serial.SerialException, OSError) as error:
            raise LilikoiError("MCU_OPEN_FAILED", str(error), port=self.port) from error
        # Let the banner and any stale output arrive, then discard them.
        time.sleep(0.2)
        self._serial.reset_input_buffer()

    def close(self) -> None:
        if self._serial is not None:
            self._serial.close()
            self._serial = None

    def request(self, command: str, *args: object, timeout: float = 2.0) -> dict[str, str]:
        if self._serial is None:
            raise LilikoiError("MCU_CLOSED", "serial link is not open")
        token = f"t{next(self._tokens)}"
        try:
            self._serial.write(encode_request(token, command, args))
            self._serial.flush()
        except (serial.SerialException, OSError) as error:
            raise LilikoiError("MCU_IO", str(error), port=self.port) from error

        deadline = time.monotonic() + timeout
        buffer = b""
        while time.monotonic() < deadline:
            try:
                chunk = self._serial.read(256)
            except (serial.SerialException, OSError) as error:
                raise LilikoiError("MCU_IO", str(error), port=self.port) from error
            if chunk:
                buffer += chunk
            while b"\n" in buffer:
                line, _, buffer = buffer.partition(b"\n")
                response = parse_response(line)
                if response is None or response.token != token:
                    continue  # banner or a stale reply from an earlier, timed-out request
                if response.ok:
                    return response.values
                raise LilikoiError(response.code or "MCU_ERROR", response.message or "", command=command)
        raise LilikoiError("MCU_TIMEOUT", f"no response to {command!r} within {timeout}s", port=self.port)
