# FlexQL — Complete Code Review

---

## Executive Summary

The codebase has two distinct parts: the **legacy server** (`flexql_server.cpp`) and the **new server skeleton** (`src/`). Both have serious problems relative to the project requirements. The legacy server is a direct SQLite wrapper, which is explicitly banned. The new server has impressive low-level infrastructure but is functionally incomplete — it has no SQL parser, no table engine, no caching, no expiration, and no query executor. The `sql_compat_server.cpp` file is missing entirely from the submitted zip (though its compiled object exists in `build/`).

**What works correctly:** `flexql.h`, `flexql.cpp` (client library), and the low-level utilities (arena, MPSC queue, thread affinity, WAL writer skeleton, Robin Hood hash, io_uring reactor).

**What doesn't work / is missing:** Everything related to actually processing SQL.

---

## 🔴 CRITICAL VIOLATIONS (Project-Disqualifying)

### 1. SQLite is Banned — You Are Using It Everywhere

The project PDF states explicitly:
> *"External database libraries are not allowed. The entire query handling and storage logic must be implemented by the students."*

**Violation 1 — `flexql_server.cpp`:**
```cpp
#include <sqlite3.h>
sqlite3_open("flexql.db", &db);
sqlite3_exec(db, sql.c_str(), callback, &client_socket, &errMsg);
```
This is a 100% SQLite wrapper. There is zero original database logic here.

**Violation 2 — `CMakeLists.txt`:**
```cmake
find_package(SQLite3 REQUIRED)
target_link_libraries(flexql_server PRIVATE SQLite::SQLite3)
```
The new server also links SQLite. Even if `sql_compat_server.cpp` does not call SQLite directly, the fact that it's linked means the submission uses SQLite.

**Both of these must be completely removed.** You need to implement your own storage, parsing, and query execution.

---

### 2. `sql_compat_server.cpp` Is Missing from the Submission

The CMake build system references it, the compiled object `build/CMakeFiles/flexql_server.dir/src/network/sql_compat_server.cpp.o` exists, but **the source file itself is not in the zip**. The header `sql_compat_server.h` exists but has no implementation file. This means your submitted source code cannot actually build the new server.

---

## 🔴 MAJOR MISSING FEATURES

### 3. No SQL Parser Whatsoever

`src/parsing/simd_parser.cpp` does **not** parse SQL. It parses raw comma-separated integer triplets:
```
key,val1,val2\n
```
There is no code anywhere in `src/` that handles:
- `CREATE TABLE`
- `INSERT INTO`
- `SELECT`
- `WHERE`
- `INNER JOIN`
- `DROP TABLE`

No lexer. No parser. No AST. The io_uring reactor calls `SIMDParser::parse_batch_into()` which produces `ParsedRow{uint64_t key, uint32_t val1, uint32_t val2}` — three hardcoded integers. This is a completely different data format from what the benchmark sends (full SQL text).

**What the benchmark actually sends over TCP:**
```sql
CREATE TABLE BIG_USERS(ID DECIMAL, NAME VARCHAR(64), EMAIL VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL);
INSERT INTO BIG_USERS VALUES (1, 'user1', 'user1@mail.com', 1001.0, 1893456000),(2, ...);
SELECT NAME, BALANCE FROM TEST_USERS WHERE ID = 2;
```

None of this can be processed by the current code.

---

### 4. No Table Storage Engine

The `Worker` class stores rows as:
```cpp
_index.insert(row.key, data);  // HashIndex<uint64_t, const char*>
```

There is no concept of:
- Tables with names
- Schemas (column names, types, column count)
- Multiple columns per row
- `DECIMAL` vs `VARCHAR(N)` vs `DATETIME` type encoding
- A row store holding variable-schema rows

For 1M BIG_USERS rows with schema `(ID DECIMAL, NAME VARCHAR(64), EMAIL VARCHAR(64), BALANCE DECIMAL, EXPIRES_AT DECIMAL)`, you need a typed storage layer. Currently, only 3 hardcoded integers are stored per "row."

