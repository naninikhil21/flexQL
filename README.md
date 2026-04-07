# FlexQL

A high-performance, custom SQL database engine built from scratch in C++17 — no SQLite, no third-party storage backend. Features a custom write-ahead log, Robin Hood hash index, LRU query cache, and a TCP-based SQL-compatible server.

---

## Quick Start

```bash
# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel

# Start the server
./build/flexql_server

# Connect via REPL (in a new terminal)
./build/flexql_client 127.0.0.1 9000
```

Once connected:
```sql
flexql> CREATE TABLE USERS (ID DECIMAL PRIMARY KEY, NAME VARCHAR(64));
flexql> INSERT INTO USERS VALUES (1, 'Alice'), (2, 'Bob');
flexql> SELECT * FROM USERS WHERE ID > 0;
flexql> .exit
```

For full build instructions, prerequisites, and SQL reference → **[COMPILATION_AND_EXECUTION.md](COMPILATION_AND_EXECUTION.md)**

---

## Project Structure

```
.
├── src/
│   ├── engine/         # Query executor
│   ├── parsing/        # SQL lexer & parser
│   ├── storage/        # WAL, RowStore, HashIndex
│   └── network/        # TCP server
├── flexql.h / flexql.cpp       # C client library
├── flexql_client.cpp           # Interactive REPL
├── benchmark_flexql.cpp        # Benchmark + unit tests
└── scale_sql_clause_validation.py  # SQL validation suite
```

---

## Documentation

| Document | Description |
|---|---|
| [DESIGN_DOCUMENT.md](DESIGN_DOCUMENT.md) | Architecture: storage, indexing, caching, expiration, multithreading |
| [COMPILATION_AND_EXECUTION.md](COMPILATION_AND_EXECUTION.md) | Build instructions, server setup, SQL reference, C API |
| [PERFORMANCE_RESULTS.md](PERFORMANCE_RESULTS.md) | Benchmark results and test suite outcomes |

---

## Performance

| Metric | Result |
|---|---|
| Insert throughput (1M rows) | **~4.7 million rows/sec** |
| Unit tests | **23 / 23 passed** |
| SQL clause validation | **23 / 23 passed** |

See **[PERFORMANCE_RESULTS.md](PERFORMANCE_RESULTS.md)** for full results.

---

## Features

- `CREATE TABLE`, `DROP TABLE IF EXISTS`, `INSERT INTO VALUES`
- `SELECT *`, column projection, `AS` aliases
- `SELECT COUNT(*) AS alias`
- `WHERE` with `=`, `<`, `>`, `<=`, `>=`, `AND`/`OR` chains
- `INNER JOIN … ON` with implicit and explicit table aliases
- Primary key on any column with O(1) duplicate detection
- LRU query cache with automatic invalidation
- Write-ahead log with crash recovery
- Row expiration via `expires_at` column
- Interactive REPL client with multi-line query support
