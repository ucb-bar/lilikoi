"""Drive the CLI through fl-agent's own LilikoiBoardBackend when the scheduler is importable."""

import sys
from pathlib import Path

import pytest

fl_agent = pytest.importorskip("fl_agent.hardware.lilikoi")

from conftest import CFG, FAKES  # noqa: E402  (tests/ is not a package)


@pytest.mark.asyncio
async def test_backend_against_fake_board(fake_mcu: str, fake_env, tmp_path: Path, monkeypatch) -> None:
    for key, value in fake_env.items():
        monkeypatch.setenv(key, value)
    elf = tmp_path / "hello.elf"
    elf.write_bytes(b"\x7fELF" + b"\0" * 60)
    bitstream = tmp_path / "top.bit"
    bitstream.write_bytes(b"\xff" * 32)
    base = [
        sys.executable, "-m", "lilikoi", "--mcu", "{mcu}", "--run-dir", str(tmp_path / "run"),
        "--tsi-tty", "{tsi_tty}", "--uart-tsi", str(FAKES / "fake_uart_tsi.py"),
        "--openocd", str(FAKES / "fake_openocd.py"), "--openocd-cfg", str(CFG),
        "--rail-limits", "{rail_limits}",
    ]
    ops = ["discover", "power_on", "power_off", "set_voltage", "program_fpga", "program_soc",
           "start", "stop", "read_uart", "wait_for_completion"]
    extra = {"set_voltage": ["{rail}", "{volts}"], "program_fpga": ["{bitstream}"], "program_soc": ["{elf}"]}
    commands = {op: [*base, op.replace("_", "-"), *extra.get(op, [])] for op in ops}
    backend = fl_agent.LilikoiBoardBackend(
        commands, timeout=30,
        devices={"mcu": fake_mcu, "tsi_tty": "/dev/null", "rail_limits": "650-900,650-900,650-900"},
    )
    try:
        assert (await backend.discover())["backend"] == "lilikoi"
        await backend.power_on()
        await backend.program_fpga(bitstream)
        await backend.program_soc(elf)
        await backend.set_voltage(1, 0.75)
        await backend.start()
        result = await backend.wait_for_completion()
        assert result.passed and result.exit_code == 0
        assert b"hello from fake soc" in await backend.read_uart()
        await backend.stop()
        await backend.power_off()
    finally:
        import subprocess
        subprocess.run([*base[:-2], "shutdown"], capture_output=True)
