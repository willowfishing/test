#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import statistics
import sys
from dataclasses import dataclass


@dataclass
class RunMetrics:
    run_dir: pathlib.Path
    total_transactions: float
    successful: float
    failed_retried: float
    success_rate: float
    total_duration_sec: float
    average_response_ms: float
    throughput_tps: float
    tpmc: float


def read_text(path: pathlib.Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def parse_float(text: str, pattern: str) -> float | None:
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        return None
    raw = match.group(1).replace(",", "").strip()
    return float(raw)


def parse_metrics(run_dir: pathlib.Path) -> RunMetrics | None:
    benchmark_log = run_dir / "benchmark.log"
    text = read_text(benchmark_log)
    if not text:
        return None

    patterns = {
        "total_transactions": r"Total Transactions:\s+([0-9.]+)",
        "successful": r"Successful:\s+([0-9.]+)",
        "failed_retried": r"Failed \(retried\):\s+([0-9.]+)",
        "success_rate": r"Success Rate:\s+([0-9.]+)",
        "total_duration_sec": r"Total Duration:\s+([0-9.]+)\s+seconds",
        "average_response_ms": r"Average Response:\s+([0-9.]+)\s+ms",
        "throughput_tps": r"Throughput:\s+([0-9.]+)\s+TPS",
        "tpmc": r"tpmC:\s+([0-9.]+)",
    }
    values: dict[str, float] = {}
    for key, pattern in patterns.items():
        value = parse_float(text, pattern)
        if value is None:
            return None
        values[key] = value

    return RunMetrics(run_dir=run_dir, **values)


def fmt_num(value: float) -> str:
    return f"{value:.2f}"


def summarize_metric(values: list[float]) -> tuple[float, float, float, float]:
    avg = statistics.mean(values)
    med = statistics.median(values)
    min_v = min(values)
    max_v = max(values)
    return avg, med, min_v, max_v


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: summarize_baseline_runs.py <baseline-root>", file=sys.stderr)
        return 1

    baseline_root = pathlib.Path(sys.argv[1]).resolve()
    if not baseline_root.exists():
        print(f"baseline root not found: {baseline_root}", file=sys.stderr)
        return 1

    run_dirs = sorted(
        [p for p in baseline_root.iterdir() if p.is_dir() and (p / "benchmark.log").exists()]
    )
    metrics = [m for m in (parse_metrics(run_dir) for run_dir in run_dirs) if m is not None]
    if not metrics:
        print("no benchmark runs found", file=sys.stderr)
        return 1

    throughput_values = [m.throughput_tps for m in metrics]
    tpmc_values = [m.tpmc for m in metrics]
    duration_values = [m.total_duration_sec for m in metrics]
    response_values = [m.average_response_ms for m in metrics]

    throughput_avg, throughput_med, throughput_min, throughput_max = summarize_metric(throughput_values)
    tpmc_avg, tpmc_med, tpmc_min, tpmc_max = summarize_metric(tpmc_values)
    duration_avg, duration_med, duration_min, duration_max = summarize_metric(duration_values)
    response_avg, response_med, response_min, response_max = summarize_metric(response_values)

    manifest = read_text(baseline_root / "baseline_manifest.txt").strip()

    lines: list[str] = []
    lines.append("# Stable Baseline Summary")
    lines.append("")
    lines.append(f"- baseline_root: `{baseline_root}`")
    lines.append(f"- run_count: {len(metrics)}")

    if manifest:
        lines.append("")
        lines.append("## Baseline Config")
        for line in manifest.splitlines():
            lines.append(f"- {line}")

    lines.append("")
    lines.append("## Aggregate Metrics")
    lines.append(f"- Throughput TPS: avg `{fmt_num(throughput_avg)}`, median `{fmt_num(throughput_med)}`, min `{fmt_num(throughput_min)}`, max `{fmt_num(throughput_max)}`")
    lines.append(f"- tpmC: avg `{fmt_num(tpmc_avg)}`, median `{fmt_num(tpmc_med)}`, min `{fmt_num(tpmc_min)}`, max `{fmt_num(tpmc_max)}`")
    lines.append(f"- Total Duration sec: avg `{fmt_num(duration_avg)}`, median `{fmt_num(duration_med)}`, min `{fmt_num(duration_min)}`, max `{fmt_num(duration_max)}`")
    lines.append(f"- Average Response ms: avg `{fmt_num(response_avg)}`, median `{fmt_num(response_med)}`, min `{fmt_num(response_min)}`, max `{fmt_num(response_max)}`")

    lines.append("")
    lines.append("## Run Table")
    lines.append("| Run | Throughput TPS | tpmC | Duration sec | Avg Response ms | Success / Total | Retries |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|")
    for metric in metrics:
        lines.append(
            f"| `{metric.run_dir.name}` | {fmt_num(metric.throughput_tps)} | {fmt_num(metric.tpmc)} | "
            f"{fmt_num(metric.total_duration_sec)} | {fmt_num(metric.average_response_ms)} | "
            f"{int(metric.successful)} / {int(metric.total_transactions)} | {int(metric.failed_retried)} |"
        )

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
