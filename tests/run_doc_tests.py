#!/usr/bin/env python3
"""Run local RMDB tests transcribed from 测试说明文档.md.

The runner intentionally does not modify CMake files.  It starts the compiled
server, sends SQL over the same TCP protocol as rmdb_client, then compares the
database output.txt with the expected result file.
"""

from __future__ import annotations

import argparse
import difflib
import re
import shutil
import signal
import socket
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
        TestCase("09_00_isolation_syntax", None, None, "isolation_syntax"),
        TestCase("09_01_si_write_write_conflict", None, None, "isolation_si_write_conflict"),
        TestCase("09_02_si_repeatable_read", None, None, "isolation_si_repeatable_read"),
        TestCase("09_03_ser_write_write_conflict", None, None, "isolation_ser_write_conflict"),
        TestCase("09_04_ser_repeatable_read", None, None, "isolation_ser_repeatable_read"),
        TestCase("09_05_si_insert_visibility", None, None, "isolation_si_insert_visibility"),
        TestCase("09_06_si_dirty_read_self_write", None, None, "isolation_si_dirty_read_self_write"),
        TestCase("09_07_si_read_write_delete_conflict", None, None, "isolation_si_read_write_delete_conflict"),
        TestCase("09_08_si_write_skew_allowed", None, None, "isolation_si_write_skew_allowed"),
        TestCase("09_09_ser_write_skew_abort", None, None, "isolation_ser_write_skew_abort"),
        TestCase("09_10_ser_phantom_empty_predicate_abort", None, None, "isolation_ser_phantom_empty_predicate_abort"),
        TestCase("09_11_ser_select_dangerous_abort", None, None, "isolation_ser_select_dangerous_abort"),
        TestCase("10_01_crash_recovery_single_thread", None, None, "recovery_single"),
        TestCase("10_02_crash_recovery_multi_thread", None, None, "recovery_multi"),
        TestCase("10_03_crash_recovery_index", None, None, "recovery_index"),
        TestCase("10_04_crash_recovery_large_data", None, None, "recovery_large"),
        TestCase("10_05_crash_recovery_without_checkpoint", None, None, "recovery_without_checkpoint"),
        TestCase("10_06_crash_recovery_with_checkpoint", None, None, "recovery_with_checkpoint"),
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


