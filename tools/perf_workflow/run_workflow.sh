#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_RMDB_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RMDB_DIR="${RMDB_DIR_OVERRIDE:-${DEFAULT_RMDB_DIR}}"
WORK_ROOT="$(cd "${RMDB_DIR}/.." && pwd)"
RECORD_ROOT_DEFAULT="${DEFAULT_RMDB_DIR}/performance_test_record"
RECORD_ROOT="${RECORD_ROOT_OVERRIDE:-${RECORD_ROOT_DEFAULT}}"
LOCAL_BIN="${HOME}/.local/bin"
SYSTEM_HEAPTRACK="${SYSTEM_HEAPTRACK_OVERRIDE:-/usr/bin/heaptrack}"
SYSTEM_HEAPTRACK_PRINT="${SYSTEM_HEAPTRACK_PRINT_OVERRIDE:-/usr/bin/heaptrack_print}"

export PATH="${LOCAL_BIN}:${PATH}"

MODE="all"
LABEL="manual"
BUILD_DIR="build-perf"
BUILD_TYPE="RelWithDebInfo"
DB_NAME="tpcc_sf1"
HOST="127.0.0.1"
PORT="8765"
SCALE="1"
THREADS="16"
TRANSACTIONS="1000000"
RW_RATIO="0.84"
TXN_PROBS="9 9 5 1 1"
TPCC_DIR="${TPCC_TESTER_DIR:-${WORK_ROOT}/TPCC-Tester}"
TPCC_BIN=""
TARGET_NAME="$(basename "${RMDB_DIR}")"
INIT_DB=0
RUN_CHECK=0
RUN_DIAGNOSE=0
SKIP_BUILD=0
SKIP_PERF_RECORD=0
PERF_RECORD_SECONDS="20"
CALLGRIND_TRANSACTIONS="60"
HEAPTRACK_TRANSACTIONS="60"
WARMUP_TRANSACTIONS="40"
WARMUP_SECONDS="30"
MEASURE_SECONDS="60"
TIMED_TRANSACTIONS_CAP="1000000"
TIMED_SHUTDOWN_GRACE="10m"
TPCC_TIMEOUT="5m"
DETACH=0
KEEP_DB_ARTIFACTS=0
MEMORY_TELEMETRY=0
MEMORY_CGROUP_PATH=""
RESULT_DIR=""
SERVER_PID=""
SERVER_LOG=""
MEMORY_TELEMETRY_PID=""
MEMORY_TELEMETRY_FILE=""
TPCC_BG_PID=""
TPCC_BG_PGID=""
TPCC_BG_WRAPPER_PID=""

usage() {
  cat <<'EOF'
Usage:
  run_workflow.sh [options]

Options:
  --mode <all|benchmark|perf|callgrind|heaptrack|tools|steady-state>
  --label <name>
  --db-name <name>
  --build-dir <dir>
  --target-dir <dir>
  --record-root <dir>
  --host <host>
  --port <port>
  --scale <n>
  --threads <n>
  --transactions <n>
  --rw-ratio <f>
  --txn-probs "<a b c d e>"
  --tpcc-dir <path>
  --init-db
  --check
  --diagnose
  --skip-build
  --skip-perf-record
  --perf-record-seconds <n>
  --callgrind-transactions <n>
  --heaptrack-transactions <n>
  --warmup-transactions <n>
  --warmup-seconds <n>
  --measure-seconds <n>
  --timed-shutdown-grace <duration>
  --tpcc-timeout <duration>
  --detach
  --keep-db-artifacts
  --memory-telemetry
  --memory-cgroup-path <path>
  --help
EOF
}

log() {
  printf '[run_workflow] %s\n' "$*"
}

die() {
  printf '[run_workflow] ERROR: %s\n' "$*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode) MODE="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    --db-name) DB_NAME="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --target-dir) RMDB_DIR="$2"; WORK_ROOT="$(cd "${RMDB_DIR}/.." && pwd)"; TARGET_NAME="$(basename "${RMDB_DIR}")"; shift 2 ;;
    --record-root) RECORD_ROOT="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --scale) SCALE="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --transactions) TRANSACTIONS="$2"; shift 2 ;;
    --rw-ratio) RW_RATIO="$2"; shift 2 ;;
    --txn-probs) TXN_PROBS="$2"; shift 2 ;;
    --tpcc-dir) TPCC_DIR="$2"; shift 2 ;;
    --init-db) INIT_DB=1; shift ;;
    --check) RUN_CHECK=1; shift ;;
    --diagnose) RUN_DIAGNOSE=1; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --skip-perf-record) SKIP_PERF_RECORD=1; shift ;;
    --perf-record-seconds) PERF_RECORD_SECONDS="$2"; shift 2 ;;
    --callgrind-transactions) CALLGRIND_TRANSACTIONS="$2"; shift 2 ;;
    --heaptrack-transactions) HEAPTRACK_TRANSACTIONS="$2"; shift 2 ;;
    --warmup-transactions) WARMUP_TRANSACTIONS="$2"; shift 2 ;;
    --warmup-seconds) WARMUP_SECONDS="$2"; shift 2 ;;
    --measure-seconds) MEASURE_SECONDS="$2"; shift 2 ;;
    --timed-shutdown-grace) TIMED_SHUTDOWN_GRACE="$2"; shift 2 ;;
    --tpcc-timeout) TPCC_TIMEOUT="$2"; shift 2 ;;
    --detach) DETACH=1; shift ;;
    --keep-db-artifacts) KEEP_DB_ARTIFACTS=1; shift ;;
    --memory-telemetry) MEMORY_TELEMETRY=1; shift ;;
    --memory-cgroup-path) MEMORY_CGROUP_PATH="$2"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

