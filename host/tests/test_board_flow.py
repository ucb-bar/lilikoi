"""End-to-end through the real CLI, daemon, and serial link against the fake RP2350."""

import json
import time
from concurrent.futures import ThreadPoolExecutor

from lilikoi.daemon import paths_for


def test_full_job_flow(board) -> None:
    discovered = json.loads(board("discover").stdout)
    assert discovered["backend"] == "lilikoi"
    assert all(isinstance(k, str) and isinstance(v, str) for k, v in discovered.items())
    assert discovered["i2c_found"] == "7" and discovered["fpga_done"] == "0"

    board("power-on")
    board("program-fpga", str(board.bitstream))
    assert json.loads(board("discover").stdout)["fpga_done"] == "1"

    board("program-soc", str(board.elf))
    board("start")
    output = b""
    deadline = time.monotonic() + 5
    while b"hello from fake soc" not in output and time.monotonic() < deadline:
        output += board("read-uart").stdout
        time.sleep(0.05)
    assert b"hello from fake soc" in output
    result = json.loads(board("wait-for-completion").stdout)
    assert result == {"passed": True, "exit_code": 0, "detail": result["detail"]}
    assert "uart_tsi exited 0" in result["detail"]
    board("stop")
    board("stop")  # idempotent
    board("power-off")


def test_failing_program_reports_exit_code(board) -> None:
    failing = board.elf.with_name("test-fail.elf")
    failing.write_bytes(board.elf.read_bytes())
    board("power-on")
    board("program-soc", str(failing))
    board("start")
    result = json.loads(board("wait-for-completion").stdout)
    assert result["passed"] is False and result["exit_code"] == 1


def test_stop_kills_hanging_program(board) -> None:
    hanging = board.elf.with_name("hang.elf")
    hanging.write_bytes(board.elf.read_bytes())
    board("program-soc", str(hanging))
    board("start")
    time.sleep(0.3)
    board("stop")
    failed = board("wait-for-completion", check=False)
    assert failed.returncode == 1 and b"NO_RUN" in failed.stderr


def test_start_requires_program_and_rejects_double_start(board) -> None:
    failed = board("start", check=False)
    assert failed.returncode == 1 and b"NO_PROGRAM" in failed.stderr
    hanging = board.elf.with_name("hang2.elf")
    hanging.write_bytes(board.elf.read_bytes())
    board("program-soc", str(hanging))
    board("start")
    busy = board("start", check=False)
    assert busy.returncode == 1 and b"BUSY" in busy.stderr
    board("stop")


def test_voltage_limits_are_enforced(board) -> None:
    board("power-on")
    board("set-voltage", "1", "0.75")
    assert "mv=750" in board("mcu", "rail", "get", "1").stdout.decode()
    too_high = board("set-voltage", "1", "0.95", check=False)
    assert too_high.returncode == 1 and b"RANGE" in too_high.stderr
    odd_step = board("set-voltage", "1", "0.751", check=False)
    assert odd_step.returncode == 1 and b"UNREPRESENTABLE" in odd_step.stderr
    # Rail limits are mandatory: a slot without them cannot change voltages at all.
    unconfigured = board("--rail-limits", "650-900,-,650-900", "set-voltage", "1", "0.75", check=False)
    assert unconfigured.returncode == 1 and b"NO_RAIL_LIMITS" in unconfigured.stderr
    board("power-off")
    assert "mv=800" in board("mcu", "rail", "get", "1").stdout.decode()


def test_unsupported_operations_fail_explicitly(board) -> None:
    for op, arg in (("read-voltage", "0"), ("read-current", "0"), ("set-frequency", "1000000")):
        failed = board(op, arg, check=False)
        assert failed.returncode == 1 and b"UNSUPPORTED" in failed.stderr, (op, failed.stderr)


def test_bad_inputs(board) -> None:
    missing = board("program-soc", str(board.elf.with_name("nope.elf")), check=False)
    assert missing.returncode == 1 and b"NO_ELF" in missing.stderr
    not_elf = board.elf.with_name("text.elf")
    not_elf.write_text("hello")
    assert b"NOT_ELF" in board("program-soc", str(not_elf), check=False).stderr
    bad = board.bitstream.with_name("bad.bit")
    bad.write_bytes(b"\0")
    failed = board("program-fpga", str(bad), check=False)
    assert failed.returncode == 1 and b"OPENOCD_FAILED" in failed.stderr


def test_concurrent_first_calls_share_one_daemon(board, fake_mcu) -> None:
    with ThreadPoolExecutor(4) as pool:
        results = list(pool.map(lambda _: board("discover"), range(4)))
    uids = {json.loads(r.stdout)["uid"] for r in results}
    assert uids == {"FAKE0001"}
    paths = paths_for(fake_mcu, board.run_dir)
    assert paths["socket"].exists()
    # Several CLIs may race to spawn; the lock lets exactly one daemon serve.
    log = paths["log"].read_text()
    assert log.count("serving ") == 1, log


def test_mcu_passthrough(board) -> None:
    out = board("mcu", "info").stdout.decode()
    assert "proto=1" in out and "rails=3" in out
