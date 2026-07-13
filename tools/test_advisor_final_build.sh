#!/usr/bin/env bash
# Smoke test for advisor final_build: verify implicit non-unique indexes are created.
set -euo pipefail

RMDB_DIR="/home/mochen/db_contest_workspace/RMDB"
BUILD_DIR="${RMDB_DIR}/build-perf"
SERVER_BIN="${BUILD_DIR}/bin/rmdb"
DB_NAME="tpcc_sf50"
PORT="18766"
SERVER_LOG="/tmp/advisor_smoke_server.log"

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        echo "Shutting down server..."
        kill -INT "${SERVER_PID}" 2>/dev/null || true
        sleep 2
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "=== Step 1: Starting RMDB with RMDB_ADVISOR_FINAL_BUILD=1 ==="
cd "${RMDB_DIR}"
RMDB_PORT="${PORT}" RMDB_ADVISOR_FINAL_BUILD=1 "${SERVER_BIN}" "${DB_NAME}" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

for i in $(seq 1 30); do
    if ss -ltnp 2>/dev/null | grep -q ":${PORT} "; then
        echo "Server ready after ${i}s"
        break
    fi
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
        echo "Server died!"
        cat "${SERVER_LOG}"
        exit 1
    fi
    sleep 1
done

echo "=== Step 2: Sending SQL queries to trigger advisor observations ==="
python3 -c "
import socket, time

def sql(query):
    s = socket.socket()
    s.settimeout(10)
    s.connect(('127.0.0.1', ${PORT}))
    s.sendall((query + '\n').encode())
    time.sleep(0.3)
    data = s.recv(4096)
    s.close()
    print(f'  -> {data.decode(errors=\"replace\")[:100].strip()}')
    return data

# Trigger StockLevel-like scan (stock with s_quantity range)
sql('select count(*) from stock where s_w_id = 1 and s_quantity < 10;')
sql('select count(*) from stock where s_w_id = 2 and s_quantity < 15;')
sql('select count(*) from stock where s_w_id = 3 and s_quantity < 20;')

# Trigger Payment-like lookup (customer by last name)
sql('select c_id from customer where c_w_id = 1 and c_d_id = 1 and c_last = \"CALLYPRI\" order by c_first;')
sql('select c_id from customer where c_w_id = 2 and c_d_id = 1 and c_last = \"BARBARBAR\" order by c_first;')
sql('select c_id from customer where c_w_id = 3 and c_d_id = 1 and c_last = \"ABLEABLE\" order by c_first;')

# Trigger OrderStatus-like query
sql('select o_id from orders where o_w_id = 1 and o_d_id = 1 and o_c_id = 1 order by o_id desc limit 1;')
"

echo "=== Step 3: Shutting down server ==="
kill -INT "${SERVER_PID}" 2>/dev/null || true
sleep 3
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""

echo ""
echo "=== Step 4: Advisor final build results ==="
grep -E '\[advisor\].*final-build|\[advisor\].*built|\[advisor\].*satisfied|\[advisor\].*reject|\[advisor\].*build-error' "${SERVER_LOG}" | tail -20

echo ""
echo "=== Step 5: Hidden non-unique index files ==="
find "${RMDB_DIR}/${DB_NAME}" -name '*__internal_non_unique*' -type f 2>/dev/null | head -10
NUM_FILES=$(find "${RMDB_DIR}/${DB_NAME}" -name '*__internal_non_unique*' -type f 2>/dev/null | wc -l)
echo "Found ${NUM_FILES} hidden index file(s)"

echo ""
if [[ "${NUM_FILES}" -gt 0 ]]; then
    echo "=== SUCCESS: ${NUM_FILES} hidden index(es) were created ==="
else
    echo "=== FAILURE: No hidden indexes were created ==="
    echo "--- checking server log for clues ---"
    grep '\[advisor\]' "${SERVER_LOG}" | tail -10
    exit 1
fi
