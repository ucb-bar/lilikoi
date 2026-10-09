import pytest

from lilikoi.errors import LilikoiError
from lilikoi.openocd import build_argv as openocd_argv
from lilikoi.protocol import encode_err, encode_ok, encode_request, parse_response
from lilikoi.settings import Settings, parse_rail_limits
from lilikoi.tsi import build_argv as tsi_argv


def test_request_roundtrip_and_banner_ignored() -> None:
    assert encode_request("t1", "rail", ("set", 0, 750, 650, 900)) == b"t1 rail set 0 750 650 900\n"
    assert parse_response(b"* lilikoi 0.1.0 ready\n") is None
    ok = parse_response(encode_ok("t1", {"mv": 750, "code": "0x46"}))
    assert ok is not None and ok.ok and ok.values == {"mv": "750", "code": "0x46"}
    err = parse_response(encode_err("t2", "RANGE", "rail=0 mv=950 limits=650..900"))
    assert err is not None and not err.ok and err.code == "RANGE" and "950" in (err.message or "")


def test_request_rejects_whitespace_and_bad_tokens() -> None:
    with pytest.raises(ValueError):
        encode_request("t 1", "ping")
    with pytest.raises(ValueError):
        encode_request("t1", "rail", ("set 0",))


def test_rail_limits_parsing() -> None:
    assert parse_rail_limits("650-900,-,700-800") == [(650, 900), None, (700, 800)]
    assert parse_rail_limits(None) == []
    with pytest.raises(LilikoiError):
        parse_rail_limits("650:900")


def test_settings_coercion() -> None:
    settings = Settings.from_dict({"tsi_baud": "115200", "selfcheck": "false", "rail_limits": "650-900"})
    assert settings.tsi_baud == 115200 and settings.selfcheck is False
    assert settings.rail_limits == [(650, 900)]


def test_argv_builders(tmp_path) -> None:
    elf = tmp_path / "a.elf"
    assert tsi_argv("uart_tsi", "/dev/cu.x", 921600, elf, True) == [
        "uart_tsi", "+tty=/dev/cu.x", "+baudrate=921600", "+selfcheck", str(elf)
    ]
    bit = tmp_path / "with space.bit"
    argv = openocd_argv("openocd", tmp_path / "fl.cfg", bit, "ABC123")
    assert argv[-1] == f"init; pld load 0 {{{bit}}}; shutdown"
    assert argv[argv.index("-c") + 1] == "adapter serial ABC123"
    with pytest.raises(LilikoiError):
        openocd_argv("openocd", tmp_path / "fl.cfg", tmp_path / "bad{name}.bit", None)
