#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_WORKFLOW="${SCRIPT_DIR}/run_workflow.sh"
SUMMARIZE="${SCRIPT_DIR}/summarize_baseline_runs.py"
DEFAULT_RMDB_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RECORD_ROOT_DEFAULT="${DEFAULT_RMDB_DIR}/performance_test_record"

LABEL="stable_baseline"
DB_BASENAME="tpcc_stable_baseline"
TARGET_DIR="${DEFAULT_RMDB_DIR}"
BUILD_DIR="build-perf"
RECORD_ROOT="${RECORD_ROOT_DEFAULT}"
RUNS=5
HOST="127.0.0.1"
PORT="8765"
SCALE="1"
THREADS="16"
TRANSACTIONS="1000000"
WARMUP_TRANSACTIONS="40"
WARMUP_SECONDS="30"
MEASURE_SECONDS="60"
TIMED_SHUTDOWN_GRACE="10m"
RW_RATIO="0.84"
TXN_PROBS="9 9 5 1 1"
SKIP_BUILD=0
RUN_CHECK=1
BASELINE_ROOT=""

usage() {
  cat <<'EOF'
Usage:
  run_stable_baseline.sh [options]

Options:
  --label <name>
  --db-basename <name>
  --target-dir <dir>
  --build-dir <dir>
  --record-root <dir>
  --runs <n>
  --host <host>
  --port <port>
  --scale <n>
  --threads <n>
  --transactions <n>
  --warmup-transactions <n>
  --warmup-seconds <n>
  --measure-seconds <n>
  --timed-shutdown-grace <duration>
  --rw-ratio <f>
  --txn-probs "<a b c d e>"
  --skip-build
  --skip-check
  --help
EOF
}

log() {
  printf '[run_stable_baseline] %s\n' "$*"
}

die() {
  printf '[run_stable_baseline] ERROR: %s\n' "$*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --label) LABEL="$2"; shift 2 ;;
    --db-basename) DB_BASENAME="$2"; shift 2 ;;
    --target-dir) TARGET_DIR="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --record-root) RECORD_ROOT="$2"; shift 2 ;;
    --runs) RUNS="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --scale) SCALE="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --transactions) TRANSACTIONS="$2"; shift 2 ;;
    --warmup-transactions) WARMUP_TRANSACTIONS="$2"; shift 2 ;;
    --warmup-seconds) WARMUP_SECONDS="$2"; shift 2 ;;
    --measure-seconds) MEASURE_SECONDS="$2"; shift 2 ;;
    --timed-shutdown-grace) TIMED_SHUTDOWN_GRACE="$2"; shift 2 ;;
    --rw-ratio) RW_RATIO="$2"; shift 2 ;;
    --txn-probs) TXN_PROBS="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --skip-check) RUN_CHECK=0; shift ;;
    --help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ "${RUNS}" =~ ^[0-9]+$ ]] || die "--runs must be an integer"
(( RUNS > 0 )) || die "--runs must be positive"

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
BASELINE_ROOT="${RECORD_ROOT}/${TIMESTAMP}_${LABEL}"
mkdir -p "${BASELINE_ROOT}"

cat >"${BASELINE_ROOT}/baseline_manifest.txt" <<EOF
label=${LABEL}
db_basename=${DB_BASENAME}
target_dir=${TARGET_DIR}
build_dir=${BUILD_DIR}
runs=${RUNS}
host=${HOST}
port=${PORT}
scale=${SCALE}
threads=${THREADS}
transactions=${TRANSACTIONS}
warmup_transactions=${WARMUP_TRANSACTIONS}
warmup_seconds=${WARMUP_SECONDS}
measure_seconds=${MEASURE_SECONDS}
timed_shutdown_grace=${TIMED_SHUTDOWN_GRACE}
rw_ratio=${RW_RATIO}
txn_probs=${TXN_PROBS}
skip_build=${SKIP_BUILD}
run_check=${RUN_CHECK}
EOF

for run_idx in $(seq 1 "${RUNS}"); do
  run_label="$(printf '%s_run%02d' "${LABEL}" "${run_idx}")"
  run_db_name="$(printf '%s_run%02d' "${DB_BASENAME}" "${run_idx}")"
  cmd=(
    "${RUN_WORKFLOW}"
    --mode benchmark
    --label "${run_label}"
    --db-name "${run_db_name}"
    --target-dir "${TARGET_DIR}"
    --build-dir "${BUILD_DIR}"
    --record-root "${BASELINE_ROOT}"
    --host "${HOST}"
    --port "${PORT}"
    --scale "${SCALE}"
    --threads "${THREADS}"
    --transactions "${TRANSACTIONS}"
    --warmup-transactions "${WARMUP_TRANSACTIONS}"
    --warmup-seconds "${WARMUP_SECONDS}"
    --measure-seconds "${MEASURE_SECONDS}"
    --timed-shutdown-grace "${TIMED_SHUTDOWN_GRACE}"
    --rw-ratio "${RW_RATIO}"
    --init-db
  )
  if [[ "${RUN_CHECK}" == "1" ]]; then
    cmd+=(--check)
  fi
  if [[ "${SKIP_BUILD}" == "1" ]]; then
    cmd+=(--skip-build)
  fi
  if [[ -n "${TXN_PROBS}" ]]; then
    cmd+=(--txn-probs "${TXN_PROBS}")
  fi

  log "starting run ${run_idx}/${RUNS}: ${run_label}"
  "${cmd[@]}"
done

python3 "${SUMMARIZE}" "${BASELINE_ROOT}" >"${BASELINE_ROOT}/baseline_summary.md"
log "baseline summary written to ${BASELINE_ROOT}/baseline_summary.md"
log "baseline root: ${BASELINE_ROOT}"
