#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any, Callable


def load_samples(path: pathlib.Path) -> list[dict[str, Any]]:
    samples: list[dict[str, Any]] = []
    with path.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict) and isinstance(value.get("elapsed_seconds"), (int, float)):
                samples.append(value)
    return sorted(samples, key=lambda sample: float(sample["elapsed_seconds"]))


def nested_number(sample: dict[str, Any], *keys: str) -> float | None:
    current: Any = sample
    for key in keys:
        if not isinstance(current, dict):
            return None
        current = current.get(key)
    return float(current) if isinstance(current, (int, float)) else None


def file_total(sample: dict[str, Any], kind: str, field: str) -> float | None:
    files = sample.get("files", {})
    entries = files.get("entries", []) if isinstance(files, dict) else []
    values = [entry.get(field) for entry in entries if isinstance(entry, dict) and entry.get("kind") == kind]
    numeric = [float(value) for value in values if isinstance(value, (int, float))]
    return sum(numeric) if numeric else None


def window(samples: list[dict[str, Any]], begin: float, end: float) -> list[dict[str, Any]]:
    return [sample for sample in samples if begin <= float(sample["elapsed_seconds"]) <= end]


def counter_delta(samples: list[dict[str, Any]], getter: Callable[[dict[str, Any]], float | None]) -> float:
    values = [(float(sample["elapsed_seconds"]), getter(sample)) for sample in samples]
    values = [(timestamp, value) for timestamp, value in values if value is not None]
    if len(values) < 2:
        return 0.0
    return max(0.0, values[-1][1] - values[0][1])


def counter_rate(samples: list[dict[str, Any]], getter: Callable[[dict[str, Any]], float | None]) -> float:
    values = [(float(sample["elapsed_seconds"]), getter(sample)) for sample in samples]
    values = [(timestamp, value) for timestamp, value in values if value is not None]
    if len(values) < 2 or values[-1][0] <= values[0][0]:
        return 0.0
    return max(0.0, values[-1][1] - values[0][1]) / (values[-1][0] - values[0][0])


def peak(samples: list[dict[str, Any]], getter: Callable[[dict[str, Any]], float | None]) -> int:
    values = [getter(sample) for sample in samples]
    numeric = [value for value in values if value is not None]
    return int(max(numeric)) if numeric else 0


def mib(value: int | float) -> str:
    return f"{float(value) / (1024 * 1024):.1f} MiB"


def main() -> int:
    parser = argparse.ArgumentParser(description="Classify RMDB memory-wall telemetry")
    parser.add_argument("telemetry", type=pathlib.Path)
    args = parser.parse_args()

    samples = load_samples(args.telemetry)
    if len(samples) < 2:
        print("# Memory Wall Analysis\n\nInsufficient telemetry samples.")
        return 0

    final_time = float(samples[-1]["elapsed_seconds"])
    later = window(samples, max(0.0, final_time - 60.0), final_time)
    earlier = window(samples, max(0.0, final_time - 120.0), max(0.0, final_time - 60.0))

    cpu_getter = lambda sample: nested_number(sample, "process", "cpu_ticks")
    network_getter = lambda sample: (
        (nested_number(sample, "network", "rx_bytes") or 0.0)
        + (nested_number(sample, "network", "tx_bytes") or 0.0)
    )
    wal_getter = lambda sample: file_total(sample, "wal", "size_bytes")
    psi_getter = lambda sample: nested_number(sample, "cgroup", "memory_pressure", "some", "total")

    earlier_rates = {
        "cpu_ticks_per_second": counter_rate(earlier, cpu_getter),
        "network_bytes_per_second": counter_rate(earlier, network_getter),
        "wal_growth_bytes_per_second": counter_rate(earlier, wal_getter),
    }
    later_rates = {
        "cpu_ticks_per_second": counter_rate(later, cpu_getter),
        "network_bytes_per_second": counter_rate(later, network_getter),
        "wal_growth_bytes_per_second": counter_rate(later, wal_getter),
    }

    event_names = ("max", "oom", "oom_kill")
    event_growth = {
        name: counter_delta(later, lambda sample, event=name: nested_number(sample, "cgroup", "memory_events", event))
        for name in event_names
    }
    psi_growth = counter_delta(later, psi_getter)
    pressure_observed = psi_growth > 0 or any(value > 0 for value in event_growth.values())
    drops = {
        name: earlier_rates[name] > 0 and later_rates[name] < earlier_rates[name] * 0.8
        for name in earlier_rates
    }
    memory_wall = pressure_observed and any(drops.values())

    peak_rss = peak(samples, lambda sample: nested_number(sample, "process", "VmRSS_bytes"))
    peak_anon = peak(samples, lambda sample: nested_number(sample, "process", "RssAnon_bytes"))
    peak_file = peak(samples, lambda sample: nested_number(sample, "process", "RssFile_bytes"))
    peak_wal_resident = peak(samples, lambda sample: file_total(sample, "wal", "resident_bytes"))
    peak_non_wal_resident = peak(samples, lambda sample: file_total(sample, "non_wal", "resident_bytes"))

    print("# Memory Wall Analysis")
    print()
    print(f"- classification: `{'memory-wall' if memory_wall else 'not-proven'}`")
    print(f"- telemetry: `{args.telemetry}`")
    print(f"- samples: {len(samples)} over {final_time:.1f} seconds")
    print(f"- peak VmRSS: {mib(peak_rss)}")
    print(f"- peak RssAnon: {mib(peak_anon)}")
    print(f"- peak RssFile: {mib(peak_file)}")
    print(f"- peak WAL resident: {mib(peak_wal_resident)}")
    print(f"- peak non-WAL resident: {mib(peak_non_wal_resident)}")
    print(f"- later-window memory PSI total growth: {psi_growth:.0f}")
    for name in event_names:
        print(f"- later-window memory.events {name} growth: {event_growth[name]:.0f}")
    print()
    print("## Rate Comparison")
    print()
    for name in earlier_rates:
        drop = "yes" if drops[name] else "no"
        print(
            f"- {name}: previous={earlier_rates[name]:.2f}, later={later_rates[name]:.2f}, "
            f"drop_gt_20_percent={drop}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
