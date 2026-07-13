#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import time
from typing import Optional


ROOT = Path(__file__).resolve().parents[1]


class Client:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10.0)
        self.pending = b""

    def close(self) -> None:
        self.sock.close()

    def execute(self, sql: str) -> str:
        self.sock.sendall(sql.encode("utf-8") + b"\0")
        while b"\0" not in self.pending:
            chunk = self.sock.recv(1024 * 1024)
            if not chunk:
                raise RuntimeError(f"server closed connection while executing: {sql}")
            self.pending += chunk
        frame, self.pending = self.pending.split(b"\0", 1)
        return frame.decode("utf-8", errors="replace")


def require_success(response: str, sql: str) -> None:
    lowered = response.lower()
    if "failure" in lowered or "abort" in lowered:
        raise AssertionError(f"unexpected response to {sql!r}: {response!r}")


def require_count(response: str, expected: int) -> None:
    for line in response.splitlines():
        if not line.strip().startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if cells == [str(expected)]:
            return
    raise AssertionError(f"expected count {expected}, got: {response!r}")


def wait_for_server(port: int, proc: subprocess.Popen, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited during startup with status {proc.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not accept connections")


def start_server(server: Path, build_dir: Path, db_name: str, port: int, log_path: Path,
                 wal_reset_bytes: int, failpoint: Optional[str] = None):
    env = os.environ.copy()
    env.update(
        RMDB_PORT=str(port),
        RMDB_BUFFER_POOL_PAGES="128",
        RMDB_WAL_RESET_BYTES=str(wal_reset_bytes),
    )
    if failpoint is not None:
        env["RMDB_WAL_RESET_FAILPOINT"] = failpoint
    log_file = log_path.open("w", encoding="utf-8")
    proc = subprocess.Popen(
        [str(server), db_name],
        cwd=build_dir,
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )
    try:
        wait_for_server(port, proc)
    except Exception:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        log_file.close()
        raise
    return proc, log_file


def stop_server(proc: subprocess.Popen, log_file) -> None:
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=20)
    log_file.close()
    if proc.returncode != 0:
        raise RuntimeError(f"server exited with status {proc.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Committed DELETE WAL reset regression")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--server", type=Path)
    parser.add_argument("--port", type=int, default=18795)
    parser.add_argument("--keep-db", action="store_true")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    server = args.server.resolve() if args.server else build_dir / "bin" / "rmdb"
    db_name = "wal_reset_delete_regression_db"
    db_dir = build_dir / db_name
    logs = [build_dir / f"wal_reset_delete_phase_{phase}.server.log" for phase in range(1, 4)]
    if db_dir.exists():
        shutil.rmtree(db_dir)

    proc = None
    log_file = None
    try:
        proc, log_file = start_server(server, build_dir, db_name, args.port, logs[0], 0)
        client = Client(args.port)
        try:
            require_success(client.execute("set output_file off;"), "set output_file off")
            require_success(client.execute("create table t (id int);"), "create table")
            require_success(client.execute("create index t(id);"), "create index")
            require_success(client.execute("insert into t values (1);"), "insert")
        finally:
            client.close()
        stop_server(proc, log_file)
        proc = log_file = None

        proc, log_file = start_server(
            server, build_dir, db_name, args.port, logs[1], 1, "after_truncate"
        )
        client = Client(args.port)
        try:
            require_success(client.execute("delete from t where id = 1;"), "delete")
        finally:
            client.close()
        proc.wait(timeout=20)
        log_file.close()
        log_file = None
        if proc.returncode != 86:
            raise AssertionError(f"expected failpoint exit 86, got {proc.returncode}")
        proc = None

        proc, log_file = start_server(server, build_dir, db_name, args.port, logs[2], 0)
        client = Client(args.port)
        try:
            require_count(client.execute("select count(*) from t where id = 1;"), 0)
        finally:
            client.close()
        stop_server(proc, log_file)
        proc = log_file = None
        print("PASS: committed DELETE survives WAL reset after_truncate crash")
        return 0
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        if log_file is not None:
            log_file.close()
        if not args.keep_db:
            shutil.rmtree(db_dir, ignore_errors=True)
            for path in logs:
                try:
                    path.unlink()
                except FileNotFoundError:
                    pass


if __name__ == "__main__":
    raise SystemExit(main())
