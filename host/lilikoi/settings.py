"""Per-call board settings.  fl-agent owns these in cluster.yaml and passes them with every
command, so the daemon holds no configuration of its own."""

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .errors import LilikoiError


def parse_rail_limits(text: str | None) -> list[tuple[int, int] | None]:
    """'650-900,650-900,-' -> [(650, 900), (650, 900), None]; index is the rail number."""
    if not text:
        return []
    limits: list[tuple[int, int] | None] = []
    for item in text.split(","):
        item = item.strip()
        if item in ("", "-"):
            limits.append(None)
            continue
        low, sep, high = item.partition("-")
        if not sep or not low.isdigit() or not high.isdigit():
            raise LilikoiError("BAD_RAIL_LIMITS", f"expected MIN-MAX in millivolts, got {item!r}")
        limits.append((int(low), int(high)))
    return limits


@dataclass
class Settings:
    mcu_baud: int = 115200
    tsi_tty: str | None = None
    tsi_baud: int = 921600
    uart_tsi: str = "uart_tsi"
    selfcheck: bool = True
    openocd: str = "openocd"
    openocd_cfg: str | None = None
    jtag_serial: str | None = None
    openocd_timeout: float = 180.0
    fpga_done_timeout: float = 5.0
    rail_limits: list[tuple[int, int] | None] = field(default_factory=list)

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "Settings":
        settings = cls()
        for key, value in data.items():
            if key == "rail_limits":
                settings.rail_limits = parse_rail_limits(value)
            elif hasattr(settings, key) and value is not None:
                current = getattr(settings, key)
                if isinstance(current, bool):
                    value = str(value).lower() in ("1", "true", "yes")
                elif isinstance(current, int):
                    value = int(value)
                elif isinstance(current, float):
                    value = float(value)
                setattr(settings, key, value)
        return settings

    def require_cfg(self) -> Path:
        if not self.openocd_cfg:
            raise LilikoiError("NO_OPENOCD_CFG", "program_fpga needs --openocd-cfg")
        cfg = Path(self.openocd_cfg)
        if not cfg.is_file():
            raise LilikoiError("NO_OPENOCD_CFG", f"openocd cfg not found: {cfg}")
        return cfg

    def require_tsi_tty(self) -> str:
        if not self.tsi_tty:
            raise LilikoiError("NO_TSI_TTY", "start needs --tsi-tty (the FPGA's UART-TSI port)")
        return self.tsi_tty
