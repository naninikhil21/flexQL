# FlexQL Project — Complete Code Review

---

## 1. Overall Assessment

The project is architecturally solid and clearly shows advanced effort. The SQL parser, WAL persistence, LRU cache, row-major storage, and hash-index are all present and mostly functional. However, there are **several real bugs, one missing deliverable (REPL client), and a few spec violations** that need to be addressed before submission.

---

## 2. What Is Correctly Implemented ✅

| Requirement | Status |
|---|---|
| `CREATE TABLE` (INT, DECIMAL, VARCHAR, TEXT, DATETIME, FLOAT) | ✅ |
| `INSERT INTO VALUES (…)` with batch support | ✅ |
| `SELECT *` and `SELECT col1, col2` | ✅ |
| `WHERE` clause with `=, <, >, <=, >=` | ✅ |
| `INNER JOIN … ON` | ✅ |
| Row-major storage layout | ✅ |
| Primary key hash-index (first column) | ✅ |
| LRU query cache with table-level invalidation | ✅ |
| Multithreaded server (`std::thread` per connection) | ✅ |
| Write-Ahead Log (WAL) for persistence & crash recovery | ✅ |
| WAL checkpoint compaction (periodic + on startup) | ✅ |
| Expiration timestamp (`expires_at`) support | ✅ |
| Opaque `FlexQL` struct (correct `typedef struct FlexQL FlexQL`) | ✅ |
| All 4 required API signatures (`open`, `close`, `exec`, `free`) | ✅ |
| Error codes `FLEXQL_OK` and `FLEXQL_ERROR` | ✅ |
| `PRIMARY KEY NOT NULL` modifiers parsed and silently accepted | ✅ |
| `NOT NULL` modifier parsed and silently accepted | ✅ |
| Batch insert via `INSERT INTO VALUES (...), (...)` | ✅ |

---

## 3. Bugs 🐛

### 3.1 `flexql.cpp` — Callback abort return value is ignored (CRITICAL)

The spec says: *"Callback return value: 0 = Continue processing, 1 = Abort query execution."*

The code ignores the callback's return value entirely:

```cpp
// flexql.cpp — current code
callback(arg, static_cast<int>(argv.size()), argv.data(), col.data());
// ^^^^ return value is silently discarded
```

**Fix:**
```cpp
int cb_ret = callback(arg, static_cast<int>(argv.size()), argv.data(), col.data());
if (cb_ret != 0) {
    done = true;
    break;
}
```

---

### 3.2 `flexql.cpp` — `flexql_open` has no NULL checks (CRITICAL)

If `malloc` fails or `outDb` is NULL, the function will crash:

```cpp
// Current code — no checks
FlexQL *db = (FlexQL*)malloc(sizeof(FlexQL));
db->sock = socket(AF_INET, SOCK_STREAM, 0);  // crashes if malloc returned NULL
// ...
*outDb = db;  // crashes if outDb is NULL
```

**Fix:**
```cpp
if (!outDb) return FLEXQL_ERROR;
FlexQL *db = (FlexQL*)malloc(sizeof(FlexQL));
if (!db) return FLEXQL_ERROR;

db->sock = socket(AF_INET, SOCK_STREAM, 0);
if (db->sock < 0) { free(db); return FLEXQL_ERROR; }

if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) { close(db->sock); free(db); return FLEXQL_ERROR; }
```

---

### 3.3 `flexql.cpp` — `flexql_close` and `flexql_exec` have no NULL guard

```cpp
int flexql_close(FlexQL *db) {
    close(db->sock);  // segfault if db == NULL
    free(db);
```

Add `if (!db) return FLEXQL_ERROR;` at the top of both `flexql_close` and `flexql_exec`.

---

### 3.4 `flexql.cpp` — SQL without a trailing semicolon will hang forever

The server's `handle_client` loop uses `;` as the query delimiter. The client sends the SQL string as-is. If a caller passes SQL without `;`, the server buffers it indefinitely and never sends a response — causing `flexql_exec` to block forever.

**Fix:** In `flexql_exec`, ensure the SQL ends with `;` before sending:
```cpp
std::string sql_str(sql);
if (!sql_str.empty() && sql_str.back() != ';') {
    sql_str.push_back(';');
}
send(db->sock, sql_str.c_str(), sql_str.size(), MSG_NOSIGNAL);
```

---

### 3.5 `flexql.cpp` — `inet_pton` return value not checked

`inet_pton` returns 0 for a non-numeric address string (e.g., `"localhost"`) and -1 for an error. The current code ignores both, resulting in a silent connection to address `0.0.0.0`. Add a check as shown in §3.2's fix above. Note that this also means **`"localhost"` as a host string will silently fail** — use `"127.0.0.1"` instead, or switch to `getaddrinfo`.

