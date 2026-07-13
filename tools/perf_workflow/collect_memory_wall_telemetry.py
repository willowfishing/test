#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import signal
import subprocess
import time
from typing import Any


STOP_REQUESTED = False


def request_stop(_signum: int, _frame: object) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def read_text(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def read_int(path: pathlib.Path) -> int | None:
    value = read_text(path).strip()
    if not value or value == "max":
        return None
    try:
        return int(value)
    except ValueError:
        return None


def parse_proc_status(pid: int) -> dict[str, int]:
    wanted = {"VmRSS", "RssAnon", "RssFile"}
    result: dict[str, int] = {}
    for line in read_text(pathlib.Path(f"/proc/{pid}/status")).splitlines():
        key, separator, raw_value = line.partition(":")
        if not separator or key not in wanted:
            continue
        fields = raw_value.split()
        if fields:
            try:
                result[f"{key}_bytes"] = int(fields[0]) * 1024
            except ValueError:
                pass
    return result


def parse_proc_stat(pid: int) -> dict[str, int]:
    raw = read_text(pathlib.Path(f"/proc/{pid}/stat")).strip()
    right_paren = raw.rfind(")")
    if right_paren < 0:
        return {}
    fields = raw[right_paren + 2 :].split()
    if len(fields) < 13:
        return {}
    try:
        return {
            "user_ticks": int(fields[11]),
            "system_ticks": int(fields[12]),
            "cpu_ticks": int(fields[11]) + int(fields[12]),
        }
    except ValueError:
        return {}


def parse_proc_io(pid: int) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in read_text(pathlib.Path(f"/proc/{pid}/io")).splitlines():
        key, separator, raw_value = line.partition(":")
        if not separator:
            continue
        try:
            result[key.strip()] = int(raw_value.strip())
        except ValueError:
            pass
    return result


def parse_network_totals() -> dict[str, int]:
    rx_bytes = 0
    tx_bytes = 0
    for line in read_text(pathlib.Path("/proc/net/dev")).splitlines()[2:]:
        _name, separator, raw_values = line.partition(":")
        if not separator:
            continue
        fields = raw_values.split()
        if len(fields) < 9:
            continue
        try:
            rx_bytes += int(fields[0])
            tx_bytes += int(fields[8])
        except ValueError:
            continue
    return {"rx_bytes": rx_bytes, "tx_bytes": tx_bytes}


def discover_cgroup_path(pid: int, override: str | None) -> tuple[pathlib.Path, int] | None:
    if override:
        path = pathlib.Path(override)
        return path, 2 if (path / "memory.current").exists() else 1
    unified_relative: str | None = None
    memory_relative: str | None = None
    for line in read_text(pathlib.Path(f"/proc/{pid}/cgroup")).splitlines():
        hierarchy, controllers, relative = line.split(":", 2)
        if hierarchy == "0" and controllers == "":
            unified_relative = relative
        elif "memory" in controllers.split(","):
            memory_relative = relative
    if unified_relative is not None:
        for root in (pathlib.Path("/sys/fs/cgroup"), pathlib.Path("/sys/fs/cgroup/unified")):
            candidate = root / unified_relative.lstrip("/")
            if (candidate / "memory.current").exists():
                return candidate, 2
    if memory_relative is not None:
        candidate = pathlib.Path("/sys/fs/cgroup/memory") / memory_relative.lstrip("/")
        if (candidate / "memory.usage_in_bytes").exists():
            return candidate, 1
    return None


def parse_counter_file(path: pathlib.Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in read_text(path).splitlines():
        fields = line.split()
        if len(fields) != 2:
            continue
        try:
            result[fields[0]] = int(fields[1])
        except ValueError:
            pass
    return result


def parse_pressure(path: pathlib.Path) -> dict[str, dict[str, float | int]]:
    result: dict[str, dict[str, float | int]] = {}
    for line in read_text(path).splitlines():
        fields = line.split()
        if not fields:
            continue
        values: dict[str, float | int] = {}
        for field in fields[1:]:
            key, separator, raw_value = field.partition("=")
            if not separator:
                continue
            try:
                values[key] = int(raw_value) if key == "total" else float(raw_value)
            except ValueError:
                pass
        result[fields[0]] = values
    return result


def sample_cgroup(location: tuple[pathlib.Path, int] | None) -> dict[str, Any]:
    if location is None:
        return {}
    path, version = location
    if version == 2:
        pressure_path = path / "memory.pressure"
        pressure_scope = "cgroup"
        if not pressure_path.exists():
            pressure_path = pathlib.Path("/proc/pressure/memory")
            pressure_scope = "system"
        return {
            "path": str(path),
            "version": version,
            "memory_current_bytes": read_int(path / "memory.current"),
            "memory_max_bytes": read_int(path / "memory.max"),
            "memory_events": parse_counter_file(path / "memory.events"),
            "memory_pressure": parse_pressure(pressure_path),
            "memory_pressure_scope": pressure_scope,
        }

    oom_control = parse_counter_file(path / "memory.oom_control")
    return {
        "path": str(path),
        "version": version,
        "memory_current_bytes": read_int(path / "memory.usage_in_bytes"),
        "memory_max_bytes": read_int(path / "memory.limit_in_bytes"),
        "memory_events": {
            "max": read_int(path / "memory.failcnt") or 0,
            "oom": oom_control.get("under_oom", 0),
            "oom_kill": oom_control.get("oom_kill", 0),
        },
        "memory_pressure": parse_pressure(pathlib.Path("/proc/pressure/memory")),
        "memory_pressure_scope": "system",
    }


def numeric_bytes(value: object) -> int | None:
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if not isinstance(value, str):
        return None
    stripped = value.strip().replace(",", "")
    try:
        return int(stripped)
    except ValueError:
        return None


def sample_fincore(db_dir: pathlib.Path, fincore_path: str | None) -> dict[str, Any]:
    files = sorted(path for path in db_dir.iterdir() if path.is_file()) if db_dir.is_dir() else []
    if fincore_path is None:
        return {"available": False, "entries": []}
    if not files:
        return {"available": True, "entries": []}

    command = [fincore_path, "-J", "-b", "--", *(str(path) for path in files)]
    try:
        completed = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
        document = json.loads(completed.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        return {"available": True, "error": str(error), "entries": []}

    raw_entries = document.get("fincore", []) if isinstance(document, dict) else []
    entries: list[dict[str, Any]] = []
    for raw_entry in raw_entries:
        if not isinstance(raw_entry, dict):
            continue
        file_name = str(raw_entry.get("file", raw_entry.get("name", "")))
        path = pathlib.Path(file_name)
        size = numeric_bytes(raw_entry.get("size"))
        resident = numeric_bytes(raw_entry.get("resident", raw_entry.get("res")))
        entries.append(
            {
                "path": file_name,
                "kind": "wal" if path.name == "db.log" else "non_wal",
                "size_bytes": size,
                "resident_bytes": resident,
            }
        )
    return {"available": True, "entries": entries}


def process_exists(pid: int) -> bool:
    return pathlib.Path(f"/proc/{pid}").exists()


def main() -> int:
    parser = argparse.ArgumentParser(description="Collect RMDB memory-wall telemetry as JSON lines")
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--db-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--interval-seconds", type=float, default=5.0)
    parser.add_argument("--cgroup-path")
    args = parser.parse_args()

    if args.interval_seconds <= 0:
        parser.error("--interval-seconds must be positive")

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    cgroup_path = discover_cgroup_path(args.pid, args.cgroup_path)
    fincore_path = shutil.which("fincore")
    started = time.monotonic()
    next_sample = started

    with args.output.open("w", encoding="utf-8") as output:
        while not STOP_REQUESTED and process_exists(args.pid):
            now_monotonic = time.monotonic()
            sample = {
                "timestamp_unix": time.time(),
                "elapsed_seconds": now_monotonic - started,
                "pid": args.pid,
                "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
                "process": {
                    **parse_proc_status(args.pid),
                    **parse_proc_stat(args.pid),
                    "io": parse_proc_io(args.pid),
                },
                "network": parse_network_totals(),
                "cgroup": sample_cgroup(cgroup_path),
                "files": sample_fincore(args.db_dir, fincore_path),
            }
            output.write(json.dumps(sample, sort_keys=True, separators=(",", ":")) + "\n")
            output.flush()

            next_sample += args.interval_seconds
            delay = max(0.0, next_sample - time.monotonic())
            if delay > 0:
                time.sleep(delay)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
