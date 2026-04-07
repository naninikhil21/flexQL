#!/usr/bin/env python3
import argparse
import json
import re
import socket
import subprocess
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Dict, List, Tuple

ROOT = Path(__file__).resolve().parent
HOST = "127.0.0.1"
PORT = 9000
REPORT_DIR = ROOT / "validation_reports"


@dataclass
class SqlResult:
    ok: bool
    rows: List[List[Tuple[str, str]]]
    error: str = ""


def parse_row_payload(payload: str) -> List[Tuple[str, str]]:
    first_space = payload.find(" ")
    if first_space <= 0:
        return [("row", payload)]

    try:
        col_count = int(payload[:first_space])
    except ValueError:
        return [("row", payload)]

    pos = first_space + 1
    out: List[Tuple[str, str]] = []

    for _ in range(col_count):
        colon = payload.find(":", pos)
        if colon <= pos:
            return [("row", payload)]
        name_len = int(payload[pos:colon])
        name_start = colon + 1
        name_end = name_start + name_len
        if name_end > len(payload):
            return [("row", payload)]
        name = payload[name_start:name_end]

        pos = name_end
        colon = payload.find(":", pos)
        if colon <= pos:
            return [("row", payload)]
        val_len = int(payload[pos:colon])
        val_start = colon + 1
        val_end = val_start + val_len
        if val_end > len(payload):
            return [("row", payload)]
        value = payload[val_start:val_end]

        out.append((name, value))
        pos = val_end

    return out


def exec_sql(sock: socket.socket, sql: str) -> SqlResult:
    sock.sendall(sql.encode("utf-8"))

    buf = ""
    rows: List[List[Tuple[str, str]]] = []
    err_text = ""

    while True:
        data = sock.recv(65536)
        if not data:
            return SqlResult(False, rows, "connection closed before END")
        buf += data.decode("utf-8", errors="replace")

        while True:
            nl = buf.find("\n")
            if nl < 0:
                break
            line = buf[:nl]
            buf = buf[nl + 1 :]

            if line == "END":
                return SqlResult(err_text == "", rows, err_text)

            if line.startswith("ERROR:"):
                err_text = line
                continue

            if line.startswith("ROW "):
                rows.append(parse_row_payload(line[4:]))


def wait_port_ready(host: str, port: int, timeout_sec: float = 8.0) -> bool:
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def run_cmd(cmd: List[str], cwd: Path) -> None:
    p = subprocess.run(cmd, cwd=cwd, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(cmd)}")


def run_cmd_capture(cmd: List[str], cwd: Path) -> str:
    p = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(
            f"command failed: {' '.join(cmd)}\nSTDOUT:\n{p.stdout}\nSTDERR:\n{p.stderr}"
        )
    return p.stdout


def run_benchmark_with_retry(rows: int, attempts: int = 3) -> str:
    last_exc: Exception | None = None
    for attempt in range(1, attempts + 1):
        try:
            return run_cmd_capture(["./benchmark", str(rows)], ROOT)
        except RuntimeError as exc:
            msg = str(exc)
            transient = ("Cannot open FlexQL" in msg) or ("read failed" in msg)
            if not transient or attempt == attempts:
                last_exc = exc
                break
            time.sleep(0.8)

    if last_exc is None:
        raise RuntimeError("benchmark failed with unknown error")
    raise last_exc


def parse_elapsed_ms(benchmark_output: str) -> int:
    m = re.search(r"^Elapsed:\s+(\d+)\s+ms$", benchmark_output, flags=re.MULTILINE)
    if not m:
        raise RuntimeError("Unable to parse benchmark elapsed time")
    return int(m.group(1))


def parse_throughput(benchmark_output: str) -> int:
    m = re.search(r"^Throughput:\s+(\d+)\s+rows/sec$", benchmark_output, flags=re.MULTILINE)
    if not m:
        raise RuntimeError("Unable to parse benchmark throughput")
    return int(m.group(1))


def as_dict_rows(rows: List[List[Tuple[str, str]]]) -> List[Dict[str, str]]:
    out: List[Dict[str, str]] = []
    for row in rows:
        obj: Dict[str, str] = {}
        for k, v in row:
            obj[k] = v
        out.append(obj)
    return out


