"""Simulated lilikoi firmware on a pseudo-terminal.  Prints the slave device path on its
first stdout line so a test (or a bench user) can point `lilikoi --mcu` at it.

Environment:
  FAKE_MCU_MISSING        comma-separated device names that should not ACK (e.g. vcore1)
  FAKE_FPGA_DONE_FILE     `fpga done` reports 1 while this file exists
"""

import os
import sys
import time
import tty

from ..protocol import encode_err, encode_ok

PCB_MIN_MV, PCB_MAX_MV = 400, 1200
DEVICES = ["isense0", "isense1", "vcore0", "vcore1", "vcore2", "vsense_pmic", "vsense_vcore"]
RAIL_STEP_UV = [1250, 5000, 5000]


class FakeMcu:
    def __init__(self) -> None:
        missing = {name for name in os.environ.get("FAKE_MCU_MISSING", "").split(",") if name}
        self.present = {name: name not in missing for name in DEVICES}
        self.codes = [self._code(rail, 800) for rail in range(3)]
        self.snapshot: list[int | None] = [None, None, None]
        self.fpga_reset = False
        self.chip_reset = False
        self.sertl_rx = 0
        self.snapshot_all()

    def _code(self, rail: int, mv: int) -> int:
        return (mv * 1000 - 400_000) // RAIL_STEP_UV[rail]

    def _mv(self, rail: int, code: int) -> int:
        return (400_000 + code * RAIL_STEP_UV[rail] + 500) // 1000

    def snapshot_all(self) -> int:
        count = 0
        for rail in range(3):
            if self.present[f"vcore{rail}"]:
                self.snapshot[rail] = self.codes[rail]
                count += 1
            else:
                self.snapshot[rail] = None
        return count

    def fpga_done(self) -> int:
        path = os.environ.get("FAKE_FPGA_DONE_FILE")
        return 1 if path and os.path.exists(path) else 0

    def rail_state(self, rail: int) -> dict[str, object]:
        snap = self.snapshot[rail]
        return {
            "rail": rail,
            "name": f"vcore{rail}",
            "mv": self._mv(rail, self.codes[rail]),
            "code": f"0x{self.codes[rail]:02x}",
            "step_uv": RAIL_STEP_UV[rail],
            "status": "0x00",
            "snapshot_mv": self._mv(rail, snap) if snap is not None else 0,
        }

    def handle(self, token: str, command: str, args: list[str]) -> bytes:
        if command == "ping":
            return encode_ok(token)
        if command == "info":
            return encode_ok(token, {"fw": "fake", "proto": 1, "board": "fake_rp2350b", "uid": "FAKE0001",
                                     "sertl": "ready", "rails": 3, "pcb_min_mv": PCB_MIN_MV, "pcb_max_mv": PCB_MAX_MV})
        if command == "scan":
            values: dict[str, object] = {name: int(ok) for name, ok in self.present.items()}
            values["found"] = sum(self.present.values())
            values["expected"] = len(DEVICES)
            return encode_ok(token, values)
        if command == "fpga":
            return encode_ok(token, {"done": self.fpga_done()})
        if command == "reset":
            return self.reset(token, args)
        if command == "rail":
            return self.rail(token, args)
        if command == "sertl":
            return encode_ok(token, {"ready": 1, "rx_phits": self.sertl_rx, "tx_phits": 0})
        if command in ("sense", "power"):
            return encode_err(token, "UNSUPPORTED", f"{command} not implemented in fake")
        return encode_err(token, "UNKNOWN", "unknown command")

    def reset(self, token: str, args: list[str]) -> bytes:
        if len(args) < 2 or args[0] not in ("fpga", "chip"):
            return encode_err(token, "BAD_REQUEST", "usage: reset fpga|chip assert|release|pulse [ms]")
        attr = "fpga_reset" if args[0] == "fpga" else "chip_reset"
        if args[1] == "assert":
            setattr(self, attr, True)
        elif args[1] == "release":
            setattr(self, attr, False)
        elif args[1] == "pulse":
            ms = int(args[2]) if len(args) > 2 else 10
            if not 1 <= ms <= 10000:
                return encode_err(token, "BAD_REQUEST", "pulse must be 1..10000 ms")
            time.sleep(ms / 1000)
            setattr(self, attr, False)
        else:
            return encode_err(token, "BAD_REQUEST", "unknown reset action")
        return encode_ok(token, {"fpga_reset": int(self.fpga_reset), "chip_reset": int(self.chip_reset), "done": self.fpga_done()})

    def rail(self, token: str, args: list[str]) -> bytes:
        if not args:
            return encode_err(token, "BAD_REQUEST", "usage: rail ...")
        if args[0] == "count":
            return encode_ok(token, {"count": 3})
        if args[0] == "snapshot":
            return encode_ok(token, {"snapshotted": self.snapshot_all(), "expected": 3})
        if args[0] == "restore" and len(args) > 1 and args[1] == "all":
            restored = 0
            for rail in range(3):
                if self.snapshot[rail] is not None:
                    self.codes[rail] = self.snapshot[rail]
                    restored += 1
            values = {"restored": restored, "expected": 3}
            return encode_ok(token, values) if restored == 3 else encode_err(token, "I2C", f"restored={restored}")
        if len(args) < 2 or not args[1].isdigit() or int(args[1]) > 2:
            return encode_err(token, "BAD_RAIL", "rail must be 0..2")
        rail = int(args[1])
        if not self.present[f"vcore{rail}"]:
            return encode_err(token, "I2C", f"rail={rail} no ack")
        if args[0] == "get":
            return encode_ok(token, self.rail_state(rail))
        if args[0] == "restore":
            if self.snapshot[rail] is None:
                return encode_err(token, "NO_SNAPSHOT", f"rail={rail}")
            self.codes[rail] = self.snapshot[rail]
            return encode_ok(token, self.rail_state(rail))
        if args[0] == "set":
            if len(args) < 5:
                return encode_err(token, "BAD_REQUEST", "usage: rail set <n> <mv> <min_mv> <max_mv>")
            mv, lo, hi = (int(a) for a in args[2:5])
            if self.snapshot[rail] is None:
                return encode_err(token, "NO_SNAPSHOT", f"rail={rail}")
            if lo > hi or lo < PCB_MIN_MV or hi > PCB_MAX_MV:
                return encode_err(token, "PCB_BOUND", f"limits {lo}..{hi} exceed pcb {PCB_MIN_MV}..{PCB_MAX_MV}")
            if not lo <= mv <= hi:
                return encode_err(token, "RANGE", f"rail={rail} mv={mv} limits={lo}..{hi}")
            offset_uv = mv * 1000 - 400_000
            if offset_uv % RAIL_STEP_UV[rail] or offset_uv // RAIL_STEP_UV[rail] > 255:
                return encode_err(token, "UNREPRESENTABLE", f"rail={rail} mv={mv}")
            self.codes[rail] = offset_uv // RAIL_STEP_UV[rail]
            return encode_ok(token, self.rail_state(rail))
        return encode_err(token, "BAD_REQUEST", "unknown rail action")


def main() -> int:
    master, slave = os.openpty()
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)
    mcu = FakeMcu()
    os.write(master, b"* lilikoi fake proto=1 uid=FAKE0001 ready\n")
    buffer = b""
    while True:
        try:
            chunk = os.read(master, 256)
        except OSError:
            return 0
        if not chunk:
            return 0
        buffer += chunk
        while b"\n" in buffer:
            line, _, buffer = buffer.partition(b"\n")
            words = line.decode(errors="replace").strip("\r").split()
            if not words:
                continue
            if len(words) < 2:
                os.write(master, encode_err(words[0], "BAD_REQUEST", "missing command"))
                continue
            os.write(master, mcu.handle(words[0], words[1], words[2:]))


if __name__ == "__main__":
    sys.exit(main())
