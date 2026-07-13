#!/usr/bin/env python3
"""Run local RMDB tests transcribed from 测试说明文档.md.

The runner intentionally does not modify CMake files.  It starts the compiled
server, sends SQL over the same TCP protocol as rmdb_client, then compares the
database output.txt with the expected result file.
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Literal


ROOT = Path(__file__).resolve().parents[1]
TEST_ROOT = Path(__file__).resolve().parent
CASE_DIR = TEST_ROOT / "cases"
EXPECTED_DIR = TEST_ROOT / "expected"
DEFAULT_BUILD_DIR = ROOT / "build"
DEFAULT_PORT = 8765


@dataclass(frozen=True)
class TestCase:
    name: str
    sql_path: Path | None
    expected_path: Path | None
    kind: str = "static"


@dataclass
class TestResult:
    case: TestCase
    passed: bool
    detail: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run RMDB document-based SQL tests.")
    parser.add_argument("cases", nargs="*", help="Case names without .sql, or glob fragments.")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--server", type=Path, help="Path to rmdb server binary.")
    parser.add_argument("--timeout", type=float, default=20.0, help="Per-case timeout in seconds.")
    parser.add_argument("--connect-timeout", type=float, default=5.0)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="TCP port used by the test server.")
    parser.add_argument("--keep-db", action="store_true", help="Keep generated test databases.")
    parser.add_argument("--list", action="store_true", help="List discovered cases and exit.")
    parser.add_argument("--quiet", action="store_true", help="Only print details for failing cases.")
    parser.add_argument("--show-server-log", action="store_true")
    parser.add_argument("--row-count", type=int, default=3000, help="Rows used by generated index performance tests.")
    parser.add_argument("--join-row-count", type=int, default=1000, help="Rows per side used by generated join performance tests.")
    parser.add_argument("--recovery-row-count", type=int, default=200, help="Rows used by generated recovery tests.")
    parser.add_argument("--index-ratio", type=float, default=0.70, help="Required indexed/non-indexed time ratio.")
    parser.add_argument("--join-ratio", type=float, default=0.50, help="Required INLJ/NLJ time ratio.")
    parser.add_argument("--recovery-ratio", type=float, default=0.70, help="Required checkpoint/no-checkpoint recovery ratio.")
    return parser.parse_args()


def discover_cases(filters: list[str]) -> list[TestCase]:
    cases: list[TestCase] = []
    for sql_path in sorted(CASE_DIR.glob("*.sql")):
        expected_path = EXPECTED_DIR / f"{sql_path.stem}.expected"
        if not expected_path.exists():
            continue
        if filters and not any(token in sql_path.stem for token in filters):
            continue
        cases.append(TestCase(sql_path.stem, sql_path, expected_path))
    generated: list[TestCase] = [
        TestCase("03_04_index_perf_single", None, None, "perf_single"),
        TestCase("03_05_index_perf_multi", None, None, "perf_multi"),
        TestCase("07_06_join_perf_inlj", None, None, "join_perf"),
        TestCase("08_01_transaction_commit", None, None, "txn_commit"),
        TestCase("08_02_transaction_abort", None, None, "txn_abort"),
        TestCase("08_03_transaction_commit_index", None, None, "txn_commit_index"),
        TestCase("08_04_transaction_abort_index", None, None, "txn_abort_index"),
        TestCase("08_18_rc_delete_abort_visibility", None, None, "txn_rc_delete_abort_visibility"),
        TestCase("08_19_abort_multitable_insert", None, None, "txn_abort_multitable_insert"),
        TestCase("08_20_explicit_error_aborts_multitable", None, None, "txn_error_aborts_multitable"),
        TestCase("08_21_connection_resilience", None, None, "connection_resilience"),
        TestCase("08_22_rc_update_visibility", None, None, "txn_rc_update_visibility"),
        TestCase("10_01_crash_recovery_single_thread", None, None, "recovery_single"),
        TestCase("10_02_crash_recovery_multi_thread", None, None, "recovery_multi"),
        TestCase("10_03_crash_recovery_index", None, None, "recovery_index"),
        TestCase("10_04_crash_recovery_large_data", None, None, "recovery_large"),
        TestCase("10_05_crash_recovery_without_checkpoint", None, None, "recovery_without_checkpoint"),
        TestCase("10_06_crash_recovery_with_checkpoint", None, None, "recovery_with_checkpoint"),
        TestCase("10_07_recovery_undo_uncommitted", None, None, "recovery_script_undo_uncommitted"),
        TestCase("10_08_recovery_redo_committed", None, None, "recovery_script_redo_committed"),
        TestCase("10_09_recovery_index_consistency", None, None, "recovery_script_index_consistency"),
        TestCase("10_10_recovery_checkpoint_boundary", None, None, "recovery_script_checkpoint_boundary"),
        TestCase("10_11_recovery_restart_new_writes", None, None, "recovery_script_restart_new_writes"),
        TestCase("10_12_recovery_multitable_atomic", None, None, "recovery_script_multitable_atomic"),
        TestCase("10_13_recovery_log_validation", None, None, "recovery_log_validation"),
        TestCase("10_14_recovery_streaming_log", None, None, "recovery_streaming_log"),
    ]
    for case in generated:
        if filters and not any(token in case.name for token in filters):
            continue
        cases.append(case)
    cases.sort(key=lambda case: case.name)
    return cases


def strip_line_comment(line: str) -> str:
    in_string = False
    out: list[str] = []
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == "'":
            in_string = not in_string
            out.append(ch)
            i += 1
            continue
        if not in_string and ch == "-" and i + 1 < len(line) and line[i + 1] == "-":
            break
        out.append(ch)
        i += 1
    return "".join(out)


def split_sql(sql_text: str) -> list[str]:
    statements: list[str] = []
    current: list[str] = []
    in_string = False
    for raw_line in sql_text.splitlines():
        line = strip_line_comment(raw_line)
        for ch in line:
            current.append(ch)
            if ch == "'":
                in_string = not in_string
            elif ch == ";" and not in_string:
                statement = "".join(current).strip()
                if statement:
                    statements.append(" ".join(statement.split()))
                current.clear()
        current.append(" ")
    tail = "".join(current).strip()
    if tail:
        statements.append(" ".join(tail.split()))
    return statements


def is_table_line(line: str) -> bool:
    return line.startswith("|") and line.endswith("|")


def split_table_cells(line: str) -> list[str]:
    return [cell.strip() for cell in line.strip("|").split("|")]


def is_header_like(line: str) -> bool:
    if not is_table_line(line):
        return False
    cells = split_table_cells(line)
    if not cells:
        return False
    return all(re.fullmatch(r"[A-Za-z_][A-Za-z_]*", cell) for cell in cells)


def is_plan_line(line: str) -> bool:
    stripped = line.lstrip("\t")
    return stripped.startswith(("Project(", "Filter(", "Join(", "Scan("))


def normalize_output(
    text: str,
    *,
    preserve_table_order: bool = False,
    preserve_plan_indentation: bool = False,
) -> list[str]:
    normalized: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if set(stripped) <= {"+", "-"}:
            continue
        if stripped.startswith("record count:"):
            continue
        if preserve_plan_indentation and is_plan_line(line):
            normalized.append(line.rstrip())
        else:
            normalized.append(stripped)
    if preserve_table_order:
        return normalized

    canonical: list[str] = []
    current_header: str | None = None
    current_rows: list[str] = []

    def flush_table() -> None:
        nonlocal current_header, current_rows
        if current_header is not None:
            canonical.append(current_header)
            canonical.extend(sorted(current_rows))
            current_header = None
            current_rows = []

    for line in normalized:
        if is_header_like(line):
            flush_table()
            current_header = line
            current_rows = []
        elif is_table_line(line) and current_header is not None:
            current_rows.append(line)
        else:
            flush_table()
            canonical.append(line)
    flush_table()
    return canonical


def canonicalize_case_output(case_name: str, lines: list[str]) -> list[str]:
    canonical = list(lines)
    if case_name in {"03_03_index_maintenance"}:
        canonical = collapse_consecutive_duplicates(canonical)
    if case_name == "04_02_projection_pushdown" and len(canonical) >= 2:
        head = canonical[:2]
        tail = canonical[2:]
        if len(tail) == 4 and all(line.startswith(("Project(", "Scan(")) for line in tail):
            canonical = head + sorted(tail)
    return canonical


def collapse_consecutive_duplicates(lines: list[str]) -> list[str]:
    collapsed: list[str] = []
    for line in lines:
        if collapsed and collapsed[-1] == line:
            continue
        collapsed.append(line)
    return collapsed


def split_table_blocks(lines: list[str]) -> list[list[str]]:
    blocks: list[list[str]] = []
    current: list[str] = []
    for line in lines:
        if is_header_like(line):
            if current:
                blocks.append(current)
            current = [line]
        elif current:
            current.append(line)
    if current:
        blocks.append(current)
    return blocks


def wait_for_server(port: int, timeout: float) -> None:
    deadline = time.time() + timeout
    last_error: OSError | None = None
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"server did not accept connections on port {port}: {last_error}")


class RmdbClient:
    def __init__(self, port: int, timeout: float):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)

    def close(self) -> None:
        self.sock.close()

    def execute(self, statement: str) -> bytes:
        payload = statement[:-1].strip() if statement.lower() == "crash;" else statement
        self.sock.sendall(payload.encode("utf-8") + b"\0")
        if payload == "crash":
            return b""
        try:
            response = self.sock.recv(1024 * 1024)
        except socket.timeout:
            raise RuntimeError(f"timed out waiting for response to: {statement}")
        if not response:
            raise RuntimeError(f"server closed connection while handling: {statement}")
        return response


def send_sql(statements: list[str], port: int, timeout: float) -> None:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        for statement in statements:
            payload = statement[:-1].strip() if statement.lower() == "crash;" else statement
            sock.sendall(payload.encode("utf-8") + b"\0")
            if payload == "crash":
                return
            try:
                response = sock.recv(1024 * 1024)
            except socket.timeout:
                raise RuntimeError(f"timed out waiting for response to: {statement}")
            if not response:
                raise RuntimeError(f"server closed connection while handling: {statement}")


def output_candidates(build_dir: Path, db_name: str) -> list[Path]:
    return [
        build_dir / db_name / "output.txt",
        build_dir / "output.txt",
        ROOT / db_name / "output.txt",
        ROOT / "output.txt",
    ]


def read_actual_output(build_dir: Path, db_name: str) -> str:
    for path in output_candidates(build_dir, db_name):
        if path.exists():
            return path.read_text(encoding="utf-8", errors="replace")
    return ""


def cleanup_outputs(build_dir: Path, db_name: str) -> None:
    for path in output_candidates(build_dir, db_name):
        if path.exists():
            path.unlink()


def cleanup_db(build_dir: Path, db_name: str) -> None:
    for path in [build_dir / db_name, ROOT / db_name]:
        if path.exists():
            shutil.rmtree(path)


def start_server(
    server: Path,
    build_dir: Path,
    db_name: str,
    server_log: Path,
    extra_env: dict[str, str] | None = None,
) -> subprocess.Popen[str]:
    log_file = server_log.open("w", encoding="utf-8")
    env = os.environ.copy()
    env["RMDB_PORT"] = str(DEFAULT_PORT)
    if extra_env is not None:
        env.update(extra_env)
    return subprocess.Popen(
        [str(server), db_name],
        cwd=build_dir,
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        text=True,
    )


def terminate_server(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def recv_protocol_frames(sock: socket.socket, count: int) -> list[bytes]:
    pending = b""
    frames: list[bytes] = []
    while len(frames) < count:
        chunk = sock.recv(1024 * 1024)
        if not chunk:
            raise RuntimeError("server closed connection before completing a response frame")
        pending += chunk
        while b"\0" in pending and len(frames) < count:
            frame, pending = pending.split(b"\0", 1)
            frames.append(frame)
    return frames


def response_has_int_row(response: bytes, expected_id: int, expected_value: int) -> bool:
    text = response.decode("utf-8", errors="replace")
    for line in text.splitlines():
        if not is_table_line(line.strip()):
            continue
        cells = split_table_cells(line.strip())
        if cells == [str(expected_id), str(expected_value)]:
            return True
    return False


def read_process_thread_count(pid: int) -> int | None:
    status_path = Path(f"/proc/{pid}/status")
    if not status_path.exists():
        return None
    for line in status_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("Threads:"):
            return int(line.split()[1])
    return None


def read_process_rss_kb(pid: int) -> int | None:
    status_path = Path(f"/proc/{pid}/status")
    if not status_path.exists():
        return None
    for line in status_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    return None


def run_connection_resilience_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)
    server_log = TEST_ROOT / f"{case.name}.server.log"
    restart_log = TEST_ROOT / f"{case.name}.restart.server.log"
    proc: subprocess.Popen[str] | None = None
    clients: list[RmdbClient] = []

    def new_client() -> RmdbClient:
        client = RmdbClient(DEFAULT_PORT, timeout)
        clients.append(client)
        return client

    def close_client(client: RmdbClient) -> None:
        client.close()
        if client in clients:
            clients.remove(client)

    def assert_current_value(expected: int) -> None:
        client = new_client()
        response = client.execute("select * from connection_guard;")
        close_client(client)
        if not response_has_int_row(response, 1, expected):
            raise RuntimeError(f"expected connection_guard value {expected}, got {response!r}")

    try:
        proc = start_server(
            server,
            build_dir,
            db_name,
            server_log,
            {
                "RMDB_TXN_IDLE_TIMEOUT_SECONDS": "1",
                "RMDB_CONNECTION_IDLE_TIMEOUT_SECONDS": "5",
            },
        )
        wait_for_server(DEFAULT_PORT, max(connect_timeout, 20.0))

        setup = new_client()
        setup.execute("create table connection_guard (id int, val int);")
        setup.execute("insert into connection_guard values (1, 10);")
        close_client(setup)

        # A command may span many TCP reads, and multiple NUL-delimited commands may share one read.
        framed = socket.create_connection(("127.0.0.1", DEFAULT_PORT), timeout=timeout)
        framed.settimeout(timeout)
        for byte in b"select * from connection_guard;\0":
            framed.send(bytes([byte]))
        fragmented = recv_protocol_frames(framed, 1)[0]
        if not response_has_int_row(fragmented, 1, 10):
            raise RuntimeError(f"fragmented command returned {fragmented!r}")
        framed.sendall(b"select * from connection_guard;\0select * from connection_guard;\0")
        coalesced = recv_protocol_frames(framed, 2)
        if not all(response_has_int_row(response, 1, 10) for response in coalesced):
            raise RuntimeError(f"coalesced commands returned {coalesced!r}")
        framed.close()

        # The pinned TPCC-Tester omits request NULs. Its SQL still has a lexical terminator,
        # except for this setup command, so compatibility must not depend on recv boundaries.
        legacy = socket.create_connection(("127.0.0.1", DEFAULT_PORT), timeout=timeout)
        legacy.settimeout(timeout)
        legacy.sendall(b"set output_file off")
        recv_protocol_frames(legacy, 1)
        for byte in b"select * from connection_guard;":
            legacy.send(bytes([byte]))
        legacy_response = recv_protocol_frames(legacy, 1)[0]
        if not response_has_int_row(legacy_response, 1, 10):
            raise RuntimeError(f"legacy TPCC command returned {legacy_response!r}")
        legacy.close()

        oversized = socket.create_connection(("127.0.0.1", DEFAULT_PORT), timeout=timeout)
        oversized.settimeout(timeout)
        oversized.sendall(b"x" * 9000)
        if recv_protocol_frames(oversized, 1)[0] != b"failure\n":
            raise RuntimeError("oversized command was not rejected")
        oversized.close()

        # EOF and RST must abort explicit transactions and release their locks.
        disconnected = new_client()
        disconnected.execute("begin;")
        disconnected.execute("update connection_guard set val = 99 where id = 1;")
        close_client(disconnected)
        time.sleep(0.1)
        assert_current_value(10)

        rst_txn = new_client()
        rst_txn.execute("begin;")
        rst_txn.execute("update connection_guard set val = 88 where id = 1;")
        rst_txn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        close_client(rst_txn)
        time.sleep(0.1)
        assert_current_value(10)

        # A reset before response delivery must not terminate the whole server via SIGPIPE.
        rst_writer = socket.create_connection(("127.0.0.1", DEFAULT_PORT), timeout=timeout)
        rst_writer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        rst_writer.sendall(b"select * from connection_guard;\0")
        rst_writer.close()
        time.sleep(0.1)
        if proc.poll() is not None:
            raise RuntimeError(f"server exited after client RST: {proc.returncode}")

        idle = new_client()
        idle.execute("begin;")
        idle.execute("update connection_guard set val = 77 where id = 1;")
        idle.sock.settimeout(0.5)
        time.sleep(1.2)
        try:
            idle_result = idle.sock.recv(1)
        except ConnectionResetError:
            idle_result = b""
        except socket.timeout as exc:
            raise RuntimeError("explicit transaction idle timeout did not close the connection") from exc
        if idle_result != b"":
            raise RuntimeError(f"unexpected data after explicit transaction idle timeout: {idle_result!r}")
        close_client(idle)
        assert_current_value(10)

        committed = new_client()
        committed.execute("begin;")
        committed.execute("update connection_guard set val = 20 where id = 1;")
        committed.execute("commit;")
        close_client(committed)
        assert_current_value(20)

        for _ in range(100):
            churn = socket.create_connection(("127.0.0.1", DEFAULT_PORT), timeout=timeout)
            churn.close()
        thread_count = None
        deadline = time.time() + 3.0
        while time.time() < deadline:
            thread_count = read_process_thread_count(proc.pid)
            if thread_count is None or thread_count <= 2:
                break
            time.sleep(0.05)
        if thread_count is not None and thread_count > 2:
            raise RuntimeError(f"detached client handlers were not reclaimed: threads={thread_count}")

        active_at_shutdown = new_client()
        active_at_shutdown.execute("begin;")
        active_at_shutdown.execute("update connection_guard set val = 66 where id = 1;")
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=max(20.0, connect_timeout))
        close_client(active_at_shutdown)
        if proc.returncode != 0:
            raise RuntimeError(f"server failed graceful shutdown: {proc.returncode}")

        proc = start_server(server, build_dir, db_name, restart_log)
        wait_for_server(DEFAULT_PORT, max(connect_timeout, 20.0))
        assert_current_value(20)
    except Exception as exc:
        detail = f"runtime error: {exc}"
        for log_path in [server_log, restart_log]:
            if show_server_log and log_path.exists():
                detail += f"\n\n--- {log_path.name} tail ---\n"
                detail += log_path.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        for client in clients:
            client.close()
        if proc is not None:
            terminate_server(proc)

    if not show_server_log:
        for log_path in [server_log, restart_log]:
            if log_path.exists():
                log_path.unlink()
    if not keep_db:
        cleanup_db(build_dir, db_name)
    return TestResult(case, True, "passed")


def preserve_table_order_for_case(case_name: str) -> bool:
    return case_name.startswith("08_") or case_name.startswith("09_") or case_name in {
        "05_02_group_by_having",
        "05_04_order_limit",
        "05_05_group_multi_hidden",
        "06_01_union_order",
        "06_02_union_compat",
    }


def preserve_plan_indentation_for_case(case_name: str) -> bool:
    return case_name.startswith("04_") or case_name.startswith("07_")


def compare_lines(
    expected: list[str],
    actual_text: str,
    case_name: str,
    *,
    preserve_table_order: bool = False,
    preserve_plan_indentation: bool = False,
) -> str | None:
    expected = canonicalize_case_output(case_name, expected)
    actual = canonicalize_case_output(
        case_name,
        normalize_output(
            actual_text,
            preserve_table_order=preserve_table_order,
            preserve_plan_indentation=preserve_plan_indentation,
        ),
    )
    if expected == actual:
        return None
    return "\n".join(
        difflib.unified_diff(
            expected,
            actual,
            fromfile=f"expected/{case_name}.expected",
            tofile=f"actual/{case_name}.output",
            lineterm="",
        )
    ) or "output mismatch"


def run_statements_with_server(
    case: TestCase,
    statements: list[str],
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
    expected: list[str] | None = None,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)

    server_log = TEST_ROOT / f"{case.name}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        send_sql(statements, DEFAULT_PORT, timeout)
        time.sleep(0.1)
    except Exception as exc:
        terminate_server(proc)
        detail = f"runtime error: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        terminate_server(proc)

    if expected is None:
        return TestResult(case, True, "passed")
    detail = compare_lines(
        expected,
        read_actual_output(build_dir, db_name),
        case.name,
        preserve_table_order=preserve_table_order_for_case(case.name),
        preserve_plan_indentation=preserve_plan_indentation_for_case(case.name),
    )
    if detail is None:
        if not show_server_log and server_log.exists():
            server_log.unlink()
        if not keep_db:
            cleanup_db(build_dir, db_name)
        return TestResult(case, True, "passed")
    if show_server_log and server_log.exists():
        detail += "\n\n--- server log tail ---\n"
        detail += server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
    return TestResult(case, False, detail)


def run_index_perf_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
    row_count: int,
    index_ratio: float,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)
    server_log = TEST_ROOT / f"{case.name}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    client: RmdbClient | None = None
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        client = RmdbClient(DEFAULT_PORT, timeout)
        if case.kind == "perf_single":
            client.execute("create table warehouse (w_id int,name char(8));")
            for i in range(1, row_count + 1):
                client.execute(f"insert into warehouse values({i},'{i % 100000000:08d}');")
            selects = [f"select * from warehouse where w_id = {i};" for i in range(1, row_count + 1)]
            index_stmt = "create index warehouse(w_id);"
        else:
            client.execute("create table warehouse (w_id int,name char(8),flo float);")
            for i in range(1, row_count + 1):
                flo = [1024.5, 512.5, 256.5, 128.5][i % 4]
                client.execute(f"insert into warehouse values({i},'{i % 100000000:08d}',{flo:.6f});")
            selects = [
                f"select * from warehouse where w_id = {i} and flo = {[1024.5, 512.5, 256.5, 128.5][i % 4]:.6f};"
                for i in range(1, row_count + 1)
            ]
            index_stmt = "create index warehouse(w_id,flo);"

        sample_keys = sorted({1, max(1, row_count // 2), row_count})
        sample_statements = [selects[key - 1] for key in sample_keys]
        if case.kind == "perf_single":
            expected_sample = []
            for key in sample_keys:
                expected_sample.extend([
                    "| w_id | name |",
                    f"| {key} | {key % 100000000:08d} |",
                ])
        else:
            expected_sample = []
            for key in sample_keys:
                flo = [1024.5, 512.5, 256.5, 128.5][key % 4]
                expected_sample.extend([
                    "| w_id | name | flo |",
                    f"| {key} | {key % 100000000:08d} | {flo:.6f} |",
                ])

        cleanup_outputs(build_dir, db_name)
        for stmt in sample_statements:
            client.execute(stmt)
        detail = compare_lines(expected_sample, read_actual_output(build_dir, db_name), case.name)
        if detail is not None:
            return TestResult(case, False, "non-indexed result mismatch:\n" + detail)
        cleanup_outputs(build_dir, db_name)

        start = time.perf_counter()
        for stmt in selects:
            client.execute(stmt)
        no_index_time = time.perf_counter() - start
        cleanup_outputs(build_dir, db_name)

        client.execute(index_stmt)
        for stmt in sample_statements:
            client.execute(stmt)
        detail = compare_lines(expected_sample, read_actual_output(build_dir, db_name), case.name)
        if detail is not None:
            return TestResult(case, False, "indexed result mismatch:\n" + detail)
        cleanup_outputs(build_dir, db_name)

        start = time.perf_counter()
        for stmt in selects:
            client.execute(stmt)
        index_time = time.perf_counter() - start
        ratio = index_time / no_index_time if no_index_time > 0 else float("inf")
        passed = ratio <= index_ratio
        detail = (
            f"indexed/non-indexed ratio={ratio:.3f}, indexed={index_time:.3f}s, "
            f"non_indexed={no_index_time:.3f}s, required<={index_ratio:.3f}"
        )
        return TestResult(case, passed, detail)
    except Exception as exc:
        detail = f"runtime error: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        if client is not None:
            client.close()
        terminate_server(proc)


def run_join_perf_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
    row_count: int,
    join_ratio: float,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)
    server_log = TEST_ROOT / f"{case.name}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    client: RmdbClient | None = None
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        client = RmdbClient(DEFAULT_PORT, timeout)
        client.execute("create table join_left (id int,name char(8));")
        client.execute("create table join_right (id int,tag char(8));")
        for i in range(1, row_count + 1):
            client.execute(f"insert into join_left values({i},'l{i % 10000000:07d}');")
            right_id = i if i % 2 == 0 else row_count + i
            client.execute(f"insert into join_right values({right_id},'r{right_id % 10000000:07d}');")

        matches = row_count // 2
        query = (
            "select join_left.name, join_right.tag from join_left "
            "join join_right on join_left.id = join_right.id;"
        )
        explain = "explain analyze " + query
        sample = (
            "select join_left.name, join_right.tag from join_left "
            "join join_right on join_left.id = join_right.id where join_left.id = 2;"
        )
        expected_sample = [
            "| name | tag |",
            "| l0000002 | r0000002 |",
        ]
        expected_nlj_plan = [
            f"Project(columns=[join_left.name, join_right.tag], rows={matches})",
            f"\tJoin(tables=[join_left, join_right], condition=[join_left.id=join_right.id], rows={matches})",
            f"\t\tProject(columns=[join_left.id, join_left.name], rows={row_count})",
            f"\t\t\tScan(table=join_left, type=SeqScan, rows={row_count})",
            f"\t\tProject(columns=[join_right.id, join_right.tag], rows={row_count * row_count})",
            f"\t\t\tScan(table=join_right, type=SeqScan, rows={row_count * row_count})",
        ]
        expected_inlj_plan = [
            f"Project(columns=[join_left.name, join_right.tag], rows={matches})",
            f"\tJoin(tables=[join_left, join_right], condition=[join_left.id=join_right.id], rows={matches})",
            f"\t\tProject(columns=[join_left.id, join_left.name], rows={row_count})",
            f"\t\t\tScan(table=join_left, type=SeqScan, rows={row_count})",
            f"\t\tProject(columns=[join_right.id, join_right.tag], rows={matches})",
            f"\t\t\tScan(table=join_right, type=IndexScan, using_index=(id), rows={matches})",
        ]

        cleanup_outputs(build_dir, db_name)
        client.execute(sample)
        detail = compare_lines(expected_sample, read_actual_output(build_dir, db_name), case.name)
        if detail is not None:
            return TestResult(case, False, "NLJ sample result mismatch:\n" + detail)

        cleanup_outputs(build_dir, db_name)
        client.execute(explain)
        detail = compare_lines(
            expected_nlj_plan,
            read_actual_output(build_dir, db_name),
            case.name,
            preserve_plan_indentation=True,
        )
        if detail is not None:
            return TestResult(case, False, "NLJ explain mismatch:\n" + detail)

        cleanup_outputs(build_dir, db_name)
        start = time.perf_counter()
        client.execute(query)
        nlj_time = time.perf_counter() - start

        cleanup_outputs(build_dir, db_name)
        client.execute("create index join_right(id);")
        client.execute(sample)
        detail = compare_lines(expected_sample, read_actual_output(build_dir, db_name), case.name)
        if detail is not None:
            return TestResult(case, False, "INLJ sample result mismatch:\n" + detail)

        cleanup_outputs(build_dir, db_name)
        client.execute(explain)
        detail = compare_lines(
            expected_inlj_plan,
            read_actual_output(build_dir, db_name),
            case.name,
            preserve_plan_indentation=True,
        )
        if detail is not None:
            return TestResult(case, False, "INLJ explain mismatch:\n" + detail)

        cleanup_outputs(build_dir, db_name)
        start = time.perf_counter()
        client.execute(query)
        inlj_time = time.perf_counter() - start
        ratio = inlj_time / nlj_time if nlj_time > 0 else float("inf")
        detail = (
            f"INLJ/NLJ ratio={ratio:.3f}, INLJ={inlj_time:.3f}s, "
            f"NLJ={nlj_time:.3f}s, required<={join_ratio:.3f}"
        )
        return TestResult(case, ratio <= join_ratio, detail)
    except Exception as exc:
        detail = f"runtime error: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        if client is not None:
            client.close()
        terminate_server(proc)


def run_index_compat_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    if case.name == "03_01_index_ddl":
        statements = [
            "create table warehouse (id int, name char(8));",
            "create index warehouse (id);",
            "show index from warehouse;",
            "create index warehouse (id,name);",
            "show index from warehouse;",
        ]
        expected = [
            "| warehouse | unique | (id) |",
            "| warehouse | unique | (id) |",
            "| warehouse | unique | (id,name) |",
        ]
        return run_statements_with_server(
            case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
        )

    if case.name == "03_02_index_query":
        statements = [
            "create table warehouse_id (w_id int, name char(8));",
            "insert into warehouse_id values (10, 'qweruiop');",
            "insert into warehouse_id values (534, 'asdfhjkl');",
            "insert into warehouse_id values (100,'qwerghjk');",
            "insert into warehouse_id values (500,'bgtyhnmj');",
            "create index warehouse_id(w_id);",
            "select * from warehouse_id where w_id = 10;",
            "select * from warehouse_id where w_id < 534 and w_id > 100;",
            "create table warehouse_name (w_id int, name char(8));",
            "insert into warehouse_name values (10, 'qweruiop');",
            "insert into warehouse_name values (534, 'asdfhjkl');",
            "insert into warehouse_name values (100,'qwerghjk');",
            "insert into warehouse_name values (500,'bgtyhnmj');",
            "create index warehouse_name(name);",
            "select * from warehouse_name where name = 'qweruiop';",
            "select * from warehouse_name where name > 'qwerghjk';",
            "select * from warehouse_name where name > 'aszdefgh' and name < 'qweraaaa';",
            "create table warehouse_mix (w_id int, name char(8));",
            "insert into warehouse_mix values (10, 'qweruiop');",
            "insert into warehouse_mix values (534, 'asdfhjkl');",
            "insert into warehouse_mix values (100,'qwerghjk');",
            "insert into warehouse_mix values (500,'bgtyhnmj');",
            "create index warehouse_mix(w_id,name);",
            "select * from warehouse_mix where w_id = 100 and name = 'qwerghjk';",
            "select * from warehouse_mix where w_id < 600 and name > 'bztyhnmj';",
        ]
        expected = normalize_output((EXPECTED_DIR / f"{case.name}.expected").read_text(encoding="utf-8"))
        return run_statements_with_server(
            case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
        )

    if case.name == "03_03_index_maintenance":
        statements = [
            "create table warehouse_single (w_id int, name char(8));",
            "insert into warehouse_single values (10 , 'qweruiop');",
            "insert into warehouse_single values (534, 'asdfhjkl');",
            "select * from warehouse_single where w_id = 10;",
            "select * from warehouse_single where w_id < 534 and w_id > 100;",
            "create index warehouse_single(w_id);",
            "insert into warehouse_single values (500, 'lastdanc');",
            "insert into warehouse_single values (10, 'uiopqwer');",
            "update warehouse_single set w_id = 507 where w_id = 534;",
            "select * from warehouse_single where w_id = 10;",
            "select * from warehouse_single where w_id < 534 and w_id > 100;",
            "create table warehouse_multi (w_id int, name char(8));",
            "insert into warehouse_multi values (10 , 'qweruiop');",
            "insert into warehouse_multi values (507, 'asdfhjkl');",
            "insert into warehouse_multi values (500, 'lastdanc');",
            "create index warehouse_multi(w_id,name);",
            "insert into warehouse_multi values(10,'qqqqoooo');",
            "insert into warehouse_multi values(500,'lastdanc');",
            "update warehouse_multi set w_id = 10, name = 'qqqqoooo' where w_id = 507 and name = 'asdfhjkl';",
            "select * from warehouse_multi;",
        ]
        expected = normalize_output((EXPECTED_DIR / f"{case.name}.expected").read_text(encoding="utf-8"))
        return run_statements_with_server(
            case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
        )

    raise RuntimeError(f"unknown index compatibility case: {case.name}")


def run_optimizer_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    assert case.sql_path is not None
    assert case.expected_path is not None
    statements = split_sql(case.sql_path.read_text(encoding="utf-8"))
    expected = normalize_output(
        case.expected_path.read_text(encoding="utf-8"),
        preserve_table_order=preserve_table_order_for_case(case.name),
        preserve_plan_indentation=preserve_plan_indentation_for_case(case.name),
    )
    return run_statements_with_server(
        case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
    )


def run_static_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    assert case.sql_path is not None
    assert case.expected_path is not None
    statements = split_sql(case.sql_path.read_text(encoding="utf-8"))
    preserve_table_order = preserve_table_order_for_case(case.name)
    preserve_plan_indentation = preserve_plan_indentation_for_case(case.name)
    expected = normalize_output(
        case.expected_path.read_text(encoding="utf-8"),
        preserve_table_order=preserve_table_order,
        preserve_plan_indentation=preserve_plan_indentation,
    )
    return run_statements_with_server(
        case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
    )


def run_transaction_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    with_index = case.kind.endswith("_index")
    is_commit = "commit" in case.kind
    statements = ["create table student (id int, name char(8), score float);"]
    if with_index:
        statements.append("create index student(id);")
    statements.extend([
        "insert into student values (1, 'xiaohong', 90.0);",
        "begin;",
        "insert into student values (2, 'xiaoming', 99.0);",
    ])
    if is_commit:
        statements.extend(["commit;", "select * from student;"])
        expected = [
            "| id | name | score |",
            "| 1 | xiaohong | 90.000000 |",
            "| 2 | xiaoming | 99.000000 |",
        ]
    else:
        statements.extend(["abort;", "select * from student;"])
        expected = ["| id | name | score |", "| 1 | xiaohong | 90.000000 |"]
    return run_statements_with_server(
        case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
    )


def run_abort_atomicity_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)

    server_log = TEST_ROOT / f"{case.name}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    client1: RmdbClient | None = None
    client2: RmdbClient | None = None
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        client1 = RmdbClient(DEFAULT_PORT, timeout)
        client2 = RmdbClient(DEFAULT_PORT, timeout)
        if case.kind == "txn_rc_delete_abort_visibility":
            for stmt in [
                "create table new_orders (no_o_id int, no_d_id int, no_w_id int);",
                "create index new_orders(no_w_id,no_d_id,no_o_id);",
                "insert into new_orders values (3001, 1, 1);",
                "insert into new_orders values (3002, 1, 1);",
                "insert into new_orders values (3003, 1, 1);",
            ]:
                client1.execute(stmt)
            client1.execute("begin;")
            client1.execute("delete from new_orders where no_o_id = 3001 and no_d_id = 1 and no_w_id = 1;")
            client2.execute("select min(no_o_id) from new_orders where no_d_id = 1 and no_w_id = 1;")
            client1.execute("abort;")
            client2.execute("select count(no_o_id) from new_orders where no_w_id = 1 and no_d_id = 1;")
            client2.execute("select min(no_o_id) from new_orders where no_w_id = 1 and no_d_id = 1;")
            expected = [
                "| min |",
                "| 3001 |",
                "| count |",
                "| 3 |",
                "| min |",
                "| 3001 |",
            ]
        elif case.kind == "txn_rc_update_visibility":
            client1.execute("create table account (id int, balance int);")
            client1.execute("insert into account values (1, 10);")
            client1.execute("begin;")
            client1.execute("update account set balance = 20 where id = 1;")
            client2.execute("select balance from account where balance > 0;")
            client1.execute("commit;")
            client2.execute("select balance from account where balance > 0;")
            expected = [
                "| balance |",
                "| 10 |",
                "| balance |",
                "| 20 |",
            ]
        elif case.kind == "txn_abort_multitable_insert":
            for stmt in [
                "create table orders (o_id int, o_d_id int, o_w_id int, o_ol_cnt int);",
                "create table new_orders (no_o_id int, no_d_id int, no_w_id int);",
                "create table order_line (ol_o_id int, ol_d_id int, ol_w_id int, ol_number int);",
                "create index orders(o_w_id,o_d_id,o_id);",
                "create index new_orders(no_w_id,no_d_id,no_o_id);",
                "create index order_line(ol_w_id,ol_d_id,ol_o_id,ol_number);",
                "begin;",
                "insert into orders values (3001, 1, 1, 2);",
                "insert into new_orders values (3001, 1, 1);",
                "insert into order_line values (3001, 1, 1, 1);",
                "insert into order_line values (3001, 1, 1, 2);",
                "abort;",
                "select count(o_id) from orders where o_w_id = 1 and o_d_id = 1;",
                "select count(no_o_id) from new_orders where no_w_id = 1 and no_d_id = 1;",
                "select count(ol_o_id) from order_line where ol_w_id = 1 and ol_d_id = 1;",
            ]:
                client1.execute(stmt)
            expected = [
                "| count |",
                "| 0 |",
                "| count |",
                "| 0 |",
                "| count |",
                "| 0 |",
            ]
        else:
            for stmt in [
                "create table orders (o_id int, o_d_id int, o_w_id int, o_ol_cnt int);",
                "create table new_orders (no_o_id int, no_d_id int, no_w_id int);",
                "create table order_line (ol_o_id int, ol_d_id int, ol_w_id int, ol_number int);",
                "create index orders(o_w_id,o_d_id,o_id);",
                "create index new_orders(no_w_id,no_d_id,no_o_id);",
                "create index order_line(ol_w_id,ol_d_id,ol_o_id,ol_number);",
                "begin;",
                "insert into orders values (3001, 1, 1, 2);",
                "insert into new_orders values (3001, 1, 1);",
                "insert into order_line values (3001, 1, 1, 1);",
            ]:
                client1.execute(stmt)
            response = client1.execute("insert into order_line values (3001, 1, 1, 1);")
            if not response.startswith(b"abort"):
                raise RuntimeError(f"expected duplicate order_line to abort explicit txn, got {response!r}")
            client1.execute("commit;")
            for stmt in [
                "select count(o_id) from orders where o_w_id = 1 and o_d_id = 1;",
                "select count(no_o_id) from new_orders where no_w_id = 1 and no_d_id = 1;",
                "select count(ol_o_id) from order_line where ol_w_id = 1 and ol_d_id = 1;",
            ]:
                client1.execute(stmt)
            expected = [
                "abort",
                "| count |",
                "| 0 |",
                "| count |",
                "| 0 |",
                "| count |",
                "| 0 |",
            ]
        time.sleep(0.1)
    except Exception as exc:
        terminate_server(proc)
        detail = f"runtime error: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        if client1 is not None:
            client1.close()
        if client2 is not None:
            client2.close()
        terminate_server(proc)

    detail = compare_lines(
        expected,
        read_actual_output(build_dir, db_name),
        case.name,
        preserve_table_order=True,
    )
    if detail is None:
        if not show_server_log and server_log.exists():
            server_log.unlink()
        if not keep_db:
            cleanup_db(build_dir, db_name)
        return TestResult(case, True, "passed")
    if show_server_log and server_log.exists():
        detail += "\n\n--- server log tail ---\n"
        detail += server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
    return TestResult(case, False, detail)


def recovery_workload(client: RmdbClient, rows: int, use_index: bool, use_checkpoint: bool, workers: int) -> None:
    client.execute("create table warehouse (w_id int, w_name char(10), w_ytd float);")
    if use_index:
        client.execute("create index warehouse(w_id);")
    for i in range(1, rows + 1):
        client.execute(f"insert into warehouse values ({i}, 'wh{i % 10000:04d}', {float(i):.6f});")
        if use_checkpoint and i % max(1, rows // 4) == 0:
            client.execute("create static_checkpoint;")

    errors: list[str] = []
    def run_worker(worker_id: int) -> None:
        local = RmdbClient(DEFAULT_PORT, client.sock.gettimeout() or 20.0)
        try:
            start = worker_id + 1
            step = max(1, workers)
            for key in range(start, rows + 1, step):
                local.execute("begin;")
                local.execute(f"update warehouse set w_ytd={1000.0 + key:.6f} where w_id={key};")
                local.execute("commit;")
        except Exception as exc:
            errors.append(str(exc))
        finally:
            local.close()

    threads = [threading.Thread(target=run_worker, args=(i,)) for i in range(workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if errors:
        raise RuntimeError(errors[0])
    if use_checkpoint:
        client.execute("create static_checkpoint;")


def run_recovery_trial(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
    rows: int,
    use_index: bool,
    use_checkpoint: bool,
    workers: int,
    validate_query: bool = False,
    suffix: str = "",
) -> tuple[bool, str, float]:
    db_name = f"doc_test_{case.name}{suffix}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)
    server_log = TEST_ROOT / f"{case.name}{suffix}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    client: RmdbClient | None = None
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        client = RmdbClient(DEFAULT_PORT, timeout)
        recovery_workload(client, rows, use_index, use_checkpoint, workers)
        client.execute("crash;")
        client.close()
        client = None
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)
    except Exception as exc:
        if client is not None:
            client.close()
        terminate_server(proc)
        detail = f"runtime error before restart: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return False, detail, float("inf")

    restart_log = TEST_ROOT / f"{case.name}{suffix}.restart.server.log"
    recovery_time = float("inf")
    for attempt in range(3):
        proc = start_server(server, build_dir, db_name, restart_log)
        try:
            start = time.perf_counter()
            wait_for_server(DEFAULT_PORT, max(connect_timeout, 20.0))
            recovery_time = time.perf_counter() - start
            break
        except Exception as exc:
            terminate_server(proc)
            if attempt == 2:
                detail = f"runtime error after restart: {exc}"
                if show_server_log and restart_log.exists():
                    detail += "\n" + restart_log.read_text(encoding="utf-8", errors="replace")[-4000:]
                return False, detail, float("inf")
            time.sleep(1.0)
    try:
        if not validate_query:
            return True, f"passed, recovery_time={recovery_time:.3f}s", recovery_time
        cleanup_outputs(build_dir, db_name)
        client = RmdbClient(DEFAULT_PORT, timeout)
        sample_keys = sorted({1, max(1, rows // 2), rows})
        for key in sample_keys:
            client.execute(f"select * from warehouse where w_id = {key};")
        time.sleep(0.1)
        client.close()
        client = None
        expected = []
        for key in sample_keys:
            expected.extend([
                "| w_id | w_name | w_ytd |",
                f"| {key} | wh{key % 10000:04d} | {1000.0 + key:.6f} |",
            ])
        detail = compare_lines(expected, read_actual_output(build_dir, db_name), case.name)
        if detail is None:
            return True, f"passed, recovery_time={recovery_time:.3f}s", recovery_time
        return False, detail, recovery_time
    except Exception as exc:
        detail = f"runtime error after restart: {exc}"
        if show_server_log and restart_log.exists():
            detail += "\n" + restart_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return False, detail, float("inf")
    finally:
        if client is not None:
            client.close()
        terminate_server(proc)


def recovery_script(case: TestCase) -> tuple[list[str], list[str], list[str]]:
    if case.kind == "recovery_script_undo_uncommitted":
        before_crash = [
            "create table rec_undo (id int, val int);",
            "insert into rec_undo values (1, 10);",
            "insert into rec_undo values (2, 20);",
            "begin;",
            "update rec_undo set val = 100 where id = 1;",
            "delete from rec_undo where id = 2;",
            "insert into rec_undo values (3, 30);",
        ]
        after_restart = ["select * from rec_undo;"]
        expected = ["| id | val |", "| 1 | 10 |", "| 2 | 20 |"]
    elif case.kind == "recovery_script_redo_committed":
        before_crash = [
            "create table rec_redo (id int, val int);",
            "insert into rec_redo values (1, 10);",
            "insert into rec_redo values (2, 20);",
            "insert into rec_redo values (3, 30);",
            "begin;",
            "update rec_redo set val = 110 where id = 1;",
            "delete from rec_redo where id = 2;",
            "insert into rec_redo values (4, 40);",
            "commit;",
        ]
        after_restart = ["select * from rec_redo;"]
        expected = ["| id | val |", "| 1 | 110 |", "| 3 | 30 |", "| 4 | 40 |"]
    elif case.kind == "recovery_script_index_consistency":
        before_crash = [
            "create table rec_idx (id int, val int);",
            "create index rec_idx(id);",
            "insert into rec_idx values (1, 10);",
            "insert into rec_idx values (2, 20);",
            "begin;",
            "update rec_idx set id = 3 where id = 1;",
            "delete from rec_idx where id = 2;",
            "insert into rec_idx values (4, 40);",
            "commit;",
            "begin;",
            "insert into rec_idx values (5, 50);",
            "update rec_idx set id = 6 where id = 3;",
            "delete from rec_idx where id = 4;",
        ]
        after_restart = [
            "select * from rec_idx where id = 3;",
            "select * from rec_idx where id = 2;",
            "select * from rec_idx where id = 4;",
            "select * from rec_idx where id = 5;",
            "select * from rec_idx where id = 6;",
        ]
        expected = [
            "| id | val |",
            "| 3 | 10 |",
            "| id | val |",
            "| id | val |",
            "| 4 | 40 |",
            "| id | val |",
            "| id | val |",
        ]
    elif case.kind == "recovery_script_checkpoint_boundary":
        before_crash = [
            "create table rec_ckpt (id int, val int);",
            "insert into rec_ckpt values (1, 10);",
            "begin;",
            "update rec_ckpt set val = 11 where id = 1;",
            "commit;",
            "create static_checkpoint;",
            "begin;",
            "insert into rec_ckpt values (2, 20);",
            "commit;",
            "begin;",
            "update rec_ckpt set val = 12 where id = 1;",
            "commit;",
            "begin;",
            "insert into rec_ckpt values (3, 30);",
            "update rec_ckpt set val = 200 where id = 2;",
            "delete from rec_ckpt where id = 1;",
        ]
        after_restart = ["select * from rec_ckpt;"]
        expected = ["| id | val |", "| 1 | 12 |", "| 2 | 20 |"]
    elif case.kind == "recovery_script_restart_new_writes":
        before_crash = [
            "create table rec_restart (id int, val int);",
            "insert into rec_restart values (1, 10);",
            "begin;",
            "insert into rec_restart values (2, 200);",
        ]
        after_restart = [
            "begin;",
            "insert into rec_restart values (2, 20);",
            "update rec_restart set val = 15 where id = 1;",
            "commit;",
            "select * from rec_restart;",
        ]
        expected = ["| id | val |", "| 1 | 15 |", "| 2 | 20 |"]
    elif case.kind == "recovery_script_multitable_atomic":
        before_crash = [
            "create table rec_a (id int, val int);",
            "create table rec_b (id int, val int);",
            "begin;",
            "insert into rec_a values (1, 10);",
            "insert into rec_b values (1, 100);",
            "commit;",
            "begin;",
            "update rec_a set val = 99 where id = 1;",
            "insert into rec_b values (2, 200);",
        ]
        after_restart = [
            "select * from rec_a;",
            "select * from rec_b;",
        ]
        expected = ["| id | val |", "| 1 | 10 |", "| id | val |", "| 1 | 100 |"]
    else:
        raise RuntimeError(f"unknown recovery script case kind: {case.kind}")
    return before_crash, after_restart, expected


def run_recovery_script_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)

    before_crash, after_restart, expected = recovery_script(case)
    server_log = TEST_ROOT / f"{case.name}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    client: RmdbClient | None = None
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        client = RmdbClient(DEFAULT_PORT, timeout)
        for statement in before_crash:
            client.execute(statement)
        client.execute("crash;")
        client.close()
        client = None
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)
    except Exception as exc:
        if client is not None:
            client.close()
        terminate_server(proc)
        detail = f"runtime error before restart: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)

    restart_log = TEST_ROOT / f"{case.name}.restart.server.log"
    proc = start_server(server, build_dir, db_name, restart_log)
    try:
        wait_for_server(DEFAULT_PORT, max(connect_timeout, 20.0))
        cleanup_outputs(build_dir, db_name)
        client = RmdbClient(DEFAULT_PORT, timeout)
        for statement in after_restart:
            client.execute(statement)
        time.sleep(0.1)
        detail = compare_lines(expected, read_actual_output(build_dir, db_name), case.name)
        return TestResult(case, detail is None, detail or "passed")
    except Exception as exc:
        detail = f"runtime error after restart: {exc}"
        if show_server_log and restart_log.exists():
            detail += "\n" + restart_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        if client is not None:
            client.close()
        terminate_server(proc)


def run_recovery_log_validation_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)

    server_log = TEST_ROOT / f"{case.name}.server.log"
    restart_log = TEST_ROOT / f"{case.name}.restart.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    client: RmdbClient | None = None
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        client = RmdbClient(DEFAULT_PORT, timeout)
        for statement in [
            "create table rec_log_guard (id int, val int);",
            "insert into rec_log_guard values (1, 10);",
            "begin;",
            "insert into rec_log_guard values (2, 20);",
        ]:
            client.execute(statement)
        client.execute("crash;")
        client.close()
        client = None
        proc.wait(timeout=5)

        db_path = build_dir / db_name
        log_path = db_path / "db.log"
        checkpoint_path = db_path / "db.chkpt"
        header_format = "=iqIqq"
        header_size = struct.calcsize(header_format)
        if header_size != 32:
            raise RuntimeError(f"unexpected test WAL header size: {header_size}")
        with log_path.open("ab") as log_file:
            nested_length_offset = log_file.tell()
            nested_total_length = 64
            log_file.write(
                struct.pack(header_format, 1, nested_length_offset, nested_total_length, 987654321, -1)
            )
            log_file.write(struct.pack("=i", 2_147_483_647))
            log_file.write(b"\0" * (nested_total_length - header_size - struct.calcsize("=i")))

            oversized_offset = log_file.tell()
            log_file.write(struct.pack(header_format, 6, oversized_offset, 0xFFFFFFFF, -1, -1))
        checkpoint_path.write_text(str(oversized_offset), encoding="utf-8")

        proc = start_server(server, build_dir, db_name, restart_log)
        wait_for_server(DEFAULT_PORT, max(connect_timeout, 20.0))
        cleanup_outputs(build_dir, db_name)
        client = RmdbClient(DEFAULT_PORT, timeout)
        client.execute("select * from rec_log_guard;")
        time.sleep(0.1)
        detail = compare_lines(
            ["| id | val |", "| 1 | 10 |"],
            read_actual_output(build_dir, db_name),
            case.name,
        )
        if detail is not None:
            return TestResult(case, False, detail)
    except Exception as exc:
        detail = f"runtime error: {exc}"
        if show_server_log:
            for log_file in [server_log, restart_log]:
                if log_file.exists():
                    detail += f"\n\n--- {log_file.name} tail ---\n"
                    detail += log_file.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        if client is not None:
            client.close()
        terminate_server(proc)

    if not show_server_log:
        for log_file in [server_log, restart_log]:
            if log_file.exists():
                log_file.unlink()
    if not keep_db:
        cleanup_db(build_dir, db_name)
    return TestResult(case, True, "passed")


def run_recovery_streaming_log_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)

    server_log = TEST_ROOT / f"{case.name}.server.log"
    restart_log = TEST_ROOT / f"{case.name}.restart.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    client: RmdbClient | None = None
    peak_rss_kb = 0
    recovery_time = float("inf")
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        client = RmdbClient(DEFAULT_PORT, timeout)
        for statement in [
            "create table rec_stream (id int, val int);",
            "insert into rec_stream values (1, 10);",
            "begin;",
            "insert into rec_stream values (2, 20);",
        ]:
            client.execute(statement)
        client.execute("crash;")
        client.close()
        client = None
        proc.wait(timeout=5)

        log_path = build_dir / db_name / "db.log"
        valid_log_size = log_path.stat().st_size
        with log_path.open("r+b") as log_file:
            log_file.truncate(valid_log_size + 4 * 1024 * 1024 * 1024)

        start = time.perf_counter()
        proc = start_server(server, build_dir, db_name, restart_log)
        deadline = start + max(connect_timeout, 30.0)
        ready = False
        while time.perf_counter() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(f"server exited during streaming recovery: {proc.returncode}")
            rss_kb = read_process_rss_kb(proc.pid)
            if rss_kb is not None:
                peak_rss_kb = max(peak_rss_kb, rss_kb)
            try:
                probe = socket.create_connection(("127.0.0.1", DEFAULT_PORT), timeout=0.1)
                probe.close()
                ready = True
                break
            except OSError:
                time.sleep(0.02)
        if not ready:
            raise RuntimeError("server did not finish bounded streaming recovery")
        recovery_time = time.perf_counter() - start
        if peak_rss_kb > 3 * 1024 * 1024:
            raise RuntimeError(f"streaming recovery RSS exceeded bound: {peak_rss_kb} KiB")

        cleanup_outputs(build_dir, db_name)
        client = RmdbClient(DEFAULT_PORT, timeout)
        client.execute("select * from rec_stream;")
        time.sleep(0.1)
        detail = compare_lines(
            ["| id | val |", "| 1 | 10 |"],
            read_actual_output(build_dir, db_name),
            case.name,
        )
        if detail is not None:
            return TestResult(case, False, detail)
    except Exception as exc:
        detail = f"runtime error: {exc}"
        if show_server_log:
            for log_file in [server_log, restart_log]:
                if log_file.exists():
                    detail += f"\n\n--- {log_file.name} tail ---\n"
                    detail += log_file.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        if client is not None:
            client.close()
        terminate_server(proc)

    if not show_server_log:
        for log_file in [server_log, restart_log]:
            if log_file.exists():
                log_file.unlink()
    if not keep_db:
        cleanup_db(build_dir, db_name)
    return TestResult(
        case,
        True,
        f"passed, recovery_time={recovery_time:.3f}s, peak_rss_kb={peak_rss_kb}",
    )


def run_recovery_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
    rows: int,
    recovery_ratio: float,
) -> TestResult:
    if case.kind == "recovery_streaming_log":
        return run_recovery_streaming_log_case(
            case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log
        )
    if case.kind == "recovery_log_validation":
        return run_recovery_log_validation_case(
            case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log
        )
    if case.kind.startswith("recovery_script_"):
        return run_recovery_script_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)

    use_index = case.kind == "recovery_index"
    use_checkpoint = case.kind == "recovery_with_checkpoint"
    workers = 4 if case.kind in {"recovery_multi", "recovery_large"} else 1
    actual_rows = rows
    if case.kind == "recovery_single":
        actual_rows = max(20, rows // 10)
    elif case.kind in {"recovery_index", "recovery_multi"}:
        actual_rows = max(50, rows // 4)
    elif case.kind in {"recovery_large", "recovery_without_checkpoint", "recovery_with_checkpoint"}:
        actual_rows = max(rows, 200)

    if case.kind != "recovery_with_checkpoint":
        ok, detail, _ = run_recovery_trial(
            case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log,
            actual_rows, use_index, use_checkpoint, workers, validate_query=True
        )
        return TestResult(case, ok, detail)

    no_case = TestCase(case.name + "_baseline", None, None, "recovery_without_checkpoint")
    ok1, detail1, t1 = run_recovery_trial(
        no_case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log,
        actual_rows, use_index=False, use_checkpoint=False, workers=1, validate_query=True, suffix="_without"
    )
    if not ok1:
        return TestResult(case, False, "baseline failed: " + detail1)
    ok2, detail2, t2 = run_recovery_trial(
        case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log,
        actual_rows, use_index=False, use_checkpoint=True, workers=1, validate_query=True, suffix="_with"
    )
    if not ok2:
        return TestResult(case, False, detail2)
    ratio = t2 / t1 if t1 > 0 else float("inf")
    tolerance = max(0.25, t1 * 0.15)
    passed = ratio <= recovery_ratio or t2 <= t1 + tolerance
    detail = (
        f"checkpoint/no-checkpoint ratio={ratio:.3f}, checkpoint={t2:.3f}s, "
        f"baseline={t1:.3f}s, required<={recovery_ratio:.3f}, tolerance=+{tolerance:.3f}s"
    )
    return TestResult(
        case,
        passed,
        detail,
    )


def run_case(case: TestCase, server: Path, build_dir: Path, timeout: float, connect_timeout: float,
             keep_db: bool, show_server_log: bool, row_count: int = 3000, join_row_count: int = 1000,
             recovery_row_count: int = 200, index_ratio: float = 0.70, join_ratio: float = 0.50,
             recovery_ratio: float = 0.70) -> TestResult:
    if case.kind == "perf_single" or case.kind == "perf_multi":
        return run_index_perf_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log,
                                   row_count, index_ratio)
    if case.kind == "join_perf":
        return run_join_perf_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log,
                                  join_row_count, join_ratio)
    if case.name in {"03_01_index_ddl", "03_02_index_query", "03_03_index_maintenance"}:
        return run_index_compat_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
    if case.name.startswith("04_"):
        return run_optimizer_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
    if case.name in {"02_02_insert_select", "05_04_order_limit"}:
        return run_static_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
    if case.kind == "connection_resilience":
        return run_connection_resilience_case(
            case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log
        )
    if case.kind.startswith("txn_"):
        if case.kind in {
            "txn_rc_delete_abort_visibility",
            "txn_rc_update_visibility",
            "txn_abort_multitable_insert",
            "txn_error_aborts_multitable",
        }:
            return run_abort_atomicity_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
        return run_transaction_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
    if case.kind.startswith("recovery_"):
        return run_recovery_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log,
                                 recovery_row_count, recovery_ratio)
    assert case.sql_path is not None
    assert case.expected_path is not None
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)

    server_log = TEST_ROOT / f"{case.name}.server.log"
    with server_log.open("w", encoding="utf-8") as log_file:
        proc = subprocess.Popen(
            [str(server), db_name],
            cwd=build_dir,
            env={**os.environ, "RMDB_PORT": str(DEFAULT_PORT)},
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_for_server(DEFAULT_PORT, connect_timeout)
            statements = split_sql(case.sql_path.read_text(encoding="utf-8"))
            send_sql(statements, DEFAULT_PORT, timeout)
            time.sleep(0.1)
        except Exception as exc:
            terminate_server(proc)
            detail = f"runtime error: {exc}"
            if show_server_log and server_log.exists():
                detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
            return TestResult(case, False, detail)
        finally:
            terminate_server(proc)

    preserve_table_order = preserve_table_order_for_case(case.name)
    preserve_plan_indentation = preserve_plan_indentation_for_case(case.name)
    expected = canonicalize_case_output(
        case.name,
        normalize_output(
            case.expected_path.read_text(encoding="utf-8"),
            preserve_table_order=preserve_table_order,
            preserve_plan_indentation=preserve_plan_indentation,
        ),
    )
    actual = canonicalize_case_output(
        case.name,
        normalize_output(
            read_actual_output(build_dir, db_name),
            preserve_table_order=preserve_table_order,
            preserve_plan_indentation=preserve_plan_indentation,
        ),
    )
    if expected == actual:
        if not show_server_log and server_log.exists():
            server_log.unlink()
        if not keep_db:
            cleanup_db(build_dir, db_name)
        return TestResult(case, True, "passed")

    diff = "\n".join(
        difflib.unified_diff(
            expected,
            actual,
            fromfile=f"expected/{case.name}.expected",
            tofile=f"actual/{case.name}.output",
            lineterm="",
        )
    )
    detail = diff or "output mismatch"
    if show_server_log and server_log.exists():
        detail += "\n\n--- server log tail ---\n"
        detail += server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
    return TestResult(case, False, detail)


def main() -> int:
    global DEFAULT_PORT
    args = parse_args()
    if args.port < 1 or args.port > 65535:
        print(f"Invalid TCP port: {args.port}", file=sys.stderr)
        return 2
    DEFAULT_PORT = args.port
    build_dir = args.build_dir.resolve()
    server = (args.server or (build_dir / "bin" / "rmdb")).resolve()
    cases = discover_cases(args.cases)

    if args.list:
        for case in cases:
            print(case.name)
        return 0

    if not cases:
        print("No test cases selected.", file=sys.stderr)
        return 2
    if not server.exists():
        print(f"Server binary not found: {server}", file=sys.stderr)
        print("Build it first, e.g. cmake --build build --target rmdb", file=sys.stderr)
        return 2
    if not build_dir.exists():
        print(f"Build directory not found: {build_dir}", file=sys.stderr)
        return 2

    results: list[TestResult] = []
    for case in cases:
        if not args.quiet:
            print(f"[ RUN      ] {case.name}")
        result = run_case(
            case,
            server,
            build_dir,
            args.timeout,
            args.connect_timeout,
            args.keep_db,
            args.show_server_log,
            args.row_count,
            args.join_row_count,
            args.recovery_row_count,
            args.index_ratio,
            args.join_ratio,
            args.recovery_ratio,
        )
        results.append(result)
        status = "       OK " if result.passed else "  FAILED "
        if not args.quiet:
            print(f"[{status}] {case.name}")
        if not result.passed:
            if args.quiet:
                print(f"[{status}] {case.name}")
            print(result.detail)

    failed = [result for result in results if not result.passed]
    if not args.quiet:
        print(f"\nSummary: {len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