if [[ "${MODE}" == "steady-state" ]]; then
  THREADS="4"
  SCALE="1"
  TRANSACTIONS="5000"
  WARMUP_TRANSACTIONS="1000"
  WARMUP_SECONDS=""
  MEASURE_SECONDS=""
  TXN_PROBS="45 43 4 4 4"
fi

validate_seconds_option() {
  local name="$1"
  local value="$2"
  local allow_zero="$3"
  if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
    die "${name} must be an integer number of seconds"
  fi
  local numeric=$((10#${value}))
  if [[ "${allow_zero}" == "1" ]]; then
    if (( numeric < 0 )); then
      die "${name} must be non-negative"
    fi
  elif (( numeric <= 0 )); then
    die "${name} must be positive"
  fi
}

if [[ -n "${WARMUP_SECONDS}" ]]; then
  validate_seconds_option "--warmup-seconds" "${WARMUP_SECONDS}" 1
fi
if [[ -n "${MEASURE_SECONDS}" ]]; then
  validate_seconds_option "--measure-seconds" "${MEASURE_SECONDS}" 1
  if [[ "${MEASURE_SECONDS}" == "0" ]]; then
    MEASURE_SECONDS=""
  fi
fi

RUN_TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
RESULT_DIR="${RECORD_ROOT}/${RUN_TIMESTAMP}_${LABEL}"
mkdir -p "${RESULT_DIR}"
SERVER_LOG="${RESULT_DIR}/server.log"

cleanup_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill -INT "${SERVER_PID}" 2>/dev/null || true
    for _ in $(seq 1 40); do
      kill -0 "${SERVER_PID}" 2>/dev/null || break
      sleep 0.25
    done
    if kill -0 "${SERVER_PID}" 2>/dev/null; then
      kill -TERM "${SERVER_PID}" 2>/dev/null || true
      sleep 1
      kill -KILL "${SERVER_PID}" 2>/dev/null || true
    fi
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  stop_memory_telemetry
  while read -r pid; do
    [[ -z "${pid}" ]] && continue
    kill -TERM "${pid}" 2>/dev/null || true
  done < <(port_listener_pids)
  sleep 0.25
  SERVER_PID=""
}

cleanup() {
  cleanup_server
  if [[ "${DETACH}" == "0" && "${KEEP_DB_ARTIFACTS}" == "0" ]]; then
    cleanup_database_artifacts
  fi
}
trap cleanup EXIT

write_file() {
  local path="$1"
  shift
  printf '%s\n' "$@" >"${path}"
}

append_file() {
  local path="$1"
  shift
  printf '%s\n' "$@" >>"${path}"
}

tool_path() {
  which "$1" 2>/dev/null | head -n 1 || true
}

record_tool_status() {
  local status_file="${RESULT_DIR}/tool_status.txt"
  {
    echo "perf=$(tool_path perf)"
    echo "valgrind=$(tool_path valgrind)"
    echo "callgrind_annotate=$(tool_path callgrind_annotate)"
    echo "flamegraph.pl=$(tool_path flamegraph.pl)"
    echo "stackcollapse-perf.pl=$(tool_path stackcollapse-perf.pl)"
    echo "heaptrack=$(tool_path heaptrack)"
    echo "heaptrack_print=$(tool_path heaptrack_print)"
    echo "system_heaptrack=${SYSTEM_HEAPTRACK}"
    echo "system_heaptrack_print=${SYSTEM_HEAPTRACK_PRINT}"
    echo "heaptrack_gui=$(tool_path heaptrack_gui)"
    echo "hotspot=$(tool_path hotspot)"
    echo "trace_processor=$(tool_path trace_processor)"
    echo "traceconv=$(tool_path traceconv)"
    echo "home_flamegraph=${HOME}/FlameGraph"
  } >"${status_file}"
  if [[ -x "$(tool_path hotspot)" ]]; then
    {
      echo
      echo "[ldd hotspot]"
      ldd "$(tool_path hotspot)" || true
    } >>"${status_file}"
  fi
  if [[ -x "$(tool_path heaptrack_gui)" ]]; then
    {
      echo
      echo "[ldd heaptrack_gui]"
      ldd "$(tool_path heaptrack_gui)" || true
    } >>"${status_file}"
  fi
}

record_system_info() {
  {
    echo "date=$(date -Is 2>/dev/null || date '+%Y-%m-%dT%H:%M:%S%z')"
    echo "cwd=${WORK_ROOT}"
    echo "rmdb_dir=${RMDB_DIR}"
    echo "result_dir=${RESULT_DIR}"
    echo "kernel=$(uname -a)"
    echo "os_release="
    if [[ -r /etc/os-release ]]; then
      cat /etc/os-release
    else
      sw_vers 2>/dev/null || true
    fi
  } >"${RESULT_DIR}/system_info.txt"
}

record_manifest() {
  {
    echo "mode=${MODE}"
    echo "label=${LABEL}"
    echo "db_name=${DB_NAME}"
    echo "target_name=${TARGET_NAME}"
    echo "target_dir=${RMDB_DIR}"
    echo "build_dir=${BUILD_DIR}"
    echo "record_root=${RECORD_ROOT}"
    echo "host=${HOST}"
    echo "port=${PORT}"
    echo "scale=${SCALE}"
    echo "threads=${THREADS}"
    echo "transactions=${TRANSACTIONS}"
    echo "rw_ratio=${RW_RATIO}"
    echo "txn_probs=${TXN_PROBS}"
    echo "tpcc_dir=${TPCC_DIR}"
    echo "init_db=${INIT_DB}"
    echo "run_check=${RUN_CHECK}"
    echo "run_diagnose=${RUN_DIAGNOSE}"
    echo "skip_build=${SKIP_BUILD}"
    echo "skip_perf_record=${SKIP_PERF_RECORD}"
    echo "perf_record_seconds=${PERF_RECORD_SECONDS}"
    echo "callgrind_transactions=${CALLGRIND_TRANSACTIONS}"
    echo "heaptrack_transactions=${HEAPTRACK_TRANSACTIONS}"
    echo "warmup_transactions=${WARMUP_TRANSACTIONS}"
    echo "warmup_seconds=${WARMUP_SECONDS}"
    echo "measure_seconds=${MEASURE_SECONDS}"
    echo "timed_transactions_cap=${TIMED_TRANSACTIONS_CAP}"
    echo "timed_shutdown_grace=${TIMED_SHUTDOWN_GRACE}"
    echo "tpcc_timeout=${TPCC_TIMEOUT}"
    echo "keep_db_artifacts=${KEEP_DB_ARTIFACTS}"
    echo "memory_telemetry=${MEMORY_TELEMETRY}"
    echo "memory_cgroup_path=${MEMORY_CGROUP_PATH}"
  } >"${RESULT_DIR}/manifest.txt"
}

start_memory_telemetry() {
  local mode_name="$1"
  [[ "${MEMORY_TELEMETRY}" == "1" ]] || return 0
  local memory_dir="${RESULT_DIR}/memory"
  mkdir -p "${memory_dir}"
  MEMORY_TELEMETRY_FILE="${memory_dir}/${mode_name}.jsonl"
  local command=(python3 "${SCRIPT_DIR}/collect_memory_wall_telemetry.py"
    --pid "${SERVER_PID}"
    --db-dir "$(database_path)"
    --output "${MEMORY_TELEMETRY_FILE}"
    --interval-seconds 5)
  if [[ -n "${MEMORY_CGROUP_PATH}" ]]; then
    command+=(--cgroup-path "${MEMORY_CGROUP_PATH}")
  fi
  "${command[@]}" >"${memory_dir}/${mode_name}.collector.log" 2>&1 &
  MEMORY_TELEMETRY_PID=$!
}

stop_memory_telemetry() {
  [[ -n "${MEMORY_TELEMETRY_PID}" ]] || return 0
  if kill -0 "${MEMORY_TELEMETRY_PID}" 2>/dev/null; then
    kill -TERM "${MEMORY_TELEMETRY_PID}" 2>/dev/null || true
  fi
  wait "${MEMORY_TELEMETRY_PID}" 2>/dev/null || true
  MEMORY_TELEMETRY_PID=""
}

ensure_tpcc_tester() {
  if [[ ! -d "${TPCC_DIR}/.git" ]]; then
    log "cloning TPCC-Tester into ${TPCC_DIR}"
    rm -rf "${TPCC_DIR}"
    git clone --depth 1 https://github.com/db-camp/TPCC-Tester "${TPCC_DIR}" \
      >"${RESULT_DIR}/tpcc_clone.log" 2>&1
  fi
  log "building TPCC-Tester"
  cargo build --release --manifest-path "${TPCC_DIR}/Cargo.toml" \
    >"${RESULT_DIR}/tpcc_build.log" 2>&1
  TPCC_BIN="${TPCC_DIR}/target/release/tpcc-tester"
  [[ -x "${TPCC_BIN}" ]] || die "tpcc-tester binary not found at ${TPCC_BIN}"
}

build_rmdb() {
  [[ "${SKIP_BUILD}" == "1" ]] && return 0
  log "configuring RMDB into ${BUILD_DIR}"
  cmake -S "${RMDB_DIR}" -B "${RMDB_DIR}/${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O0 -g -fno-omit-frame-pointer" \
    >"${RESULT_DIR}/cmake_configure.log" 2>&1
  log "building RMDB"
  cmake --build "${RMDB_DIR}/${BUILD_DIR}" --target rmdb -j"$(nproc)" \
    >"${RESULT_DIR}/cmake_build.log" 2>&1
}

server_bin() {
  printf '%s\n' "${RMDB_DIR}/${BUILD_DIR}/bin/rmdb"
}

database_path() {
  printf '%s\n' "${RMDB_DIR}/${DB_NAME}"
}

reset_database_dir() {
  local db_path
  db_path="$(database_path)"
  if [[ -e "${db_path}" ]]; then
    log "removing existing database directory ${db_path}"
    rm -rf "${db_path}"
  fi
}

cleanup_database_artifacts() {
  local db_path
  db_path="$(database_path)"
  if [[ -e "${db_path}" ]]; then
    log "cleaning database directory ${db_path}"
    rm -rf "${db_path}"
  fi
}

port_listener_pids() {
  ss -ltnpH "( sport = :${PORT} )" 2>/dev/null \
    | grep -o 'pid=[0-9]\+' \
    | cut -d= -f2 \
    | sort -u
}

describe_port_listeners() {
  local found=0
  while read -r pid; do
    [[ -z "${pid}" ]] && continue
    found=1
    ps -p "${pid}" -o pid=,ppid=,cmd= || true
  done < <(port_listener_pids)
  if [[ "${found}" == "0" ]]; then
    echo "(none)"
  fi
}

ensure_port_available() {
  local listeners
  listeners="$(port_listener_pids || true)"
  [[ -z "${listeners}" ]] && return 0
  {
    echo "port ${HOST}:${PORT} is already in use before startup"
    echo
    echo "[ss]"
    ss -ltnp "( sport = :${PORT} )" || true
    echo
    echo "[ps]"
    describe_port_listeners
  } >"${RESULT_DIR}/port_conflict.txt"
  die "port ${HOST}:${PORT} is already in use; stop the existing listener first. See ${RESULT_DIR}/port_conflict.txt"
}

wait_for_server() {
  local timeout_seconds="${1:-20}"
  local retries=$((timeout_seconds * 4))
  while (( retries > 0 )); do
    if [[ -z "${SERVER_PID}" ]] || ! kill -0 "${SERVER_PID}" 2>/dev/null; then
      return 1
    fi
    if [[ -z "$(port_listener_pids)" ]]; then
      sleep 0.25
      retries=$((retries - 1))
      continue
    fi
    if python3 - <<PY
import socket
s = socket.socket()
s.settimeout(0.2)
try:
    s.connect(("${HOST}", int("${PORT}")))
except OSError:
    raise SystemExit(1)
else:
    s.close()
    raise SystemExit(0)
PY
    then
      return 0
    fi
    sleep 0.25
    retries=$((retries - 1))
  done
  return 1
}

disable_output_file() {
  python3 - "${HOST}" "${PORT}" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.create_connection((host, port), timeout=5.0) as sock:
    sock.settimeout(5.0)
    sock.sendall(b"set output_file off\0")
    sock.recv(8192)
PY
}

start_server() {
  local mode_name="$1"
  shift || true
  cleanup_server
  ensure_port_available
  SERVER_LOG="${RESULT_DIR}/server_${mode_name}.log"
  : >"${SERVER_LOG}"
  ln -sfn "$(basename "${SERVER_LOG}")" "${RESULT_DIR}/server.log"
  log "starting RMDB server (${mode_name})"
  (
    cd "${RMDB_DIR}"
    RMDB_PORT="${PORT}" "$@" "$(server_bin)" "${DB_NAME}"
  ) >"${SERVER_LOG}" 2>&1 &
  SERVER_PID=$!
  local start_timeout="${RMDB_SERVER_START_TIMEOUT_SECONDS:-20}"
  wait_for_server "${start_timeout}" || {
    {
      echo
      echo "[ss after failed startup]"
      ss -ltnp "( sport = :${PORT} )" || true
      echo
      echo "[ps after failed startup]"
      describe_port_listeners
    } >>"${SERVER_LOG}"
    die "RMDB server failed to listen on ${HOST}:${PORT}; see ${SERVER_LOG}"
  }
  log "disabling RMDB output.txt writes (${mode_name})"
  disable_output_file || die "failed to send set output_file off to RMDB server (${mode_name})"
  echo "${SERVER_PID}" >"${RESULT_DIR}/server.pid"
  start_memory_telemetry "${mode_name}"
}

run_tpcc() {
  local log_path="$1"
  shift
  (
    cd "${TPCC_DIR}"
    timeout "${TPCC_TIMEOUT}" "${TPCC_BIN}" --host "${HOST}" --port "${PORT}" -s "${SCALE}" "$@"
  ) >"${log_path}" 2>&1
}

run_tpcc_for_seconds() {
  local log_path="$1"
  local seconds="$2"
  shift 2
  local rc=0
  (
    cd "${TPCC_DIR}"
    timeout -s INT -k "${TIMED_SHUTDOWN_GRACE}" "${seconds}s" "${TPCC_BIN}" --host "${HOST}" --port "${PORT}" -s "${SCALE}" "$@"
  ) >"${log_path}" 2>&1 || rc=$?

  # GNU timeout exits 124 after sending SIGINT even when tpcc-tester catches it
  # and prints the final benchmark report. Treat that as a successful timed run.
  if [[ "${rc}" == "124" ]] && grep -q "TPC-C CONCURRENT BENCHMARK RESULTS" "${log_path}"; then
    return 0
  fi
  return "${rc}"
}

start_tpcc_background_group() {
  local log_path="$1"
  shift
  local pid_file="${log_path}.pid"
  TPCC_BG_PID=""
  TPCC_BG_PGID=""
  TPCC_BG_WRAPPER_PID=""
  rm -f "${pid_file}"
  (
    cd "${TPCC_DIR}"
    if command -v setsid >/dev/null 2>&1; then
      exec setsid sh -c 'printf "%s\n" "$$" > "$1"; shift; exec "$@"' \
        sh "${pid_file}" "${TPCC_BIN}" --host "${HOST}" --port "${PORT}" -s "${SCALE}" "$@"
    fi
    exec sh -c 'printf "%s\n" "$$" > "$1"; shift; exec "$@"' \
      sh "${pid_file}" "${TPCC_BIN}" --host "${HOST}" --port "${PORT}" -s "${SCALE}" "$@"
  ) >"${log_path}" 2>&1 &
  local wrapper_pid="$!"
  local client_pid=""
  for _ in $(seq 1 40); do
    if [[ -s "${pid_file}" ]]; then
      read -r client_pid <"${pid_file}" || true
      if [[ -n "${client_pid}" ]]; then
        break
      fi
    fi
    if ! kill -0 "${wrapper_pid}" 2>/dev/null; then
      break
    fi
    sleep 0.05
  done

  if [[ -z "${client_pid}" ]]; then
    client_pid="${wrapper_pid}"
  fi

  TPCC_BG_PID="${client_pid}"
  TPCC_BG_WRAPPER_PID="${wrapper_pid}"
  TPCC_BG_PGID="$(ps -p "${client_pid}" -o pgid= 2>/dev/null | tr -d '[:space:]' || true)"
  if [[ -z "${TPCC_BG_PGID}" && "${wrapper_pid}" != "${client_pid}" ]]; then
    TPCC_BG_PGID="$(ps -p "${wrapper_pid}" -o pgid= 2>/dev/null | tr -d '[:space:]' || true)"
  fi
  log "started TPCC background client pid=${TPCC_BG_PID} pgid=${TPCC_BG_PGID:-unknown}"
}

tpcc_pid_alive() {
  local pid="$1"
  local stat
  [[ -z "${pid}" ]] && return 1
  stat="$(ps -p "${pid}" -o stat= 2>/dev/null || true)"
  [[ -n "${stat}" && "${stat}" != Z* ]]
}

tpcc_process_alive() {
  local pid="$1"
  local pgid="${2:-}"
  local self_pgid
  self_pgid="$(ps -p "$$" -o pgid= 2>/dev/null | tr -d '[:space:]' || true)"
  if [[ -n "${pgid}" && "${pgid}" != "${self_pgid}" ]]; then
    while read -r stat; do
      [[ -z "${stat}" ]] && continue
      [[ "${stat}" == Z* ]] && continue
      return 0
    done < <(ps -g "${pgid}" -o stat= 2>/dev/null || true)
    return 1
  fi
  tpcc_pid_alive "${pid}"
}

signal_tpcc_process_group() {
  local signal="$1"
  local pid="$2"
  local pgid="${3:-}"
  local self_pgid
  self_pgid="$(ps -p "$$" -o pgid= 2>/dev/null | tr -d '[:space:]' || true)"
  if [[ -n "${pgid}" && "${pgid}" != "${self_pgid}" ]]; then
    kill "-${signal}" -- "-${pgid}" 2>/dev/null || true
  fi
  if [[ -n "${pid}" ]]; then
    kill "-${signal}" "${pid}" 2>/dev/null || true
  fi
  if [[ -n "${TPCC_BG_WRAPPER_PID}" && "${TPCC_BG_WRAPPER_PID}" != "${pid}" ]]; then
    kill "-${signal}" "${TPCC_BG_WRAPPER_PID}" 2>/dev/null || true
  fi
}

wait_tpcc_background_group() {
  local pid="$1"
  local pgid="$2"
  local loops="$3"
  for _ in $(seq 1 "${loops}"); do
    if ! tpcc_process_alive "${pid}" "${pgid}"; then
      wait "${TPCC_BG_WRAPPER_PID:-${pid}}" 2>/dev/null || true
      return 0
    fi
    sleep 0.25
  done
  return 1
}

describe_tpcc_background_group() {
  local pgid="${1:-}"
  if [[ -n "${pgid}" ]]; then
    ps -g "${pgid}" -o pid=,ppid=,pgid=,sid=,stat=,cmd= 2>/dev/null || true
  elif [[ -n "${TPCC_BG_PID}" ]]; then
    ps -p "${TPCC_BG_PID}" -o pid=,ppid=,pgid=,sid=,stat=,cmd= 2>/dev/null || true
  fi
}

stop_tpcc_background_group() {
  local label="$1"
  local grace_loops="${2:-12}"
  local pid="${TPCC_BG_PID}"
  local pgid="${TPCC_BG_PGID}"
  [[ -z "${pid}" ]] && return 0
  if ! tpcc_process_alive "${pid}" "${pgid}"; then
    wait "${TPCC_BG_WRAPPER_PID:-${pid}}" 2>/dev/null || true
    TPCC_BG_PID=""
    TPCC_BG_PGID=""
    TPCC_BG_WRAPPER_PID=""
    return 0
  fi

  signal_tpcc_process_group INT "${pid}" "${pgid}"
  if wait_tpcc_background_group "${pid}" "${pgid}" "${grace_loops}"; then
    TPCC_BG_PID=""
    TPCC_BG_PGID=""
    TPCC_BG_WRAPPER_PID=""
    return 0
  fi

  log "${label} did not exit after SIGINT; sending SIGTERM"
  signal_tpcc_process_group TERM "${pid}" "${pgid}"
  if wait_tpcc_background_group "${pid}" "${pgid}" 8; then
    TPCC_BG_PID=""
    TPCC_BG_PGID=""
    TPCC_BG_WRAPPER_PID=""
    return 0
  fi

  log "${label} did not exit after SIGTERM; sending SIGKILL"
  signal_tpcc_process_group KILL "${pid}" "${pgid}"
  if ! wait_tpcc_background_group "${pid}" "${pgid}" 4; then
    log "${label} still appears alive after SIGKILL:"
    describe_tpcc_background_group "${pgid}" >&2
  fi
  TPCC_BG_PID=""
  TPCC_BG_PGID=""
  TPCC_BG_WRAPPER_PID=""
}

maybe_append_txn_probs() {
  local -n cmd_ref=$1
  if [[ -n "${TXN_PROBS}" ]]; then
    cmd_ref+=(--txn-probs)
    IFS=' ' read -r -a probs <<<"${TXN_PROBS}"
    cmd_ref+=("${probs[@]}")
  fi
}

prepare_database() {
  local csv_dir="${RESULT_DIR}/tpcc_csv"
  mkdir -p "${csv_dir}"
  if [[ "${INIT_DB}" == "1" ]]; then
    reset_database_dir
  fi
  start_server "prepare"
  if [[ "${RUN_DIAGNOSE}" == "1" ]]; then
    log "running TPCC diagnose -> ${RESULT_DIR}/diagnose.log"
    run_tpcc "${RESULT_DIR}/diagnose.log" --diagnose \
      || die "TPCC diagnose failed; see ${RESULT_DIR}/diagnose.log"
  fi
  if [[ "${INIT_DB}" == "1" ]]; then
    log "running TPCC init -> ${RESULT_DIR}/init.log"
    local init_rc=0
    run_tpcc "${RESULT_DIR}/init.log" --init --csv-path "${csv_dir}" || init_rc=$?
    rm -rf "${csv_dir}"
    if [[ "${init_rc}" != "0" ]]; then
      die "TPCC init failed; see ${RESULT_DIR}/init.log"
    fi
  fi
  if [[ "${RUN_CHECK}" == "1" ]]; then
    log "running TPCC check -> ${RESULT_DIR}/check.log"
    run_tpcc "${RESULT_DIR}/check.log" --check \
      || die "TPCC check failed; see ${RESULT_DIR}/check.log"
  fi
  cleanup_server
}

run_benchmark_phase() {
  local phase_name="$1"
  local transactions="$2"
  local log_path="${RESULT_DIR}/${phase_name}.log"
  local benchmark_transactions="${transactions}"
  if [[ -n "${MEASURE_SECONDS}" ]]; then
    benchmark_transactions="${TIMED_TRANSACTIONS_CAP}"
  fi
  local cmd=(--benchmark --threads "${THREADS}" --transactions "${benchmark_transactions}" --rw-ratio "${RW_RATIO}")
  maybe_append_txn_probs cmd
  local server_started=0
  if [[ -n "${WARMUP_SECONDS}" ]]; then
    if [[ "${WARMUP_SECONDS}" != "0" ]]; then
      start_server "${phase_name}" env RMDB_ADVISOR_FINAL_BUILD=1
      server_started=1
      local warmup_log="${RESULT_DIR}/${phase_name}_warmup.log"
      local warmup_cmd=(--benchmark --threads "${THREADS}" --transactions "${TIMED_TRANSACTIONS_CAP}" --rw-ratio "${RW_RATIO}")
      maybe_append_txn_probs warmup_cmd
      log "running TPCC warmup (${phase_name}, ${WARMUP_SECONDS}s) -> ${warmup_log}"
      run_tpcc_for_seconds "${warmup_log}" "${WARMUP_SECONDS}" "${warmup_cmd[@]}" \
        || die "TPCC warmup failed; see ${warmup_log}"
      log "continuing RMDB server for TPCC benchmark (${phase_name})"
    fi
  elif [[ "${WARMUP_TRANSACTIONS}" != "0" ]]; then
    start_server "${phase_name}" env RMDB_ADVISOR_FINAL_BUILD=1
    server_started=1
    local warmup_log="${RESULT_DIR}/${phase_name}_warmup.log"
    local warmup_cmd=(--benchmark --threads "${THREADS}" --transactions "${WARMUP_TRANSACTIONS}" --rw-ratio "${RW_RATIO}")
    maybe_append_txn_probs warmup_cmd
    log "running TPCC warmup (${phase_name}) -> ${warmup_log}"
    run_tpcc "${warmup_log}" "${warmup_cmd[@]}" \
      || die "TPCC warmup failed; see ${warmup_log}"
    log "continuing RMDB server for TPCC benchmark (${phase_name})"
  fi
  if [[ "${server_started}" != "1" ]]; then
    start_server "${phase_name}"
  fi
  if [[ -n "${MEASURE_SECONDS}" ]]; then
    log "running TPCC benchmark (${phase_name}, ${MEASURE_SECONDS}s) -> ${log_path}"
    run_tpcc_for_seconds "${log_path}" "${MEASURE_SECONDS}" "${cmd[@]}" \
      || die "TPCC benchmark failed; see ${log_path}"
  else
    log "running TPCC benchmark (${phase_name}) -> ${log_path}"
    run_tpcc "${log_path}" "${cmd[@]}" \
      || die "TPCC benchmark failed; see ${log_path}"
  fi
  cleanup_server
}

run_perf_phase() {
  local perf_dir="${RESULT_DIR}/perf"
  mkdir -p "${perf_dir}"
  local bench_log="${perf_dir}/benchmark.log"
  local perf_stat="${perf_dir}/perf_stat.csv"
  local perf_data="${perf_dir}/perf.data"
  local perf_script_out="${perf_dir}/perf.script"
  local perf_folded="${perf_dir}/perf.folded"
  local perf_svg="${perf_dir}/perf.svg"
  local perf_client_stop_grace_loops=60
  local bench_cmd=(--benchmark --threads "${THREADS}" --transactions "${TRANSACTIONS}" --rw-ratio "${RW_RATIO}")
  maybe_append_txn_probs bench_cmd

  start_server "perf" env RMDB_ADVISOR_FINAL_BUILD=1
  local warmup_cmd=(--benchmark --threads "${THREADS}")
  if [[ -n "${WARMUP_SECONDS}" && "${WARMUP_SECONDS}" != "0" ]]; then
    warmup_cmd+=(--transactions "${TIMED_TRANSACTIONS_CAP}")
  else
    warmup_cmd+=(--transactions "${WARMUP_TRANSACTIONS}")
  fi
  warmup_cmd+=(--rw-ratio "${RW_RATIO}")
  maybe_append_txn_probs warmup_cmd
  if [[ -n "${WARMUP_SECONDS}" && "${WARMUP_SECONDS}" != "0" ]]; then
    run_tpcc_for_seconds "${perf_dir}/warmup.log" "${WARMUP_SECONDS}" "${warmup_cmd[@]}" || true
  else
    run_tpcc "${perf_dir}/warmup.log" "${warmup_cmd[@]}" || true
  fi
  log "continuing RMDB server for perf measurement"

  log "running perf stat"
  start_tpcc_background_group "${bench_log}" "${bench_cmd[@]}"
  sleep 1
  perf stat -x, -o "${perf_stat}" \
    -e task-clock,context-switches,cpu-migrations,page-faults \
    -p "${SERVER_PID}" -- sleep "${PERF_RECORD_SECONDS}" \
    >"${perf_dir}/perf_stat.stdout" 2>"${perf_dir}/perf_stat.stderr" || true
  stop_tpcc_background_group "TPCC perf stat client" "${perf_client_stop_grace_loops}"

  if [[ "${SKIP_PERF_RECORD}" == "0" ]]; then
    log "running perf record"
    start_tpcc_background_group "${perf_dir}/benchmark_record.log" "${bench_cmd[@]}"
    sleep 1
    perf record -F 99 -g -e cpu-clock -o "${perf_data}" -p "${SERVER_PID}" -- sleep "${PERF_RECORD_SECONDS}" \
      >"${perf_dir}/perf_record.stdout" 2>"${perf_dir}/perf_record.stderr" || true
    stop_tpcc_background_group "TPCC perf record client" "${perf_client_stop_grace_loops}"

    if [[ -s "${perf_data}" ]] && command -v stackcollapse-perf.pl >/dev/null 2>&1 && command -v flamegraph.pl >/dev/null 2>&1; then
      perf script -i "${perf_data}" >"${perf_script_out}" 2>"${perf_dir}/perf_script.stderr" || true
      stackcollapse-perf.pl "${perf_script_out}" >"${perf_folded}" 2>"${perf_dir}/stackcollapse.stderr" || true
      flamegraph.pl "${perf_folded}" >"${perf_svg}" 2>"${perf_dir}/flamegraph.stderr" || true
    fi
  fi
  cleanup_server
}

run_callgrind_phase() {
  local out_dir="${RESULT_DIR}/callgrind"
  mkdir -p "${out_dir}"
  if ! command -v valgrind >/dev/null 2>&1; then
    log "skipping callgrind: valgrind not found"
    echo "skipped: valgrind not found" >"${out_dir}/skipped.txt"
    return 0
  fi
  local callgrind_out="${out_dir}/callgrind.out"
  start_server "callgrind" valgrind --tool=callgrind --callgrind-out-file="${callgrind_out}"
  local callgrind_transactions="${CALLGRIND_TRANSACTIONS}"
  if [[ -n "${MEASURE_SECONDS}" ]]; then
    callgrind_transactions="${TIMED_TRANSACTIONS_CAP}"
  fi
  local bench_cmd=(--benchmark --threads "${THREADS}" --transactions "${callgrind_transactions}" --rw-ratio "${RW_RATIO}")
  maybe_append_txn_probs bench_cmd
  if [[ -n "${MEASURE_SECONDS}" ]]; then
    log "running TPCC benchmark (callgrind, ${MEASURE_SECONDS}s) -> ${out_dir}/benchmark.log"
    run_tpcc_for_seconds "${out_dir}/benchmark.log" "${MEASURE_SECONDS}" "${bench_cmd[@]}"
  else
    run_tpcc "${out_dir}/benchmark.log" "${bench_cmd[@]}"
  fi
  cleanup_server
  if [[ -s "${callgrind_out}" ]]; then
    callgrind_annotate --inclusive=yes "${callgrind_out}" >"${out_dir}/callgrind_annotate.txt" 2>"${out_dir}/callgrind_annotate.stderr" || true
  fi
}

run_heaptrack_phase() {
  local out_dir="${RESULT_DIR}/heaptrack"
  mkdir -p "${out_dir}"
  if [[ ! -x "${SYSTEM_HEAPTRACK}" ]]; then
    log "skipping heaptrack: heaptrack not found"
    echo "skipped: heaptrack not found" >"${out_dir}/skipped.txt"
    return 0
  fi
  local heaptrack_out="${out_dir}/heaptrack.data.gz"
  start_server "heaptrack" "${SYSTEM_HEAPTRACK}" -o "${heaptrack_out}"
  local bench_cmd=(--benchmark --threads "${THREADS}" --transactions "${HEAPTRACK_TRANSACTIONS}" --rw-ratio "${RW_RATIO}")
  maybe_append_txn_probs bench_cmd
  run_tpcc "${out_dir}/benchmark.log" "${bench_cmd[@]}"
  cleanup_server
  local heaptrack_result="${heaptrack_out}"
  if [[ ! -s "${heaptrack_result}" && -s "${heaptrack_out}.zst" ]]; then
    heaptrack_result="${heaptrack_out}.zst"
  fi
  if [[ -s "${heaptrack_result}" ]] && [[ -x "${SYSTEM_HEAPTRACK_PRINT}" ]]; then
    "${SYSTEM_HEAPTRACK_PRINT}" "${heaptrack_result}" >"${out_dir}/heaptrack_print.txt" 2>"${out_dir}/heaptrack_print.stderr" || true
  fi
}

generate_summary() {
  if [[ -d "${RESULT_DIR}/memory" ]]; then
    while read -r telemetry_file; do
      [[ -s "${telemetry_file}" ]] || continue
      local analysis_file="${telemetry_file%.jsonl}.analysis.md"
      python3 "${SCRIPT_DIR}/analyze_memory_wall.py" "${telemetry_file}" >"${analysis_file}"
    done < <(find "${RESULT_DIR}/memory" -maxdepth 1 -type f -name '*.jsonl' | sort)
  fi
  python3 "${SCRIPT_DIR}/summarize_perf_run.py" "${RESULT_DIR}" >"${RESULT_DIR}/summary.md"
}

run_detached() {
  local status_file="${RESULT_DIR}/detach_status.txt"
  local script_copy="${RESULT_DIR}/run_detached_inner.sh"
  cat >"${script_copy}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
trap '' HUP
$(printf 'cd %q\n' "${WORK_ROOT}")
$(printf 'export PATH=%q:"$PATH"\n' "${LOCAL_BIN}")
  $(printf 'bash %q --mode %q --label %q --db-name %q --build-dir %q --target-dir %q --record-root %q --host %q --port %q --scale %q --threads %q --transactions %q --rw-ratio %q --perf-record-seconds %q --callgrind-transactions %q --heaptrack-transactions %q --warmup-transactions %q --timed-shutdown-grace %q %s %s %s %s %s %s %s %s %s %s\n' \
  "${SCRIPT_DIR}/run_workflow.sh" "${MODE}" "${LABEL}" "${DB_NAME}" "${BUILD_DIR}" "${RMDB_DIR}" "${RECORD_ROOT}" "${HOST}" "${PORT}" "${SCALE}" "${THREADS}" "${TRANSACTIONS}" "${RW_RATIO}" "${PERF_RECORD_SECONDS}" "${CALLGRIND_TRANSACTIONS}" "${HEAPTRACK_TRANSACTIONS}" "${WARMUP_TRANSACTIONS}" \
  "${TIMED_SHUTDOWN_GRACE}" \
  "$( [[ -n "${WARMUP_SECONDS}" ]] && echo "--warmup-seconds ${WARMUP_SECONDS}" )" \
  "$( [[ -n "${MEASURE_SECONDS}" ]] && echo "--measure-seconds ${MEASURE_SECONDS}" )" \
  "$( [[ "${INIT_DB}" == "1" ]] && echo --init-db )" \
  "$( [[ "${RUN_CHECK}" == "1" ]] && echo --check )" \
  "$( [[ "${RUN_DIAGNOSE}" == "1" ]] && echo --diagnose )" \
  "$( [[ "${SKIP_BUILD}" == "1" ]] && echo --skip-build )" \
  "$( [[ "${SKIP_PERF_RECORD}" == "1" ]] && echo --skip-perf-record )" \
  "$( [[ "${KEEP_DB_ARTIFACTS}" == "1" ]] && echo --keep-db-artifacts )" \
  "$( [[ "${MEMORY_TELEMETRY}" == "1" ]] && echo --memory-telemetry )" \
  "$( [[ -n "${MEMORY_CGROUP_PATH}" ]] && echo "--memory-cgroup-path ${MEMORY_CGROUP_PATH}" )" )
EOF
  chmod +x "${script_copy}"
  nohup "${script_copy}" >"${RESULT_DIR}/detach_stdout.log" 2>"${RESULT_DIR}/detach_stderr.log" &
  local detached_pid=$!
  {
    echo "pid=${detached_pid}"
    echo "result_dir=${RESULT_DIR}"
  } >"${status_file}"
  echo "${status_file}"
}

record_system_info
record_manifest
record_tool_status

if [[ "${DETACH}" == "1" ]]; then
  run_detached
  exit 0
fi

if [[ "${MODE}" == "tools" ]]; then
  generate_summary
  log "results written to ${RESULT_DIR}"
  exit 0
fi

ensure_tpcc_tester
build_rmdb
prepare_database

case "${MODE}" in
  benchmark)
    run_benchmark_phase "benchmark" "${TRANSACTIONS}"
    ;;
  steady-state)
    run_benchmark_phase "steady_state" "${TRANSACTIONS}"
    ;;
  perf)
    run_perf_phase
    ;;
  callgrind)
    run_callgrind_phase
    ;;
  heaptrack)
    run_heaptrack_phase
    ;;
  benchmark_perf)
    run_benchmark_phase "benchmark" "${TRANSACTIONS}"
    run_perf_phase
    ;;
  all)
    run_benchmark_phase "benchmark" "${TRANSACTIONS}"
    run_perf_phase
    run_callgrind_phase
    run_heaptrack_phase
    ;;
  *)
    die "unsupported mode: ${MODE}"
    ;;
esac

generate_summary
log "results written to ${RESULT_DIR}"