def build_clause_tests(rows_target: int) -> List[Dict[str, object]]:
    bounded_hi = min(rows_target, 15000)
    bounded_count = max(0, bounded_hi - 5000 + 1)

    supported_tests: List[Dict[str, object]] = [
        {
            "label": "COUNT(*)",
            "query": "SELECT COUNT(*) AS CNT FROM BIG_USERS;",
            "expect": f"CNT={rows_target}",
            "should_succeed": True,
            "check": lambda rows: len(rows) == 1 and int(rows[0]["CNT"]) == rows_target,
        },
        {
            "label": "WHERE with AND range",
            "query": "SELECT COUNT(*) AS C FROM BIG_USERS WHERE ID >= 5000 AND ID <= 15000;",
            "expect": f"C={bounded_count}",
            "should_succeed": True,
            "check": lambda rows: len(rows) == 1 and int(rows[0]["C"]) == bounded_count,
        },
        {
            "label": "WHERE with OR chain",
            "query": "SELECT COUNT(*) AS C FROM BIG_USERS WHERE ID = 1 OR ID = 2 OR ID = 3;",
            "expect": "C=3",
            "should_succeed": True,
            "check": lambda rows: len(rows) == 1 and int(rows[0]["C"]) == 3,
        },
        {
            "label": "Projection query",
            "query": "SELECT ID, NAME FROM BIG_USERS WHERE ID <= 3;",
            "expect": "IDs 1,2,3",
            "should_succeed": True,
            "check": lambda rows: [int(r["ID"]) for r in rows] == [1, 2, 3],
        },
        {
            "label": "Float literal predicate",
            "query": "SELECT COUNT(*) AS C FROM BIG_USERS WHERE BALANCE >= 1000.0;",
            "expect": f"C={rows_target}",
            "should_succeed": True,
            "check": lambda rows: len(rows) == 1 and int(rows[0]["C"]) == rows_target,
        },
        {
            "label": "DROP TABLE IF EXISTS",
            "query": "DROP TABLE IF EXISTS CLAUSE_TMP;",
            "expect": "OK",
            "should_succeed": True,
            "check": lambda rows: True,
        },
        {
            "label": "CREATE TABLE",
            "query": "CREATE TABLE CLAUSE_TMP(ID DECIMAL, V DECIMAL);",
            "expect": "OK",
            "should_succeed": True,
            "check": lambda rows: True,
        },
        {
            "label": "INSERT multi-row",
            "query": "INSERT INTO CLAUSE_TMP VALUES (1, 10), (2, 20), (3, 30);",
            "expect": "OK",
            "should_succeed": True,
            "check": lambda rows: True,
        },
        {
            "label": "JOIN",
            "query": (
                "SELECT C.ID, C.V, B.NAME "
                "FROM CLAUSE_TMP C "
                "INNER JOIN BIG_USERS B ON B.ID = C.ID;"
            ),
            "expect": "3 joined rows",
            "should_succeed": True,
            "check": lambda rows: len(rows) == 3 and sorted(int(r["ID"]) for r in rows) == [1, 2, 3],
        },
        {
            "label": "Duplicate key reject",
            "query": "INSERT INTO CLAUSE_TMP VALUES (1, 999);",
            "expect": "duplicate primary key error",
            "should_succeed": False,
            "error_contains": "duplicate primary key",
        },
        {
            "label": "DROP TABLE cleanup",
            "query": "DROP TABLE IF EXISTS CLAUSE_TMP;",
            "expect": "OK",
            "should_succeed": True,
            "check": lambda rows: True,
        },
    ]

    unsupported_clause_tests: List[Dict[str, object]] = [
        {
            "label": "ORDER BY unsupported",
            "query": "SELECT ID FROM BIG_USERS ORDER BY ID DESC;",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "GROUP BY unsupported",
            "query": "SELECT EXPIRES_AT, COUNT(*) AS C FROM BIG_USERS GROUP BY EXPIRES_AT;",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "HAVING unsupported",
            "query": "SELECT EXPIRES_AT, COUNT(*) AS C FROM BIG_USERS HAVING COUNT(*) > 0;",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "DISTINCT unsupported",
            "query": "SELECT DISTINCT ID FROM BIG_USERS;",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "LIKE unsupported",
            "query": "SELECT COUNT(*) AS C FROM BIG_USERS WHERE EMAIL LIKE 'user1%@mail.com';",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "IN unsupported",
            "query": "SELECT ID FROM BIG_USERS WHERE ID IN (1,2,3);",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "BETWEEN unsupported",
            "query": "SELECT ID FROM BIG_USERS WHERE ID BETWEEN 1 AND 3;",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "LIMIT/OFFSET unsupported",
            "query": "SELECT ID FROM BIG_USERS LIMIT 3 OFFSET 2;",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "Subquery unsupported",
            "query": "SELECT ID FROM BIG_USERS WHERE ID IN (SELECT ID FROM BIG_USERS WHERE ID <= 3);",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "IS NOT NULL unsupported",
            "query": "SELECT COUNT(*) AS C FROM BIG_USERS WHERE NAME IS NOT NULL;",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "CASE unsupported",
            "query": "SELECT SUM(CASE WHEN BALANCE >= 1000 THEN 1 ELSE 0 END) AS C FROM BIG_USERS;",
            "expect": "query should fail",
            "should_succeed": False,
        },
        {
            "label": "MIN/MAX unsupported",
            "query": "SELECT MIN(ID) AS MIN_ID, MAX(ID) AS MAX_ID FROM BIG_USERS;",
            "expect": "query should fail",
            "should_succeed": False,
        },
    ]

    return supported_tests + unsupported_clause_tests


