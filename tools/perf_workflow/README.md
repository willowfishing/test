# RMDB Performance Workflow

This directory contains a reusable local workflow for RMDB performance work.

## Files

- `install_optional_tools.sh`: installs user-local helpers under `~/.local` without `sudo`.
- `run_workflow.sh`: runs build, optional TPC-C init/check, benchmark, `perf`, `callgrind`, and `heaptrack`.
- `summarize_perf_run.py`: generates a short Markdown summary for each run directory.
- `run_stable_baseline.sh`: runs repeated benchmark baselines serially and aggregates the results.
- `summarize_baseline_runs.py`: builds an aggregate summary across repeated baseline runs.
- `collect_memory_wall_telemetry.py`: samples process, cgroup, network, and file residency every five seconds.
- `analyze_memory_wall.py`: applies the fixed last-60-second memory-wall classification rule.
- `memory_wall_studies.json`: immutable research-only `dual-360-8g/7g/6g` campaign definitions.

## Output layout

Each run writes to:

```text
RMDB/performance_test_record/<timestamp>_<label>/
```

Typical artifacts include:

- `manifest.txt`
- `system_info.txt`
- `tool_status.txt`
- `server.log`
- `benchmark.log`
- `perf/perf_stat.csv`
- `perf/perf.data`
- `perf/perf.svg`
- `callgrind/callgrind.out`
- `callgrind/callgrind_annotate.txt`
- `heaptrack/heaptrack.data.gz`
- `heaptrack/heaptrack_print.txt`
- `summary.md`
- `memory/<phase>.jsonl`
- `memory/<phase>.analysis.md`

## Common commands

Install or relink optional tools:

```bash
./tools/perf_workflow/install_optional_tools.sh
```

Run the full workflow:

```bash
./tools/perf_workflow/run_workflow.sh \
  --mode all \
  --label sf1_t16_official \
  --db-name tpcc_sf1 \
  --init-db \
  --check
```

Run only throughput + latency benchmark with the fixed official-style window:

```bash
./tools/perf_workflow/run_workflow.sh \
  --mode benchmark \
  --label official_like \
  --db-name tpcc_sf1 \
  --init-db \
  --check
```

Add `--memory-telemetry` to any workflow mode to collect `VmRSS`, `RssAnon`, `RssFile`, cgroup
`memory.events`/PSI, system network counters, process I/O, and WAL/non-WAL `fincore -J -b`
residency. Use `--memory-cgroup-path <path>` when the RMDB process is launched outside the target
cgroup and automatic cgroup-v2 discovery is not sufficient. Missing `fincore` is recorded as
unavailable without failing the benchmark.

The fixed research campaign in `memory_wall_studies.json` is excluded from ranking. It preserves
the `e90120d` and `0298e5c` baselines, runs two rounds with swapped slots, and keeps the official
`scale=50`, `16 clients`, `30+360 seconds` workload unchanged under 8/7/6 GiB memory limits.

Run the repo steady-state mode used for lock/stat comparisons:

```bash
./tools/perf_workflow/run_workflow.sh \
  --mode steady-state \
  --label steady_state_t4 \
  --db-name tpcc_sf1 \
  --init-db \
  --check
```

This keeps one server process alive across warmup and measure and uses:

- scale `1`
- threads `4`
- warmup transactions `1000` per thread (`4000` total)
- measure transactions `5000` per thread (`20000` total)

Run only `perf`:

```bash
./tools/perf_workflow/run_workflow.sh \
  --mode perf \
  --label perf_only \
  --db-name tpcc_sf1 \
  --perf-record-seconds 30
```

Run a stable repeated baseline:

```bash
./tools/perf_workflow/run_stable_baseline.sh \
  --label rmdb_official_stable \
  --db-basename tpcc_rmdb_official_stable \
  --runs 5 \
  --skip-build
```

This creates:

```text
RMDB/performance_test_record/<timestamp>_<label>/
```

with one child directory per run plus:

- `baseline_manifest.txt`
- `baseline_summary.md`

## GitHub Actions CI

The same TPC-C flow is available as a manual GitHub Actions workflow:

```text
TPCC Performance
```

It starts the configured GCP self-hosted runner VM, runs on the `rmdb/t2d60/asia-south1` runner labels, and stops the VM in an `always()` cleanup job. It defaults to the current quick official-style test:

- scale: `1`
- threads: `16`
- warmup_seconds: `30`
- measure_seconds: `60`
- rw_ratio: `0.84`
- txn_probs: `9 9 5 1 1`
- build flags: `-O0 -g -fno-omit-frame-pointer`
- runs: `1`

Set `runs` to `3` when you need a stable median. Increase `timeout_minutes` accordingly.

Each CI run creates a clean temporary workspace under the self-hosted runner temp directory:

```text
${{ runner.temp }}/rmdb-tpcc-<run_id>-<attempt>/
```

The workflow exports the checked-out commit into that directory, then builds RMDB, clones/builds `TPCC-Tester`, creates the TPCC database, and writes performance artifacts there. It does not reuse the manual remote working directory.

The lifecycle jobs use the same repository variables as `Performance CI`:

- `GCP_WORKLOAD_IDENTITY_PROVIDER`
- `GCP_SERVICE_ACCOUNT`
- `GCP_RUNNER_INSTANCE`
- `GCP_PROJECT_ID`
- `GCP_RUNNER_ZONE`

## Environment notes

- `TPCC-Tester` defaults to a sibling checkout at `<workspace>/TPCC-Tester`. The script clones and builds it if missing.
- `perf` hardware counters are not available on this WSL2 host; the workflow uses software events and `cpu-clock` sampling.
- The installer now prefers already-installed system tools.
- If `~/FlameGraph` exists, the workflow prefers that checkout and symlinks `flamegraph.pl` and `stackcollapse-perf.pl` from it.
- `heaptrack_gui` and `hotspot` are now expected to be available from the system install.
- `trace_processor` and `traceconv` are installed as official Perfetto wrappers and can be used later for trace analysis.
- `run_workflow.sh` now cleans the temporary database directory by default after each run, so `performance_test_record/` remains the only persistent result store. Use `--keep-db-artifacts` only when you explicitly need the database files.
