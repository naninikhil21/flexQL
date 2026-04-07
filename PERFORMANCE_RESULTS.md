# FlexQL — Performance Results for Large Datasets

## Test Environment

| Parameter        | Value                                               |
|------------------|-----------------------------------------------------|
| OS               | Linux (x86-64)                                      |
| Build Type       | Release (`-O3`, `-DNDEBUG`)                         |
| Server           | `./build/flexql_server` (default config)            |
| Cache Size       | 64 entries (default `FLEXQL_CACHE_SIZE`)            |
| WAL              | Custom binary WAL (v4 format)                       |

---

## 1. Insertion Benchmark — `./benchmark <N>`

### 1,000,000 Row Insertion

```
[PASS] DROP TABLE IF EXISTS BIG_USERS  (15 ms)
[PASS] CREATE TABLE BIG_USERS         (182 ms)

Starting insertion benchmark for 1,000,000 rows...
Progress: 1000000/1000000
[PASS] INSERT benchmark complete

Rows inserted : 1,000,000
Elapsed       : 213 ms
Throughput    : 4,694,835 rows/sec
```

### Throughput at Different Dataset Sizes

| Rows Inserted | Elapsed (ms) | Throughput (rows/sec) |
|---------------|--------------|----------------------|
| 20,000        | ~12–14 ms    | ~1,430,000–1,670,000 |
| 1,000,000     | ~213 ms      | **~4,700,000**       |

> Throughput scales up at larger batch sizes because the fixed per-connection and WAL setup costs are amortised over more rows.

---

## 2. Unit Test Suite — 23/23 Passed

Run as part of `./benchmark 1000000`:

```
[[...Running Unit Tests...]]]

[PASS] DROP TABLE IF EXISTS TEST_ORDERS     (3 ms)
[PASS] DROP TABLE IF EXISTS TEST_USERS      (4 ms)
[PASS] CREATE TABLE TEST_USERS              (85 ms)
[PASS] INSERT TEST_USERS ID=1               (0 ms)
[PASS] INSERT TEST_USERS ID=2               (0 ms)
[PASS] INSERT TEST_USERS ID=3               (0 ms)
[PASS] INSERT TEST_USERS ID=4               (0 ms)
[PASS] Basic SELECT * validation
[PASS] Single-row value validation
[PASS] Filtered rows validation
[PASS] Empty result-set validation
[PASS] CREATE TABLE TEST_ORDERS             (29 ms)
[PASS] INSERT TEST_ORDERS ORDER_ID=101      (0 ms)
[PASS] INSERT TEST_ORDERS ORDER_ID=102      (0 ms)
[PASS] INSERT TEST_ORDERS ORDER_ID=103      (0 ms)
[PASS] Join with no matches validation
[PASS] Invalid SQL should fail
[PASS] Missing table should fail

Unit Test Summary: 23/23 passed, 0 failed.
```

---

## 3. SQL Clause Validation — 23/23 Passed

Run via `python3 scale_sql_clause_validation.py --rows 20000`:

```
[PASS] COUNT(*)
[PASS] WHERE with AND range
[PASS] WHERE with OR chain
[PASS] Projection query
[PASS] Float literal predicate
[PASS] DROP TABLE IF EXISTS
[PASS] CREATE TABLE
[PASS] INSERT multi-row
[PASS] JOIN
[PASS] Duplicate key reject (expected failure)
[PASS] DROP TABLE cleanup
[PASS] ORDER BY unsupported (expected failure)
[PASS] GROUP BY unsupported (expected failure)
[PASS] HAVING unsupported (expected failure)
[PASS] DISTINCT unsupported (expected failure)
[PASS] LIKE unsupported (expected failure)
[PASS] IN unsupported (expected failure)
[PASS] BETWEEN unsupported (expected failure)
[PASS] LIMIT/OFFSET unsupported (expected failure)
[PASS] Subquery unsupported (expected failure)
[PASS] IS NOT NULL unsupported (expected failure)
[PASS] CASE unsupported (expected failure)
[PASS] MIN/MAX unsupported (expected failure)

Large-scale SQL clause validation complete. (20,000-row dataset)
```

All supported SQL features passed. All unsupported clauses were **correctly rejected** with clear error messages.

---

## 4. Query Cache Performance

The LRU query cache provides near-zero latency on repeated identical `SELECT` queries:

- **Cache hit path:** result returned from in-memory map — no parser, no executor, no table scan.
- **Cache invalidation:** triggered immediately on `INSERT` / `DROP` / `CREATE` for the affected table, preventing stale reads.
- **Aggregate bypass:** `COUNT(*)` queries skip the cache to avoid returning stale counts after inserts.

---

## 5. WAL and Crash Recovery

On a server restart after inserting 1,000,000 rows:

```
FlexQL Server running on port 9000 (replayed rows: 1000010, checkpoint: ok)
```

The WAL is fully replayed on startup. A checkpoint compacts the log immediately after replay, restoring optimal write performance for subsequent operations.

---

## 6. Summary

| Metric                            | Result                   |
|-----------------------------------|--------------------------|
| Peak insert throughput (1M rows)  | **~4.7 million rows/sec** |
| Unit tests passed                 | **23 / 23**              |
| SQL clause validation tests passed | **23 / 23**             |
| WAL crash recovery                | ✅ Full replay on restart |
| Primary key duplicate detection   | ✅ O(1) via Robin Hood hash index |
| Row expiration                    | ✅ Lazy evaluation at query time  |