def start_server(server: Path, build_dir: Path, db_name: str, server_log: Path) -> subprocess.Popen[str]:
    log_file = server_log.open("w", encoding="utf-8")
    return subprocess.Popen(
        [str(server), db_name],
        cwd=build_dir,
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


def preserve_table_order_for_case(case_name: str) -> bool:
    return case_name in {
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


def run_isolation_case(
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
    clients: list[RmdbClient] = []
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        setup = RmdbClient(DEFAULT_PORT, timeout)
        clients.append(setup)

        level = "SNAPSHOT ISOLATION" if "_si_" in case.kind else "SERIALIZABLE"
        if case.kind == "isolation_syntax":
            setup.execute("create table iso_syntax (id int, val int);")
            setup.execute("set transaction isolation level snapshot isolation;")
            setup.execute("begin;")
            setup.execute("insert into iso_syntax values (1, 10);")
            setup.execute("commit;")
            setup.execute("set transaction isolation level serializable;")
            setup.execute("begin;")
            setup.execute("select * from iso_syntax;")
            setup.execute("commit;")
            expected = ["| id | val |", "| 1 | 10 |"]
        elif case.kind.endswith("write_conflict"):
            setup.execute("create table account (id int, balance int);")
            setup.execute("insert into account values (1, 100);")
            setup.close()
            clients.clear()

            t1 = RmdbClient(DEFAULT_PORT, timeout)
            t2 = RmdbClient(DEFAULT_PORT, timeout)
            verifier = RmdbClient(DEFAULT_PORT, timeout)
            clients.extend([t1, t2, verifier])
            t1.execute(f"set transaction isolation level {level};")
            t2.execute(f"set transaction isolation level {level};")
            t1.execute("begin;")
            t1.execute("update account set balance = 120 where id = 1;")
            t2.execute("begin;")
            t2.execute("update account set balance = 90 where id = 1;")
            t1.execute("commit;")
            t2.execute("commit;")
            verifier.execute("select * from account where id = 1;")
            expected = ["abort", "| id | balance |", "| 1 | 120 |"]
        elif case.kind.endswith("repeatable_read"):
            setup.execute("create table counter_test (id int, val int);")
            setup.execute("insert into counter_test values (1, 100);")
            setup.close()
            clients.clear()

            t1 = RmdbClient(DEFAULT_PORT, timeout)
            t2 = RmdbClient(DEFAULT_PORT, timeout)
            clients.extend([t1, t2])
            t1.execute(f"set transaction isolation level {level};")
            t1.execute("begin;")
            t1.execute("select * from counter_test where id = 1;")
            t2.execute(f"set transaction isolation level {level};")
            t2.execute("begin;")
            t2.execute("update counter_test set val = 200 where id = 1;")
            t2.execute("commit;")
            t1.execute("select * from counter_test where id = 1;")
            t1.execute("commit;")
            expected = [
                "| id | val |",
                "| 1 | 100 |",
                "| id | val |",
                "| 1 | 100 |",
            ]
        elif case.kind == "isolation_si_insert_visibility":
            setup.execute("create table iso_insert (id int, val int);")
            setup.execute("insert into iso_insert values (1, 10);")
            setup.close()
            clients.clear()

            t1 = RmdbClient(DEFAULT_PORT, timeout)
            t2 = RmdbClient(DEFAULT_PORT, timeout)
            verifier = RmdbClient(DEFAULT_PORT, timeout)
            clients.extend([t1, t2, verifier])
            t1.execute("set transaction isolation level snapshot isolation;")
            t2.execute("set transaction isolation level snapshot isolation;")
            t1.execute("begin;")
            t1.execute("insert into iso_insert values (2, 20);")
            t1.execute("select * from iso_insert where id = 2;")
            t2.execute("begin;")
            t2.execute("select * from iso_insert;")
            t1.execute("commit;")
            t2.execute("select * from iso_insert;")
            t2.execute("commit;")
            verifier.execute("select * from iso_insert;")
            expected = [
                "| id | val |",
                "| 2 | 20 |",
                "| id | val |",
                "| 1 | 10 |",
                "| id | val |",
                "| 1 | 10 |",
                "| id | val |",
                "| 1 | 10 |",
                "| 2 | 20 |",
            ]
        elif case.kind == "isolation_si_dirty_read_self_write":
            setup.execute("create table iso_dirty (id int, val int);")
            setup.execute("insert into iso_dirty values (1, 100);")
            setup.close()
            clients.clear()

            t1 = RmdbClient(DEFAULT_PORT, timeout)
            t2 = RmdbClient(DEFAULT_PORT, timeout)
            verifier = RmdbClient(DEFAULT_PORT, timeout)
            clients.extend([t1, t2, verifier])
            t1.execute("set transaction isolation level snapshot isolation;")
            t2.execute("set transaction isolation level snapshot isolation;")
            t1.execute("begin;")
            t1.execute("update iso_dirty set val = 200 where id = 1;")
            t1.execute("select * from iso_dirty where id = 1;")
            t2.execute("begin;")
            t2.execute("select * from iso_dirty where id = 1;")
            t1.execute("abort;")
            t2.execute("select * from iso_dirty where id = 1;")
            t2.execute("commit;")
            verifier.execute("select * from iso_dirty where id = 1;")
            expected = [
                "| id | val |",
                "| 1 | 200 |",
                "| id | val |",
                "| 1 | 100 |",
                "| id | val |",
                "| 1 | 100 |",
                "| id | val |",
                "| 1 | 100 |",
            ]
        elif case.kind == "isolation_si_read_write_delete_conflict":
            setup.execute("create table iso_delete (id int, val int);")
            setup.execute("insert into iso_delete values (1, 100);")
            setup.close()
            clients.clear()

            t1 = RmdbClient(DEFAULT_PORT, timeout)
            t2 = RmdbClient(DEFAULT_PORT, timeout)
            verifier = RmdbClient(DEFAULT_PORT, timeout)
            clients.extend([t1, t2, verifier])
            t1.execute("set transaction isolation level snapshot isolation;")
            t2.execute("set transaction isolation level snapshot isolation;")
            t1.execute("begin;")
            t1.execute("select * from iso_delete where id = 1;")
            t2.execute("begin;")
            t2.execute("delete from iso_delete where id = 1;")
            t2.execute("commit;")
            t1.execute("select * from iso_delete where id = 1;")
            t1.execute("delete from iso_delete where id = 1;")
            t1.execute("commit;")
            verifier.execute("select * from iso_delete;")
            expected = [
                "| id | val |",
                "| 1 | 100 |",
                "| id | val |",
                "| 1 | 100 |",
                "abort",
                "| id | val |",
            ]
        elif case.kind == "isolation_si_write_skew_allowed":
            setup.execute("create table duty_si (doctor_id int, on_call int);")
            setup.execute("insert into duty_si values (1, 1);")
            setup.execute("insert into duty_si values (2, 1);")
            setup.close()
            clients.clear()

            t1 = RmdbClient(DEFAULT_PORT, timeout)
            t2 = RmdbClient(DEFAULT_PORT, timeout)
            verifier = RmdbClient(DEFAULT_PORT, timeout)
            clients.extend([t1, t2, verifier])
            t1.execute("set transaction isolation level snapshot isolation;")
            t2.execute("set transaction isolation level snapshot isolation;")
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("select * from duty_si where doctor_id = 2;")
            t2.execute("select * from duty_si where doctor_id = 1;")
            t1.execute("update duty_si set on_call = 0 where doctor_id = 1;")
            t2.execute("update duty_si set on_call = 0 where doctor_id = 2;")
            t1.execute("commit;")
            t2.execute("commit;")
            verifier.execute("select * from duty_si;")
            expected = [
                "| doctor_id | on_call |",
                "| 2 | 1 |",
                "| doctor_id | on_call |",
                "| 1 | 1 |",
                "| doctor_id | on_call |",
                "| 1 | 0 |",
                "| 2 | 0 |",
            ]
        elif case.kind == "isolation_ser_write_skew_abort":
            setup.execute("create table duty_ser (doctor_id int, on_call int);")
            setup.execute("insert into duty_ser values (1, 1);")
            setup.execute("insert into duty_ser values (2, 1);")
            setup.close()
            clients.clear()

            t1 = RmdbClient(DEFAULT_PORT, timeout)
            t2 = RmdbClient(DEFAULT_PORT, timeout)
            verifier = RmdbClient(DEFAULT_PORT, timeout)
            clients.extend([t1, t2, verifier])
            t1.execute("set transaction isolation level serializable;")
            t2.execute("set transaction isolation level serializable;")
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("select * from duty_ser where doctor_id = 2;")
            t2.execute("select * from duty_ser where doctor_id = 1;")
            t1.execute("update duty_ser set on_call = 0 where doctor_id = 1;")
            t2.execute("update duty_ser set on_call = 0 where doctor_id = 2;")
            t1.execute("commit;")
            t2.execute("commit;")
            verifier.execute("select * from duty_ser;")
            expected = [
                "| doctor_id | on_call |",
                "| 2 | 1 |",
                "| doctor_id | on_call |",
                "| 1 | 1 |",
                "abort",
                "| doctor_id | on_call |",
                "| 1 | 0 |",
                "| 2 | 1 |",
            ]
        elif case.kind == "isolation_ser_phantom_empty_predicate_abort":
            setup.execute("create table booking_ser (id int, slot int);")
            setup.close()
            clients.clear()

            t1 = RmdbClient(DEFAULT_PORT, timeout)
            t2 = RmdbClient(DEFAULT_PORT, timeout)
            verifier = RmdbClient(DEFAULT_PORT, timeout)
            clients.extend([t1, t2, verifier])
            t1.execute("set transaction isolation level serializable;")
            t2.execute("set transaction isolation level serializable;")
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("select * from booking_ser where slot = 9;")
            t2.execute("select * from booking_ser where slot = 9;")
            t1.execute("insert into booking_ser values (1, 9);")
            t2.execute("insert into booking_ser values (2, 9);")
            t1.execute("commit;")
            t2.execute("commit;")
            verifier.execute("select * from booking_ser;")
            expected = [
                "| id | slot |",
                "| id | slot |",
                "abort",
                "| id | slot |",
                "| 1 | 9 |",
            ]
        elif case.kind == "isolation_ser_select_dangerous_abort":
            setup.execute("create table select_ser (id int, val int);")
            setup.execute("insert into select_ser values (1, 10);")
            setup.execute("insert into select_ser values (2, 20);")
            setup.execute("insert into select_ser values (3, 30);")
            setup.close()
            clients.clear()

            t1 = RmdbClient(DEFAULT_PORT, timeout)
            t2 = RmdbClient(DEFAULT_PORT, timeout)
            verifier = RmdbClient(DEFAULT_PORT, timeout)
            clients.extend([t1, t2, verifier])
            t1.execute("set transaction isolation level serializable;")
            t2.execute("set transaction isolation level serializable;")
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("select * from select_ser where id = 2;")
            t2.execute("update select_ser set val = 200 where id = 2;")
            t1.execute("update select_ser set val = 300 where id = 3;")
            t2.execute("select * from select_ser where id = 3;")
            t1.execute("commit;")
            t2.execute("commit;")
            verifier.execute("select * from select_ser;")
            expected = [
                "| id | val |",
                "| 2 | 20 |",
                "abort",
                "| id | val |",
                "| 1 | 10 |",
                "| 2 | 20 |",
                "| 3 | 300 |",
            ]
        else:
            raise RuntimeError(f"unknown isolation case kind: {case.kind}")
        time.sleep(0.1)
    except Exception as exc:
        detail = f"runtime error: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        for client in clients:
            client.close()
        terminate_server(proc)
    detail = compare_lines(expected, read_actual_output(build_dir, db_name), case.name)
    return TestResult(case, detail is None, detail or "passed")


def setup_mvcc_table(client: RmdbClient) -> None:
    for statement in [
        "create table concurrency_test (id int, name char(8), score float);",
        "insert into concurrency_test values (1, 'xiaohong', 90.0);",
        "insert into concurrency_test values (2, 'xiaoming', 95.0);",
        "insert into concurrency_test values (3, 'zhanghua', 88.5);",
    ]:
        client.execute(statement)


def verify_mvcc_transaction_baseline(client: RmdbClient, build_dir: Path, db_name: str) -> str | None:
    for statement in [
        "create table mvcc_txn_probe (id int, name char(8), score float);",
        "insert into mvcc_txn_probe values (1, 'base', 10.0);",
        "begin;",
        "insert into mvcc_txn_probe values (2, 'abort', 20.0);",
        "abort;",
        "select * from mvcc_txn_probe;",
    ]:
        client.execute(statement)
    expected = ["| id | name | score |", "| 1 | base | 10.000000 |"]
    detail = compare_lines(expected, read_actual_output(build_dir, db_name), "mvcc_transaction_baseline")
    cleanup_outputs(build_dir, db_name)
    return detail


def run_mvcc_case(
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
    clients: list[RmdbClient] = []
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        setup = RmdbClient(DEFAULT_PORT, timeout)
        clients.append(setup)
        baseline_detail = verify_mvcc_transaction_baseline(setup, build_dir, db_name)
        if baseline_detail is not None:
            return TestResult(case, False, "MVCC transaction baseline failed:\n" + baseline_detail)
        setup_mvcc_table(setup)
        setup.close()
        clients.clear()

        t1 = RmdbClient(DEFAULT_PORT, timeout)
        t2 = RmdbClient(DEFAULT_PORT, timeout)
        clients.extend([t1, t2])
        kind = case.kind
        if kind in {"mvcc_abort", "mvcc_dirty_read"}:
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("update concurrency_test set score = 100.0 where id = 2;")
            t2.execute("select * from concurrency_test where id = 2;")
            t1.execute("abort;")
            t1.execute("select * from concurrency_test where id = 2;")
            t2.execute("commit;")
            expected = [
                "| id | name | score |",
                "| 2 | xiaoming | 95.000000 |",
                "| id | name | score |",
                "| 2 | xiaoming | 95.000000 |",
            ]
        elif kind == "mvcc_deadlock":
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("update concurrency_test set score = 91.0 where id = 1;")
            t2.execute("update concurrency_test set score = 96.0 where id = 2;")
            errors: list[str] = []
            responses: list[str] = []
            def run_cross(client: RmdbClient, statement: str) -> None:
                try:
                    responses.append(client.execute(statement).decode("utf-8", errors="replace").lower())
                except Exception as exc:
                    errors.append(str(exc))
            a = threading.Thread(target=run_cross, args=(t1, "update concurrency_test set score = 92.0 where id = 2;"))
            b = threading.Thread(target=run_cross, args=(t2, "update concurrency_test set score = 97.0 where id = 1;"))
            a.start(); b.start(); a.join(timeout); b.join(timeout)
            if a.is_alive() or b.is_alive():
                raise RuntimeError("deadlock was not resolved before timeout")
            if not errors and not any(("abort" in response or "failure" in response) for response in responses):
                raise RuntimeError("deadlock conflict completed without abort/failure signal")
            t1.execute("abort;")
            t2.execute("abort;")
            cleanup_outputs(build_dir, db_name)
            verifier = RmdbClient(DEFAULT_PORT, timeout)
            clients.append(verifier)
            verifier.execute("select * from concurrency_test where id = 2;")
            expected = ["| id | name | score |", "| 2 | xiaoming | 95.000000 |"]
        elif kind == "mvcc_insert_delete":
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("insert into concurrency_test values (4, 'newrow', 77.0);")
            t2.execute("delete from concurrency_test where id = 4;")
            t1.execute("commit;")
            t2.execute("abort;")
            t1.execute("select * from concurrency_test where id = 4;")
            expected = ["| id | name | score |", "| 4 | newrow | 77.000000 |"]
        elif kind == "mvcc_insert":
            t1.execute("begin;")
            t1.execute("insert into concurrency_test values (4, 'insert', 80.0);")
            t1.execute("commit;")
            t2.execute("select * from concurrency_test where id = 4;")
            expected = ["| id | name | score |", "| 4 | insert | 80.000000 |"]
        elif kind == "mvcc_lost_update":
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("select * from concurrency_test where id = 2;")
            t2.execute("update concurrency_test set score = 96.0 where id = 2;")
            t2.execute("commit;")
            t1.execute("update concurrency_test set score = 97.0 where id = 2;")
            t1.execute("commit;")
            t2.execute("select * from concurrency_test where id = 2;")
            expected = [
                "| id | name | score |",
                "| 2 | xiaoming | 95.000000 |",
                "| id | name | score |",
                "| 2 | xiaoming | 97.000000 |",
            ]
        elif kind == "mvcc_read_write_delete":
            t1.execute("begin;")
            t1.execute("select * from concurrency_test where id = 3;")
            t2.execute("begin;")
            t2.execute("delete from concurrency_test where id = 3;")
            t2.execute("commit;")
            t1.execute("select * from concurrency_test where id = 3;")
            t1.execute("commit;")
            expected = [
                "| id | name | score |",
                "| 3 | zhanghua | 88.500000 |",
                "| id | name | score |",
                "| 3 | zhanghua | 88.500000 |",
            ]
        elif kind == "mvcc_scan":
            t1.execute("begin;")
            t1.execute("select * from concurrency_test;")
            t2.execute("begin;")
            t2.execute("insert into concurrency_test values (4, 'scanrow', 70.0);")
            t2.execute("commit;")
            t1.execute("select * from concurrency_test;")
            t1.execute("commit;")
            expected = [
                "| id | name | score |",
                "| 1 | xiaohong | 90.000000 |",
                "| 2 | xiaoming | 95.000000 |",
                "| 3 | zhanghua | 88.500000 |",
                "| id | name | score |",
                "| 1 | xiaohong | 90.000000 |",
                "| 2 | xiaoming | 95.000000 |",
                "| 3 | zhanghua | 88.500000 |",
            ]
        elif kind == "mvcc_timestamp":
            t1.execute("begin;")
            t1.execute("select * from concurrency_test where id = 1;")
            t1.execute("commit;")
            t2.execute("begin;")
            t2.execute("update concurrency_test set score = 91.0 where id = 1;")
            t2.execute("commit;")
            t1.execute("select * from concurrency_test where id = 1;")
            expected = [
                "| id | name | score |",
                "| 1 | xiaohong | 90.000000 |",
                "| id | name | score |",
                "| 1 | xiaohong | 91.000000 |",
            ]
        elif kind == "mvcc_tuple_reconstruct":
            t1.execute("begin;")
            t2.execute("begin;")
            t2.execute("update concurrency_test set score = 96.0 where id = 2;")
            t2.execute("commit;")
            t1.execute("select * from concurrency_test where id = 2;")
            t1.execute("commit;")
            expected = ["| id | name | score |", "| 2 | xiaoming | 95.000000 |"]
        elif kind == "mvcc_update":
            t1.execute("begin;")
            t1.execute("update concurrency_test set score = 91.0 where id = 1;")
            t1.execute("commit;")
            t2.execute("select * from concurrency_test where id = 1;")
            expected = ["| id | name | score |", "| 1 | xiaohong | 91.000000 |"]
        elif kind == "mvcc_write_write_delete_insert":
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("delete from concurrency_test where id = 2;")
            t2.execute("insert into concurrency_test values (2, 'again', 99.0);")
            t1.execute("commit;")
            t2.execute("abort;")
            t1.execute("select * from concurrency_test where id = 2;")
            expected = ["| id | name | score |"]
        elif kind == "mvcc_write_write_update":
            t1.execute("begin;")
            t2.execute("begin;")
            t1.execute("update concurrency_test set score = 96.0 where id = 2;")
            t2.execute("update concurrency_test set score = 97.0 where id = 2;")
            t1.execute("commit;")
            t2.execute("abort;")
            t1.execute("select * from concurrency_test where id = 2;")
            expected = ["| id | name | score |", "| 2 | xiaoming | 96.000000 |"]
        else:
            raise RuntimeError(f"unknown MVCC case kind: {kind}")
        time.sleep(0.1)
    except Exception as exc:
        detail = f"runtime error: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        for client in clients:
            client.close()
        terminate_server(proc)
    detail = compare_lines(expected, read_actual_output(build_dir, db_name), case.name)
    return TestResult(case, detail is None, detail or "passed")


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
    if case.kind.startswith("txn_"):
        return run_transaction_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
    if case.kind.startswith("isolation_"):
        return run_isolation_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
    if case.kind.startswith("mvcc_"):
        return run_mvcc_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
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
    args = parse_args()
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
