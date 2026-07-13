#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]


class Client:
    def __init__(self, port: int, timeout: float = 30.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)
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


def require_row(response: str, *values: int) -> None:
    expected = [str(value) for value in values]
    for line in response.splitlines():
        if not line.strip().startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if cells == expected:
            return
    raise AssertionError(f"expected row {expected}, got: {response!r}")


def wait_for_server(port: int, proc: subprocess.Popen[str], timeout: float = 30.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited during startup with status {proc.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"server did not listen on port {port}")


def read_rss_kib(pid: int) -> int:
    status = Path(f"/proc/{pid}/status").read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^VmRSS:\s+(\d+)\s+kB$", status, flags=re.MULTILINE)
    return int(match.group(1)) if match else 0


def start_server(server: Path, build_dir: Path, db_name: str, port: int, log_path: Path):
    log_file = log_path.open("w", encoding="utf-8")
    env = os.environ.copy()
    env["RMDB_PORT"] = str(port)
    proc = subprocess.Popen(
        [str(server), db_name],
        cwd=build_dir,
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        text=True,
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


def stop_server(proc: subprocess.Popen[str], log_file) -> None:
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    log_file.close()
    if proc.returncode != 0:
        raise RuntimeError(f"server exited with status {proc.returncode}")


def phase_one(port: int, rows: int, server: Path, build_dir: Path, db_name: str, log_path: Path) -> int:
    proc, log_file = start_server(server, build_dir, db_name, port, log_path)
    clients = []
    try:
        admin = Client(port)
        snapshot = Client(port)
        serial_one = Client(port)
        serial_two = Client(port)
        clients.extend([admin, snapshot, serial_one, serial_two])
        require_success(admin.execute("set output_file off;"), "set output_file off")
        require_success(admin.execute("create table mvcc_guard (id int, val int);"), "create mvcc_guard")
        require_success(admin.execute("create table churn (id int, val int);"), "create churn")
        require_success(admin.execute("create table duty (id int, on_call int);"), "create duty")
        require_success(admin.execute("insert into mvcc_guard values (1, 100);"), "insert guard")
        require_success(admin.execute("insert into duty values (1, 1);"), "insert duty 1")
        require_success(admin.execute("insert into duty values (2, 1);"), "insert duty 2")

        require_success(snapshot.execute("set transaction isolation level snapshot isolation;"), "set SI")
        require_success(snapshot.execute("begin;"), "SI begin")
        require_row(snapshot.execute("select * from mvcc_guard where id = 1;"), 1, 100)
        require_success(admin.execute("update mvcc_guard set val = 200 where id = 1;"), "update guard")

        require_success(admin.execute("begin;"), "churn begin")
        for row_id in range(rows):
            sql = f"insert into churn values ({row_id}, {row_id});"
            require_success(admin.execute(sql), sql)
        require_success(admin.execute("commit;"), "churn commit")

        require_row(snapshot.execute("select * from mvcc_guard where id = 1;"), 1, 100)
        require_success(snapshot.execute("commit;"), "SI commit")
        require_row(admin.execute("select * from mvcc_guard where id = 1;"), 1, 200)

        for client in (serial_one, serial_two):
            require_success(client.execute("set transaction isolation level serializable;"), "set SER")
            require_success(client.execute("begin;"), "SER begin")
        require_row(serial_one.execute("select * from duty where id = 2;"), 2, 1)
        require_row(serial_two.execute("select * from duty where id = 1;"), 1, 1)
        require_success(serial_one.execute("update duty set on_call = 0 where id = 1;"), "SER update one")
        serial_two_result = serial_two.execute("update duty set on_call = 0 where id = 2;")
        if "abort" not in serial_two_result.lower():
            raise AssertionError(f"expected SSI abort, got: {serial_two_result!r}")
        require_success(serial_one.execute("commit;"), "SER commit one")
        require_row(admin.execute("select * from duty where id = 1;"), 1, 0)
        require_row(admin.execute("select * from duty where id = 2;"), 2, 1)
        return read_rss_kib(proc.pid)
    finally:
        for client in clients:
            client.close()
        stop_server(proc, log_file)


def phase_two(port: int, rows: int, server: Path, build_dir: Path, db_name: str, log_path: Path) -> int:
    proc, log_file = start_server(server, build_dir, db_name, port, log_path)
    admin = None
    try:
        admin = Client(port, timeout=60.0)
        require_success(admin.execute("set output_file off;"), "set output_file off")
        require_success(admin.execute("begin;"), "delete begin")
        require_success(admin.execute("delete from churn;"), "delete churn")
        require_success(admin.execute("commit;"), "delete commit")

        require_success(admin.execute("begin;"), "reuse begin")
        for row_id in range(rows, rows * 2):
            sql = f"insert into churn values ({row_id}, {row_id});"
            require_success(admin.execute(sql), sql)
        require_success(admin.execute("commit;"), "reuse commit")
        require_row(admin.execute("select count(*) from churn;"), rows)
        return read_rss_kib(proc.pid)
    finally:
        if admin is not None:
            admin.close()
        stop_server(proc, log_file)


def main() -> int:
    parser = argparse.ArgumentParser(description="Targeted watermark/MVCC GC regression")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--server", type=Path)
    parser.add_argument("--port", type=int, default=18774)
    parser.add_argument("--rows", type=int, default=1200)
    parser.add_argument("--keep-db", action="store_true")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    server = args.server.resolve() if args.server else build_dir / "bin" / "rmdb"
    db_name = "mvcc_gc_regression_db"
    db_dir = build_dir / db_name
    if db_dir.exists():
        shutil.rmtree(db_dir)

    phase_one_log = build_dir / "mvcc_gc_phase_one.server.log"
    phase_two_log = build_dir / "mvcc_gc_phase_two.server.log"
    try:
        first_rss = phase_one(args.port, args.rows, server, build_dir, db_name, phase_one_log)
        churn_file = db_dir / "churn"
        first_size = churn_file.stat().st_size
        second_rss = phase_two(args.port, args.rows, server, build_dir, db_name, phase_two_log)
        second_size = churn_file.stat().st_size
        if second_size > first_size:
            raise AssertionError(
                f"deleted slots were not reused: churn file grew from {first_size} to {second_size} bytes"
            )
        print(
            f"PASS: SI watermark, SSI lifetime, and DELETE slot reuse; "
            f"churn_bytes={first_size}->{second_size}, rss_kib={first_rss}->{second_rss}"
        )
        return 0
    finally:
        if not args.keep_db and db_dir.exists():
            shutil.rmtree(db_dir)


if __name__ == "__main__":
    raise SystemExit(main())