---

### 3.6 `sql_compat_server.cpp` — `COUNT(*)` is not supported (shown by your own validation)

Your latest validation report shows:
```json
{ "label": "COUNT(*)", "ok": false, "error": "ERROR: expected FROM" }
```

The parser does not support aggregate functions at all (`COUNT`, `SUM`, `AVG`, etc.). While not explicitly required by the spec, the benchmark script tests for it. You should either implement it or note it clearly in your design document as a known limitation.

---

### 3.7 `executor.cpp` — AND/OR precedence logic is incorrect

The parser supports AND/OR chains even though the spec forbids multi-condition WHERE. The executor's evaluation logic tries to handle AND-before-OR precedence, but it's implemented incorrectly for mixed chains like `A OR B AND C`:

```cpp
// The "group" variable accumulates AND terms, but the initial group
// is reset on every OR, losing prior AND results within the same OR group.
bool group = eval_term(0);
bool accum = false;
for (size_t i = 0; i < stmt.where_ops.size(); ++i) {
    bool rhs = eval_term(i + 1);
    if (stmt.where_ops[i] == WhereLogicOp::AND) {
        group = group && rhs;
    } else {
        accum = accum || group;
        group = rhs;   // ← loses previous AND groups on the right of OR
    }
}
```

Since the spec only requires one WHERE condition anyway, the cleanest fix is to reject multi-condition WHERE with an error, which also aligns with the spec. Alternatively, fix the precedence logic with a proper expression tree.

---

### 3.8 `table_store.cpp` — `RowStore` has no internal lock; concurrent same-table writes race

`Catalog::get_table` acquires a short-lived shared lock, returns the raw pointer, then releases the lock. After that, two concurrent writers can call `append_rows` on the **same** `RowStore` simultaneously. `RowStore` has no internal mutex:

```cpp
// Thread 1 and Thread 2 both hold the same RowStore*
// Both enter ensure_capacity_for_rows and resize _slab concurrently → data race
```

The `exec_mu` unique_lock in `sql_compat_server.cpp` normally serialises all writes, but the fast-insert path for `BIG_USERS` acquires `exec_mu` unique lock correctly, so this is fine for now. However, if you ever add more fast-paths or change locking granularity, this becomes a real race condition. Consider adding a per-table `std::mutex` inside `RowStore`.

---

### 3.9 `wal.cpp` — WAL replay skips duplicate-key checking

During replay, rows are inserted via `append_rows` (not `append_rows_checked`), bypassing primary-key duplicate validation. If the WAL is replayed twice (e.g., after a crash during checkpoint), duplicate rows will be inserted silently:

```cpp
// wal.cpp — replay path
table_store->append_rows(data.data(), static_cast<size_t>(row_count));
// ^^^^ no duplicate PK check
```

**Fix:** Use `append_rows_checked` here, or truncate the WAL after successful replay.

---

### 3.10 `hash_index.h` — Hash index does not resize; silently drops inserts at 90% load

```cpp
if (_size >= _capacity * 0.9) {
    return false; // Needs resize, not implemented for this simple case
}
```

At 90% occupancy (900,000 entries in a 1M-slot map), all further `insert` calls silently return `false`. In `Worker::run`, this return value is not checked, so rows are lost without any error. **Note:** The legacy `Worker` path appears to be dead code (not used by `SqlCompatServer`), but this is still a correctness bug in any code that uses `HashIndex`.

---

## 4. Missing Features / Spec Violations ⚠️

### 4.1 No interactive REPL client binary (CRITICAL — required deliverable)

The spec explicitly requires:

> *"The client should behave like a simple database terminal interface... `$ ./flexql-client 127.0.0.1 9000`"*

The project has `flexql.cpp` (the client **library**) and the benchmark binary, but **there is no standalone interactive client REPL program** that accepts user input and sends queries. This is a required deliverable per the project spec. You need a `main()` that looks roughly like:

```cpp
// flexql_client_main.cpp
int main(int argc, char* argv[]) {
    // parse host/port from argv
    FlexQL* db;
    flexql_open(host, port, &db);
    std::string line;
    while (std::cout << "flexql> " && std::getline(std::cin, line)) {
        char* errmsg = nullptr;
        flexql_exec(db, line.c_str(), callback, nullptr, &errmsg);
        // print result / error
        flexql_free(errmsg);
    }
    flexql_close(db);
}
```

---

### 4.2 Parser accepts multi-condition WHERE (AND/OR) — spec says only one condition

The spec says: *"Only one condition is allowed. Logical combinations such as AND or OR are not to be supported."*

Your parser parses AND/OR chains. This isn't harmful (it's extra functionality), but if the grader tests that AND/OR returns a parse error, your implementation will pass them silently. Consider at least documenting this as an intentional extension.