---

### 5. No Caching Layer

The project requires a caching mechanism (LRU, LFU, or predictive) for query performance. There is **no cache of any kind** anywhere in the codebase. Not even a placeholder. This is an explicit graded component.

---

### 6. No Expiration Timestamp Handling

The project states:
> *"Each inserted row must also include an expiration timestamp."*

The benchmark schema has `EXPIRES_AT DECIMAL` as a column. Beyond being stored as a raw value, there is no code that:
- Tracks per-row expiration times
- Filters expired rows during SELECT
- Background-purges expired rows
- Documents the expiration strategy

---

### 7. No WAL Replay / No Real Disk Persistence

The project requires:
> *"Data must be persistent. You cannot use RAM as the primary storage. The final data must be stored persistently, and the whole system should be fault-tolerant."*

The WAL writes raw `LogEntry` buffers to disk via `writev()`. But:

1. **No replay mechanism exists.** On server restart, the WAL is never read back. All data in memory is lost. The WAL file grows but is never consumed.

2. **`fdatasync` is explicitly commented out:**
   ```cpp
   // fdatasync(_fd);
   ```
   Without `fdatasync`, even the WAL writes are not durable against OS crash. The comment says "for the benchmark, we assume async durability" — but the project requires fault tolerance.

3. **`LogEntry` holds a dangling `const char*`:**
   ```cpp
   struct LogEntry {
       const char* data;  // raw pointer — does NOT own the data
       size_t len;
   };
   ```
   The WAL stores a pointer into the arena allocator's buffer. If the arena is ever reset or the server restarts, this pointer is invalid. On startup, there is no way to reconstruct the typed row data from these raw bytes because no schema information is written to the WAL.

4. **The WAL format is schemale.** There's no table name, no column schema, no row count in the WAL records — just raw bytes. This cannot be replayed into a typed table.

---

### 8. No INNER JOIN Implementation

The project requires:
```sql
SELECT * FROM tableA INNER JOIN tableB ON tableA.column = tableB.column WHERE ...;
```

The benchmark tests this:
```cpp
"SELECT TEST_USERS.NAME, TEST_ORDERS.AMOUNT "
"FROM TEST_USERS INNER JOIN TEST_ORDERS ON TEST_USERS.ID = TEST_ORDERS.USER_ID "
"WHERE TEST_ORDERS.AMOUNT > 900;"
```

There is zero JOIN logic anywhere in the codebase.

---

### 9. WHERE Clause Operators Missing

The project (updated in the email) requires: `=`, `>`, `<`, `>=`, `<=`

The benchmark uses `WHERE BALANCE > 1000` and `WHERE ID = 2`. There is no WHERE evaluation code anywhere. The raw integer parser has no concept of predicates.

---

### 10. `DROP TABLE IF EXISTS` Not Implemented

The benchmark's first operation is:
```cpp
run_exec(db, "DROP TABLE IF EXISTS BIG_USERS;", ...)
```

There is no DROP TABLE handling anywhere.

---

## 🟠 BUGS IN EXISTING CODE

### 11. `HashIndex::insert()` Silently Fails at 90% Load

```cpp
bool insert(const Key& key, const T& value) {
    if (_size >= _capacity * 0.9) {
        return false;  // <-- SILENT FAILURE, no resize, no error propagation
    }
    ...
}
```

The worker initializes this with `_index(1024 * 1024, _allocator)` — capacity 1M. At 900,000 insertions, all further inserts silently return `false`. For a 1M row benchmark, this drops ~100,000 rows with no warning. The caller in `worker.cpp` doesn't check the return value:
```cpp
_index.insert(row.key, data);  // return value ignored
```

---

### 12. `LogEntry` Dangling Pointer Risk

```cpp
char* data = (char*)_allocator.allocate(sizeof(row));
std::memcpy(data, &row, sizeof(row));
_index.insert(row.key, data);
log_entries.push_back({data, sizeof(row)});  // stores raw pointer
```

