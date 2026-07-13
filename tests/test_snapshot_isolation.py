#!/usr/bin/env python3

import argparse
from pathlib import Path
import shutil

from test_mvcc_gc import Client, ROOT, require_success, start_server, stop_server


def int_rows(response: str) -> list[tuple[int, ...]]:
    rows: list[tuple[int, ...]] = []
    for line in response.splitlines():
        if not line.strip().startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        try:
            rows.append(tuple(int(cell) for cell in cells))
        except ValueError:
            continue
    return rows


def require_rows(response: str, expected: list[tuple[int, ...]], sql: str) -> None:
    actual = int_rows(response)
    if actual != expected:
        raise AssertionError(f"unexpected rows for {sql!r}: expected {expected}, got {actual}; response={response!r}")


def set_si(client: Client) -> None:
    require_success(
        client.execute("set transaction isolation level snapshot isolation;"),
        "set transaction isolation level snapshot isolation",
    )


def run_snapshot_regression(port: int, server: Path, build_dir: Path, db_name: str, log_path: Path) -> None:
    proc, log_file = start_server(server, build_dir, db_name, port, log_path)
    clients: list[Client] = []
    try:
        admin = Client(port)
        reader = Client(port)
        writer = Client(port)
        contender = Client(port)
        clients.extend([admin, reader, writer, contender])

        require_success(admin.execute("set output_file off;"), "set output_file off")
        require_success(admin.execute("create table si_indexed (id int, k int, value int);"), "create table")
        require_success(admin.execute("create index si_indexed(k);"), "create index")
        require_success(admin.execute("insert into si_indexed values (1, 10, 100);"), "insert row 1")
        require_success(admin.execute("insert into si_indexed values (2, 20, 200);"), "insert row 2")
        require_success(admin.execute("create table si_min (group_id int, k int);"), "create min table")
        require_success(admin.execute("create index si_min(group_id,k);"), "create min index")
        require_success(admin.execute("insert into si_min values (1, 10);"), "insert min row 1")
        require_success(admin.execute("insert into si_min values (1, 20);"), "insert min row 2")
        require_rows(admin.execute("select min(k) from si_min where group_id = 1;"), [(10,)], "seed default SI plan")

        # reader and writer intentionally rely on the server default; keep one
        # explicit SET to cover the configurable SI path as well.
        set_si(contender)

        require_success(reader.execute("begin;"), "reader begin")
        require_rows(reader.execute("select * from si_indexed where k = 10;"), [(1, 10, 100)], "reader old key")
        require_rows(reader.execute("select count(k) from si_indexed where k < 25;"), [(2,)], "reader old count")
        require_rows(reader.execute("select min(k) from si_min where group_id = 1;"), [(10,)], "reader old min")

        require_success(writer.execute("begin;"), "writer begin")
        require_success(writer.execute("update si_indexed set k = 30, value = 300 where id = 1;"), "move key")
        require_success(writer.execute("delete from si_indexed where id = 2;"), "delete row")
        require_success(writer.execute("insert into si_indexed values (3, 40, 400);"), "insert row 3")
        require_success(writer.execute("insert into si_min values (1, 5);"), "insert new min")
        require_rows(writer.execute("select * from si_indexed where k = 30;"), [(1, 30, 300)], "read own update")
        require_rows(writer.execute("select * from si_indexed where k = 40;"), [(3, 40, 400)], "read own insert")
        require_rows(writer.execute("select count(k) from si_indexed where k >= 30;"), [(2,)], "count own writes")
        require_success(writer.execute("commit;"), "writer commit")

        require_rows(reader.execute("select * from si_indexed where k = 10;"), [(1, 10, 100)], "old moved key")
        require_rows(reader.execute("select * from si_indexed where k = 20;"), [(2, 20, 200)], "old deleted key")
        require_rows(reader.execute("select * from si_indexed where k = 30;"), [], "new key hidden")
        require_rows(reader.execute("select * from si_indexed where k = 40;"), [], "insert hidden")
        require_rows(reader.execute("select count(k) from si_indexed where k < 25;"), [(2,)], "old count")
        require_rows(reader.execute("select min(k) from si_min where group_id = 1;"), [(10,)], "new min hidden")

        stale_write = reader.execute("update si_indexed set value = 301 where id = 1;")
        if "abort" not in stale_write.lower():
            raise AssertionError(f"expected stale snapshot write to abort, got {stale_write!r}")

        require_rows(admin.execute("select * from si_indexed where k = 30;"), [(1, 30, 300)], "current moved key")
        require_rows(admin.execute("select * from si_indexed where k = 20;"), [], "current deleted key")
        require_rows(admin.execute("select * from si_indexed where k = 40;"), [(3, 40, 400)], "current insert")
        require_rows(admin.execute("select count(k) from si_indexed where k < 25;"), [(0,)], "current count")

        require_success(writer.execute("begin;"), "writer conflict begin")
        require_success(contender.execute("begin;"), "contender conflict begin")
        require_success(writer.execute("update si_indexed set value = 301 where id = 1;"), "writer conflict update")
        conflict = contender.execute("update si_indexed set value = 302 where id = 1;")
        if "abort" not in conflict.lower():
            raise AssertionError(f"expected concurrent write to abort, got {conflict!r}")
        require_success(writer.execute("commit;"), "writer conflict commit")
        require_rows(admin.execute("select * from si_indexed where k = 30;"), [(1, 30, 301)], "conflict winner")
    finally:
        for client in clients:
            client.close()
        stop_server(proc, log_file)


def main() -> int:
    parser = argparse.ArgumentParser(description="Targeted snapshot-isolation index visibility regression")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--server", type=Path)
    parser.add_argument("--port", type=int, default=18775)
    parser.add_argument("--keep-db", action="store_true")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    server = args.server.resolve() if args.server else build_dir / "bin" / "rmdb"
    db_name = "snapshot_isolation_regression_db"
    db_dir = build_dir / db_name
    if db_dir.exists():
        shutil.rmtree(db_dir)

    log_path = build_dir / "snapshot_isolation.server.log"
    try:
        run_snapshot_regression(args.port, server, build_dir, db_name, log_path)
        print("PASS: SI indexed snapshot visibility, own writes, and write conflicts")
        return 0
    finally:
        if not args.keep_db and db_dir.exists():
            shutil.rmtree(db_dir)


if __name__ == "__main__":
    raise SystemExit(main())
