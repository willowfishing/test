"""Smoke test for advisor final_build: verify implicit non-unique indexes are created.

Usage: python3 tools/test_advisor_final_build.py

Requires RMDB built in build-perf/, tpcc-tester binary, and an initialized database.
"""

import subprocess, socket, time, sys, os, re, signal

RMDB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BUILD_DIR = os.path.join(RMDB_DIR, "build-perf")
SERVER_BIN = os.path.join(BUILD_DIR, "bin", "rmdb")
TPCC_BIN = os.path.abspath(os.path.join(RMDB_DIR, "..", "TPCC-Tester", "target", "release", "tpcc-tester"))
DB_NAME = "tpcc_advisor_test"
DB_PATH = os.path.join(RMDB_DIR, DB_NAME)
PORT = 18767
SERVER_LOG = "/tmp/advisor_smoke_server.log"


def sql(query):
    s = socket.socket()
    s.settimeout(10)
    s.connect(("127.0.0.1", PORT))
    s.sendall(query.encode("utf-8") + b"\0")
    time.sleep(0.5)
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    s.close()
    return data.decode(errors="replace")


def start_server():
    env = os.environ.copy()
    env["RMDB_ADVISOR_FINAL_BUILD"] = "1"
    env["RMDB_PORT"] = str(PORT)
    proc = subprocess.Popen(
        [SERVER_BIN, DB_PATH],
        cwd=RMDB_DIR,
        env=env,
        stdout=open(SERVER_LOG, "w"),
        stderr=subprocess.STDOUT,
    )
    for i in range(30):
        if os.path.exists("/proc/" + str(proc.pid)):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(1)
            try:
                s.connect(("127.0.0.1", PORT))
                s.close()
                print(f"Server ready after {i+1}s")
                return proc
            except OSError:
                s.close()
        time.sleep(1)
    proc.kill()
    raise RuntimeError("Server failed to start in 30s")


def init_db():
    print("Initializing database (scale=1)...")
    proc = subprocess.run(
        [TPCC_BIN, "--init", "--csv-path", "/tmp/advisor_test_csv", "-s", "1"],
        capture_output=True, text=True, timeout=180,
    )
    if proc.returncode != 0:
        print("Init failed:", proc.stderr[-500:])
        return False
    return True


def main():
    # Clean old db
    if os.path.exists(DB_PATH):
        subprocess.run(["rm", "-rf", DB_PATH])

    # Init database with tpcc-tester (needs server running)
    print("=== Step 1: Starting server ===")
    proc = start_server()

    print("=== Step 2: Initializing database ===")
    init_result = subprocess.run(
        [TPCC_BIN, "--init", "--csv-path", "/tmp/advisor_test_csv", "-s", "1",
         "--port", str(PORT)],
        capture_output=True, text=True, timeout=180,
    )
    if init_result.returncode != 0:
        print("Init failed:", init_result.stderr[-500:])
        proc.kill()
        sys.exit(1)
    print("Init OK")

    print("=== Step 3: Sending queries to trigger advisor ===")
    queries = [
        "select count(*) from stock where s_w_id = 1 and s_quantity < 10;",
        "select count(*) from stock where s_w_id = 1 and s_quantity < 15;",
        "select count(*) from stock where s_w_id = 1 and s_quantity < 20;",
        "select c_id from customer where c_w_id = 1 and c_d_id = 1 and c_last = 'CALLYPRI' order by c_first;",
        "select c_id from customer where c_w_id = 1 and c_d_id = 1 and c_last = 'BARBARBAR' order by c_first;",
        "select c_id from customer where c_w_id = 1 and c_d_id = 1 and c_last = 'ABLEABLE' order by c_first;",
        "select o_id from orders where o_w_id = 1 and o_d_id = 1 and o_c_id = 1 order by o_id desc limit 1;",
    ]
    for q in queries:
        result = sql(q)
        if "failure" in result.lower():
            print(f"  WARN: query failed: {q[:60]}")
        else:
            print(f"  OK: {result[:80].strip()}")

    print("=== Step 4: Shutting down server ===")
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
    print("Server stopped")

    print("=== Step 5: Checking advisor final build results ===")
    with open(SERVER_LOG) as f:
        log = f.read()

    # Show advisor summaries
    for line in log.split("\n"):
        if "[advisor]" in line and ("final" in line.lower() or "built" in line.lower()):
            print(f"  {line.strip()}")

    # Check for index files
    index_files = []
    for root, dirs, files in os.walk(DB_PATH):
        for f in files:
            if "__internal_non_unique" in f:
                index_files.append(os.path.join(root, f))

    print(f"\nHidden non-unique index files: {len(index_files)}")
    for f in index_files:
        print(f"  {os.path.basename(f)}")

    if index_files:
        print("\n=== SUCCESS ===")
    else:
        print("\n=== FAILURE: No hidden indexes created ===")
        sys.exit(1)


if __name__ == "__main__":
    main()