`LogEntry::data` points into the arena. The WAL background thread accesses this pointer asynchronously. If the arena is ever reset between the `push_back` and the WAL `writev`, this is a use-after-free. In the current server lifetime, this is unlikely to trigger, but it is structurally unsound.

---

### 13. `flexql_server.cpp` — Single Client Only, No Multithreading

```cpp
while (1) {
    client_socket = accept(...);   // blocks here
    // handles ONE client entirely
    while ((bytes_read = read(client_socket, ...)) > 0) { ... }
    close(client_socket);
    // THEN accepts next client
}
```

This is a sequential single-threaded server. The project explicitly requires:
> *"The server must support multiple simultaneous clients and therefore should be implemented as a multithreaded server."*

If a second client connects while the first is active, it just waits. Zero concurrency.

---

### 14. `io_uring_reactor.cpp` — 64MB Static Buffer Allocation

```cpp
std::array<std::array<char, BUFFER_SIZE>, MAX_CONNECTIONS> _buffers{};
// = 1024 connections × 65536 bytes = 64MB
```

This is allocated inside the `IOUringReactor` object (on the heap via `make_unique`). For 1024 possible connections, most of which will never be used simultaneously in the benchmark, this wastes 64MB. More importantly, client_fd values from the OS can exceed `MAX_CONNECTIONS` (1024), and the code crashes if they do:
```cpp
void IOUringReactor::add_read_request(int client_fd) {
    if (client_fd < 0 || client_fd >= MAX_CONNECTIONS) {
        close(client_fd);  // drops the client silently
        return;
    }
    ...
    io_uring_prep_read(sqe, client_fd, _buffers[client_fd].data(), ...);
```
On a loaded system, `client_fd` can easily be > 1024.

---

### 15. `simd_parser.h` Includes `<immintrin.h>` But Uses No SIMD

```cpp
#include <immintrin.h>
```
This header is included but no SIMD intrinsics (`_mm256_*`, `_mm_*`, etc.) are used anywhere in `simd_parser.cpp`. The comment even says:
```cpp
// A simple, non-SIMD integer parser for baseline.
// A full SIMD version is a project in itself.
```
The name "SIMDParser" is misleading. This is just a basic integer tokenizer.

---

### 16. `worker.cpp` — Spin-yield Loop Is CPU-Wasteful

```cpp
if (!processed_any) {
    std::this_thread::yield();  // spin-yield between batches
}
```

For a benchmark that sends 1M rows in large batches, this means the worker thread is yielding constantly between batch arrivals. A condition variable wait (like the WAL uses) would be more efficient and wouldn't burn CPU cycles between batches.

---

### 17. WAL `append()` Copies Pointers, Not Data

```cpp
void WriteAheadLog::append(const std::vector<LogEntry>& entries) {
    std::unique_lock<std::mutex> lock(_mutex);
    _log_buffer.insert(_log_buffer.end(), entries.begin(), entries.end());
}
```

`LogEntry` contains a `const char*` pointer. The WAL copies the `LogEntry` structs (which are just `{pointer, length}` pairs) into `_log_buffer`. It does **not** copy the actual data that the pointer points to. If the source data moves (arena reallocation, stack frame) between the `append()` call and the background `writev()`, you get a corrupted WAL. The correct approach is to either copy the data into the WAL's own buffer, or ensure the arena lifetime exceeds the WAL flush.

---

## 🟡 MISSING REQUIRED FEATURES (Not Yet Started)