def write_report(report_path: Path, report: Dict[str, object]) -> None:
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")


def validate_clause_tests(rows_target: int, report: Dict[str, object]) -> None:
    tests = build_clause_tests(rows_target)

    with socket.create_connection((HOST, PORT), timeout=10) as sock:
        for t in tests:
            label = str(t["label"])
            query = str(t["query"])
            expect = str(t["expect"])
            should_succeed = bool(t.get("should_succeed", True))
            err_contains = str(t.get("error_contains", ""))
            check: Callable[[List[Dict[str, str]]], bool] = t.get("check", lambda _: True)  # type: ignore[assignment]

            res = exec_sql(sock, query)
            rows_dict = as_dict_rows(res.rows)

            entry = {
                "label": label,
                "query": query,
                "expected": expect,
                "ok": res.ok,
                "error": res.error,
                "row_count": len(rows_dict),
                "rows": rows_dict,
            }

            if not res.ok:
                if not should_succeed:
                    passed = (not err_contains) or (err_contains.lower() in res.error.lower())
                    entry["passed"] = passed
                    report["tests"].append(entry)
                    if not passed:
                        raise RuntimeError(f"{label} failed: expected error containing '{err_contains}', got '{res.error}'")
                    print(f"[PASS] {label} (expected failure)")
                    continue

                report["tests"].append(entry)
                raise RuntimeError(f"{label} failed: {res.error}")

            if not should_succeed:
                entry["passed"] = False
                report["tests"].append(entry)
                raise RuntimeError(f"{label} expected failure but succeeded")

            passed = check(rows_dict)
            entry["passed"] = passed
            report["tests"].append(entry)

            if not passed:
                raise RuntimeError(f"{label} expectation failed")

            print(f"[PASS] {label}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Large-scale SQL clause validation for FlexQL")
    parser.add_argument("--rows", type=int, default=1_000_000, help="Rows to insert via benchmark")
    parser.add_argument("--skip-build", action="store_true", help="Skip rebuild steps")
    parser.add_argument("--report", type=str, default="", help="Optional report JSON path")
    args = parser.parse_args()

    if args.rows < 20000:
        raise SystemExit("--rows must be >= 20000 for full clause validation set")

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    report_path = Path(args.report) if args.report else REPORT_DIR / f"scale_sql_clause_report_{stamp}.json"

    report: Dict[str, object] = {
        "timestamp": stamp,
        "rows_target": args.rows,
        "benchmark": {},
        "tests": [],
        "status": "running",
    }

    try:
        if not args.skip_build:
            print("[1/5] Building optimized server...")
            run_cmd(["cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"], ROOT)
            run_cmd(["cmake", "--build", "build", "--target", "flexql_server", "-j"], ROOT)

            print("[2/5] Building benchmark client...")
            run_cmd(["g++", "flexql.cpp", "benchmark_flexql.cpp", "-O2", "-o", "benchmark"], ROOT)

        print("[3/5] Starting server...")
        subprocess.run(["pkill", "-f", "flexql_server"], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server = subprocess.Popen(["./build/flexql_server", "9000"], cwd=ROOT)

        try:
            if not wait_port_ready(HOST, PORT, timeout_sec=8.0):
                raise RuntimeError("Server did not become ready on port 9000")

            print(f"[4/5] Running benchmark insert for {args.rows} rows...")
            bench_out = run_benchmark_with_retry(args.rows)
            elapsed = parse_elapsed_ms(bench_out)
            throughput = parse_throughput(bench_out)
            print(f"[INFO] Benchmark elapsed: {elapsed} ms")
            print(f"[INFO] Benchmark throughput: {throughput} rows/sec")

            report["benchmark"] = {
                "elapsed_ms": elapsed,
                "throughput_rows_per_sec": throughput,
            }

            print("[5/5] Validating SQL clauses on large dataset...")
            validate_clause_tests(args.rows, report)

            report["status"] = "pass"
            write_report(report_path, report)
            print(f"[PASS] Large-scale SQL clause validation complete")
            print(f"[INFO] Report saved to: {report_path}")
            return 0
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)

    except Exception as exc:
        report["status"] = "fail"
        report["error"] = str(exc)
        write_report(report_path, report)
        print(f"[FAIL] {exc}")
        print(f"[INFO] Report saved to: {report_path}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
