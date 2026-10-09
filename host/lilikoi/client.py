"""Connect to a board's daemon, spawning it on first use."""

import json
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from .daemon import paths_for
from .errors import LilikoiError

SPAWN_TIMEOUT = 8.0


def _connect(path: Path) -> socket.socket | None:
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        client.connect(str(path))
    except (FileNotFoundError, ConnectionRefusedError):
        client.close()
        return None
    return client


def _spawn(mcu_id: str, run_dir: Path) -> None:
    run_dir.mkdir(parents=True, exist_ok=True)
    log = paths_for(mcu_id, run_dir)["log"].open("ab")
    # Fully detached: fl-agent kills the CLI's process group on timeout, and the daemon
    # (with its uart_tsi child) must survive that.
    subprocess.Popen(
        [sys.executable, "-m", "lilikoi.daemon", "--mcu", mcu_id, "--run-dir", str(run_dir)],
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=log,
        start_new_session=True,
        close_fds=True,
    )
    log.close()


def request(mcu_id: str, run_dir: Path, op: str, args: dict[str, Any], settings: dict[str, Any]) -> Any:
    path = paths_for(mcu_id, run_dir)["socket"]
    client = _connect(path)
    if client is None:
        _spawn(mcu_id, run_dir)
        deadline = time.monotonic() + SPAWN_TIMEOUT
        while client is None and time.monotonic() < deadline:
            time.sleep(0.05)
            client = _connect(path)
        if client is None:
            raise LilikoiError("DAEMON_UNAVAILABLE", f"daemon did not start; see {paths_for(mcu_id, run_dir)['log']}")
    with client:
        client.sendall((json.dumps({"op": op, "args": args, "settings": settings}) + "\n").encode())
        chunks = []
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    raw = b"".join(chunks)
    if not raw:
        raise LilikoiError("DAEMON_CLOSED", "daemon closed the connection without replying")
    reply = json.loads(raw)
    if not reply.get("ok"):
        raise LilikoiError(reply.get("code", "ERROR"), reply.get("message", ""), **(reply.get("details") or {}))
    return reply.get("result")
