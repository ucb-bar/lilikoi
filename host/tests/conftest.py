import os
import subprocess
import sys
from pathlib import Path

import pytest

HOST = Path(__file__).resolve().parents[1]
FAKES = HOST / "lilikoi" / "fakes"
CFG = HOST / "openocd" / "fletcherlake.cfg"


@pytest.fixture
def fake_env(tmp_path: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["FAKE_FPGA_DONE_FILE"] = str(tmp_path / "fpga.done")
    env["PYTHONPATH"] = str(HOST) + os.pathsep + env.get("PYTHONPATH", "")
    return env


@pytest.fixture
def fake_mcu(fake_env: dict[str, str]):
    process = subprocess.Popen(
        [sys.executable, "-m", "lilikoi.fakes.fake_mcu"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        env=fake_env,
        text=True,
    )
    assert process.stdout is not None
    port = process.stdout.readline().strip()
    assert port.startswith("/dev/"), port
    yield port
    process.terminate()
    process.wait(timeout=5)


@pytest.fixture
def board(fake_mcu: str, fake_env: dict[str, str], tmp_path: Path):
    """Return a callable running the CLI against the fake board; shuts the daemon down after."""
    run_dir = tmp_path / "run"
    elf = tmp_path / "hello.elf"
    elf.write_bytes(b"\x7fELF" + b"\0" * 60)
    bitstream = tmp_path / "top.bit"
    bitstream.write_bytes(b"\xff" * 32)
    base = [
        sys.executable, "-m", "lilikoi",
        "--mcu", fake_mcu,
        "--run-dir", str(run_dir),
        "--tsi-tty", "/dev/null",
        "--uart-tsi", str(FAKES / "fake_uart_tsi.py"),
        "--openocd", str(FAKES / "fake_openocd.py"),
        "--openocd-cfg", str(CFG),
        "--rail-limits", "650-900,650-900,650-900",
    ]

    def run(*args: str, check: bool = True, **kwargs) -> subprocess.CompletedProcess:
        result = subprocess.run([*base, *args], capture_output=True, env=fake_env, timeout=30, **kwargs)
        if check:
            assert result.returncode == 0, result.stderr.decode()
        return result

    run.elf = elf  # type: ignore[attr-defined]
    run.bitstream = bitstream  # type: ignore[attr-defined]
    run.run_dir = run_dir  # type: ignore[attr-defined]
    yield run
    subprocess.run([*base, "shutdown"], capture_output=True, env=fake_env, timeout=30)