| Feature | Required By | Status |
|---------|-------------|--------|
| SQL Lexer + Parser | PDF: all SQL commands | ❌ Not started |
| CREATE TABLE with schema enforcement | PDF §a | ❌ Not started |
| INSERT with typed value encoding | PDF §b | ❌ Not started |
| SELECT * and SELECT col1, col2 | PDF §c | ❌ Not started |
| WHERE clause (=, >, <, >=, <=) | PDF §d + email | ❌ Not started |
| INNER JOIN with optional WHERE | PDF §e | ❌ Not started |
| DROP TABLE IF EXISTS | Benchmark | ❌ Not started |
| DECIMAL type | PDF §a | ❌ Not started |
| VARCHAR(N) type | PDF §a | ❌ Not started |
| DATETIME type | PDF §a | ❌ Not started |
| INT type | PDF §a | ❌ Not started |
| Expiration timestamp per row | PDF §b | ❌ Not started |
| Background expiry reaper | PDF §b | ❌ Not started |
| Primary key index (connected to schema) | PDF Indexing | ❌ Not started |
| Query result cache (LRU/LFU/etc.) | PDF Caching | ❌ Not started |
| WAL replay on startup | PDF Persistence | ❌ Not started |
| Multithreaded connection handling | PDF Concurrency | ❌ Not started (legacy server is single-threaded) |
| Multi-client concurrent access safety | PDF Concurrency | ❌ Not started |
| Disk persistence (no RAM as primary) | PDF + Email | ❌ Not started |
| Fault tolerance / crash recovery | Email Image 1 | ❌ Not started |
| Batch INSERT support | Email Image 1 | ❌ Not started |

---

## ✅ WHAT IS CORRECTLY IMPLEMENTED

| Component | Quality |
|-----------|---------|
| `flexql.h` — API header | ✅ Correct opaque handle, correct signatures |
| `flexql.cpp` — Client library | ✅ Correct TCP connection, send/recv, ROW/END/ERROR parsing, callback |
| `src/utils/arena_allocator.h` | ✅ Correct slab allocator with alignment |
| `src/utils/mpsc_ring_buffer.h` | ✅ Correct lock-free MPSC queue with cache-line alignment |
| `src/utils/thread_affinity.h` | ✅ Correct Linux CPU pinning |
| `src/storage/wal.cpp/.h` (writer) | ✅ Correct async `writev` via background thread — good foundation |
| `src/storage/hash_index.h` | ✅ Correct Robin Hood probing, good foundation |
| `src/network/io_uring_reactor.cpp/.h` | ✅ Correct io_uring accept/read loop — good foundation |
| `CMakeLists.txt` build flags | ✅ `-Ofast -march=native -flto` are correct for performance |

---

## What Needs to Be Built

To pass this project, the following need to be implemented from scratch, in rough priority order:

1. **Remove SQLite** from CMakeLists.txt and `flexql_server.cpp` entirely.

2. **SQL Lexer** — tokenize keywords, identifiers, literals, operators. Hand-written is fine; the SQL subset is small.

3. **SQL Parser** — recursive descent for CREATE TABLE, DROP TABLE, INSERT, SELECT (with WHERE and JOIN).

4. **Type system** — DECIMAL (int64_t), VARCHAR(N) (N-byte char buffer), DATETIME (int64_t Unix timestamp), INT (int64_t).

5. **Row-oriented table store** — fixed-size row slabs (one per table). Pre-allocate for 2M rows. Each row = sum of all column widths in bytes.

6. **Table catalog** — maps table name → (Schema, RowStore). Protected by a reader-writer lock for concurrent access.

7. **Primary index** — connect `HashIndex<int64_t, size_t>` (key → row offset) to each table's first column. This already exists structurally, just needs wiring.

8. **Query executor** — walks the parsed AST, calls the appropriate table store operations, formats ROW/OK/ERROR responses.

9. **Expiration handling** — store `expires_at` as a column, filter expired rows in all SELECT results, and run a background thread that purges expired rows periodically.

10. **Query result cache** — LRU cache keyed on the SQL string (after normalizing whitespace). Invalidated on INSERT/DROP affecting the queried table.

11. **WAL replay** — on startup, read the WAL file and reconstruct all tables. Write schema entries to WAL on CREATE TABLE.

12. **Multithreaded connection handler** — one thread per accepted client (or a thread pool). Use a `shared_mutex` on the catalog (readers in parallel, writer for DDL).

13. **`sql_compat_server.cpp`** — the glue: TCP accept loop → read SQL → parse → execute → format → send response.
