#!/usr/bin/env python3
"""Backend for the Ohm Downloads Quickshell panel.

Reads/mutates the same SQLite database Ohm's C++ DownloadManager writes
(app/src/DownloadManager.cpp) and cancels downloads over the same
ohm.sock protocol the old downloads-tui Go app used
(downloads-tui/actions.go), so this is a drop-in replacement for that TUI's
data layer. Service.qml shells out to this script rather than touching
SQLite/sockets directly from QML.

Usage:
  downloads_helper.py list             -> JSON array of download rows
  downloads_helper.py clear-finished   -> delete all non-in-progress rows
  downloads_helper.py cancel <id>      -> ask the Ohm daemon to cancel
  downloads_helper.py open <path>      -> xdg-open the finished file
  downloads_helper.py reveal <path>    -> reveal the file in a file manager
"""

import json
import os
import socket
import sqlite3
import subprocess
import sys
from pathlib import Path

# Must stay in sync with DownloadManager.h's `enum class State` and the
# ordinals the old downloads-tui/db.go hand-mirrored -- this is a
# cross-language contract with no compile-time check on either side.
STATE_IN_PROGRESS = 0
STATE_COMPLETED = 1
STATE_INTERRUPTED = 2
STATE_CANCELLED = 3


def data_home() -> Path:
    xdg = os.environ.get("XDG_DATA_HOME")
    base = Path(xdg) if xdg else Path.home() / ".local" / "share"
    return base / "ohm"


def db_path() -> Path:
    return data_home() / "downloads.sqlite"


def socket_path() -> Path:
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
    if runtime_dir:
        return Path(runtime_dir) / "ohm.sock"
    return Path("/tmp/ohm.sock")


def cmd_list() -> int:
    path = db_path()
    if not path.exists():
        # No downloads yet -- a normal empty state, not an error.
        print("[]")
        return 0
    try:
        # Not opened read-only: a WAL-mode DB's -shm sidecar can need write
        # access even for reads, and this connection never issues a write
        # anyway (see cmd_clear_finished for the one query that does).
        conn = sqlite3.connect(str(path), timeout=2.0)
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            "SELECT id, filename, path, url, total_bytes, received_bytes, "
            "state, interrupt_reason, started_at FROM downloads "
            "ORDER BY started_at DESC"
        ).fetchall()
        conn.close()
    except sqlite3.Error as exc:
        # Print nothing and fail non-zero -- Service.qml must keep its last
        # known-good list rather than treating this as "zero downloads".
        # (This exact bug shipped once in the Go TUI; see db_test.go's
        # TestConcurrentReadsNeverSeeAFalseEmptyList in the old downloads-tui.)
        print(f"downloads_helper: list failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps([dict(row) for row in rows]))
    return 0


def cmd_clear_finished() -> int:
    path = db_path()
    if not path.exists():
        return 0
    try:
        conn = sqlite3.connect(str(path), timeout=2.0)
        conn.execute("DELETE FROM downloads WHERE state != ?", (STATE_IN_PROGRESS,))
        conn.commit()
        conn.close()
    except sqlite3.Error as exc:
        print(f"downloads_helper: clear-finished failed: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_cancel(download_id: str) -> int:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.5)
            sock.connect(str(socket_path()))
            sock.sendall(f"CANCEL_DOWNLOAD {download_id}\n".encode())
            try:
                sock.recv(64)  # best-effort; the daemon's reply is ignored
            except socket.timeout:
                pass
    except OSError as exc:
        print(f"downloads_helper: cancel failed: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_open(target: str) -> int:
    subprocess.Popen(
        ["xdg-open", target],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return 0


def cmd_reveal(target: str) -> int:
    uri = "file://" + str(Path(target))
    result = subprocess.run(
        [
            "dbus-send",
            "--session",
            "--print-reply",
            "--dest=org.freedesktop.FileManager1",
            "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1.ShowItems",
            f"array:string:{uri}",
            "string:",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if result.returncode != 0:
        return cmd_open(str(Path(target).parent))
    return 0


def main(argv: list) -> int:
    if len(argv) < 2:
        print("downloads_helper: missing subcommand", file=sys.stderr)
        return 2
    action, args = argv[1], argv[2:]
    if action == "list":
        return cmd_list()
    if action == "clear-finished":
        return cmd_clear_finished()
    if action == "cancel" and args:
        return cmd_cancel(args[0])
    if action == "open" and args:
        return cmd_open(args[0])
    if action == "reveal" and args:
        return cmd_reveal(args[0])
    print(f"downloads_helper: unknown command: {' '.join(argv[1:])}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