---

### 4.3 Primary key indexing is always on column[0], ignoring `PRIMARY KEY` annotation

The parser correctly parses `PRIMARY KEY` but immediately discards the information. The `RowStore` always uses column index 0 as the primary key for the hash index, regardless of which column is declared `PRIMARY KEY`. This means:

```sql
CREATE TABLE T (NAME TEXT, ID INT PRIMARY KEY NOT NULL);
```

...will index on `NAME` (column 0), not `ID`. If the benchmark creates tables with a non-first primary key column, queries will not benefit from the index.

---

### 4.4 `BULK_INSERT` command is hardcoded for `BIG_USERS` only

The `try_fast_bulk_insert_big_users` and `try_fast_insert_big_users` fast-paths work only when the table is literally named `BIG_USERS` with exactly 5 specific columns. Any other table will fall through to the generic parser. This is fine for the benchmark, but it should be clearly documented — the `BULK_INSERT` command is not a standard SQL extension.

---

### 4.5 `COUNT(*)` / aggregate functions not supported

The validation report from today shows `COUNT(*)` failing with `"ERROR: expected FROM"`. While aggregates aren't in the core spec, if the benchmark script tests them you'll lose points. Implementing `COUNT(*)` is straightforward: add a special case in the `SELECT` parser and executor.

---

## 5. Performance / Design Notes

### 5.1 One thread per connection — does not scale to many clients

The server spawns `std::thread(handle_client, client_fd).detach()` for every connection. With 100+ concurrent clients this wastes OS threads. A thread pool or epoll-based approach would scale better. The legacy `io_uring_reactor` path exists but is not wired into the active server path.

### 5.2 `format_response` builds the entire response in one `std::string`

For large `SELECT *` results returning millions of rows, this allocates one giant string before sending. For truly large results, streaming the response in chunks would reduce peak memory.

### 5.3 Query cache capacity is hardcoded to 256

`QueryCache(256)` is a reasonable default, but making it configurable (env variable or command-line flag) would improve observability and tuning.

### 5.4 The `exec_mu` shared_mutex serialises all writes globally

All INSERT/DDL operations take a global `unique_lock<shared_mutex>`. This means only one write can proceed at a time across all tables. For a multi-table workload, per-table locking would be more performant.

### 5.5 WAL writer thread uses a deque with mutex + condvar — good design

The async WAL writer with `writev` batching is well-designed and minimises write latency on the critical path. The `FLEXQL_STRICT_DURABILITY` env flag for `fdatasync` is a nice touch.

---

## 6. Minor / Code-Quality Issues

| Location | Issue |
|---|---|
| `flexql.cpp` | `buffer[4096 + 1]` is declared on the stack — fine, but document the 4KB read chunk |
| `table_store.cpp` | `PREALLOC_ROWS = 2,000,000` pre-allocates ~160MB per table (for a 80-byte row) — document this |
| `sql_compat_server.cpp` | The checkpoint background thread is `detach()`-ed — on server shutdown it may write to a closed WAL fd |
| `wal.cpp` | `checkpoint_from_catalog` restarts the writer thread by directly assigning `_stop_flag = false` and creating a new thread — this is not safe if called from multiple threads concurrently |
| `executor.cpp` | `ieq_ascii` is copy-pasted in three files — move to a shared `utils/string_util.h` |
| `worker.cpp` | The entire legacy Worker/Server/IOUringReactor path is dead code not reachable from `main.cpp` — remove or clearly label it |

---

## 7. Summary Checklist

| # | Issue | Severity |
|---|---|---|
| 3.1 | Callback abort return value ignored | 🔴 Bug |
| 3.2 | `flexql_open` malloc/outDb NULL crash | 🔴 Bug |
| 3.3 | `flexql_close`/`flexql_exec` NULL crash | 🔴 Bug |
| 3.4 | SQL without `;` hangs `flexql_exec` | 🔴 Bug |
| 3.5 | `inet_pton` result unchecked; `"localhost"` silently fails | 🟠 Bug |
| 3.6 | `COUNT(*)` fails (shown by your own validation) | 🟠 Bug |
| 3.7 | AND/OR WHERE logic is incorrect | 🟡 Bug |
| 3.8 | RowStore has no internal lock | 🟡 Bug |
| 3.9 | WAL replay skips duplicate PK check | 🟡 Bug |
| 3.10 | HashIndex silently drops inserts at 90% load | 🟡 Bug |
| 4.1 | No interactive REPL client | 🔴 Missing |
| 4.3 | PRIMARY KEY on non-first column ignored | 🟠 Missing |
| 4.4 | BULK_INSERT hardcoded for BIG_USERS only | 🟡 Note |
| 5.1 | Thread-per-connection doesn't scale | 🟡 Design |
