"""RP2350 line protocol, shared by the serial link and the fake MCU used in tests.

    request   <token> <command> [args...]\\n
    response  <token> OK [key=value ...]\\n
              <token> ERR <CODE> <message>\\n
    async     * <text>\\n
"""

import re
from dataclasses import dataclass, field

TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{1,16}$")
PROTO_VERSION = 1


@dataclass
class Response:
    token: str
    ok: bool
    values: dict[str, str] = field(default_factory=dict)
    code: str | None = None
    message: str | None = None


def encode_request(token: str, command: str, args: tuple[object, ...] = ()) -> bytes:
    if not TOKEN_RE.match(token):
        raise ValueError(f"invalid token {token!r}")
    parts = [token, command, *(str(a) for a in args)]
    for part in parts:
        if not part or any(c.isspace() for c in part):
            raise ValueError(f"request field may not be empty or contain whitespace: {part!r}")
    return (" ".join(parts) + "\n").encode()


def parse_kv(fields: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for item in fields:
        key, sep, value = item.partition("=")
        if sep and key:
            values[key] = value
    return values


def parse_response(line: str | bytes) -> Response | None:
    """Return None for banners, blank lines, and anything that is not a response."""
    if isinstance(line, bytes):
        line = line.decode(errors="replace")
    line = line.strip("\r\n")
    if not line or line.startswith("*"):
        return None
    parts = line.split(" ")
    if len(parts) < 2:
        return None
    token, status = parts[0], parts[1]
    if status == "OK":
        return Response(token, True, parse_kv(parts[2:]))
    if status == "ERR":
        code = parts[2] if len(parts) > 2 else "UNKNOWN"
        return Response(token, False, {}, code, " ".join(parts[3:]))
    return None


def encode_ok(token: str, values: dict[str, object] | None = None) -> bytes:
    body = " ".join(f"{k}={v}" for k, v in (values or {}).items())
    return (f"{token} OK {body}".rstrip() + "\n").encode()


def encode_err(token: str, code: str, message: str) -> bytes:
    return f"{token} ERR {code} {message}\n".encode()
