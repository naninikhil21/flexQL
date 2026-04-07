# FlexQL — Updated Code Review (v2)

---

## Executive Summary

This is a **massive improvement** over the previous version. The core SQLite violation is completely fixed, and the vast majority of required features are now genuinely implemented from scratch. The server can actually run the benchmark end-to-end. The review below identifies the remaining bugs, some design risks, and a few missing polish items.

**Overall status: Functionally complete. Several bugs need fixing before submission.**

---

## ✅ What's Correctly Fixed Since Last Review

| Previous Issue | Status |
|---|---|
| SQLite wrapper in `flexql_server.cpp` | ✅ Fixed — now prints "deprecated" and exits |
| `CMakeLists.txt` links SQLite | ✅ Fixed — no SQLite anywhere, message says "no SQLite" |
| `sql_compat_server.cpp` missing | ✅ Fixed — fully implemented |
| No SQL lexer/parser | ✅ Fixed — complete hand-written recursive descent parser |
| No table storage engine | ✅ Fixed — fixed-size row slab with dynamic growth |
| No query executor | ✅ Fixed — CREATE, DROP, INSERT, SELECT, JOIN all implemented |
| No WHERE operators | ✅ Fixed — =, <, >, <=, >= all supported |
| No INNER JOIN | ✅ Fixed — nested loop with ON + optional WHERE |
| No DROP TABLE IF EXISTS | ✅ Fixed |
| No caching | ✅ Fixed — LRU query cache with normalization |
| No expiration handling | ✅ Fixed — filtering in SELECT + background purge thread |
| No WAL replay | ✅ Fixed — full binary WAL with CREATE/INSERT/DROP records |
| WAL replay missing disk persistence | ✅ Fixed — checkpoint + replay on startup |
| Single-threaded server | ✅ Fixed — thread-per-connection with reader-writer lock |
| `HashIndex` silently fails at 90% load | ✅ Fixed — replaced with `std::unordered_map` in RowStore |
| `LogEntry` dangling pointer bug | ✅ Fixed — WAL now copies data into owned `vector<char>` |

---

## 🔴 BUGS — Must Fix Before Submission

### Bug 1: `writev()` can exceed `IOV_MAX` and silently lose WAL data

**File:** `src/storage/wal.cpp` — `WriteAheadLog::run()`

```cpp
std::vector<iovec> iov;
iov.reserve(pending.size());
for (auto& rec : pending) { iov.push_back(...); }
// ❌ No bounds check
if (!iov.empty() && writev(_fd, iov.data(), static_cast<int>(iov.size())) == -1)
```

On Linux, `IOV_MAX = 1024`. If `pending` has more than 1024 entries, `writev()` fails with `EINVAL`. The error is printed to stderr but **the data is lost** — it is neither retried nor re-queued. With `INSERT_BATCH_SIZE=250000` and a single benchmark client, only 4 records are ever pending at once (1M / 250k = 4), so this won't trigger during the benchmark. But under multi-client load or with many small INSERT statements, this is a silent data loss bug.

**Fix:**
```cpp
size_t written = 0;
while (written < iov.size()) {
    int count = static_cast<int>(std::min(iov.size() - written, (size_t)IOV_MAX));
    if (writev(_fd, iov.data() + written, count) == -1) {
        std::cerr << "Error writing to WAL" << std::endl;
        break;
    }
    written += count;
}
```

---

### Bug 2: `encode_int64_slot()` in the fast path rejects `INT` and `DATETIME` columns

**File:** `src/network/sql_compat_server.cpp`

```cpp
bool encode_int64_slot(char* row, const ColDesc& col, int64_t v) {
    if (col.type != ColDesc::Type::DECIMAL || col.size != sizeof(int64_t)) {
        return false;  // ❌ Rejects INT and DATETIME silently
    }
    ...
}
```

The fast path for BIG_USERS hardcodes column indices 0–4 and calls this for columns 0, 3, 4. Since BIG_USERS uses `DECIMAL` for all numeric columns, the benchmark passes. But if any `INT` or `DATETIME` column appears in position 0, 3, or 4 (perfectly valid SQL), the fast path silently returns an error `"BIG_USERS schema mismatch"`.

**Fix:** Broaden the type check:
```cpp
if (!(col.type == ColDesc::Type::DECIMAL ||
      col.type == ColDesc::Type::INT ||
      col.type == ColDesc::Type::DATETIME) || col.size != sizeof(int64_t)) {
    return false;
}
```

---

### Bug 3: Lexer produces a malformed token when `-` or `+` is followed by whitespace

**File:** `src/parsing/sql_lexer.h` — `next_raw()`

```cpp
if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
    ++_pos;  // consumes '-'
    // reads digits... but if next char is ' ', exits immediately
    return {TokenType::INTEGER_LITERAL, _input.substr(start, _pos - start)};
    // returns INTEGER_LITERAL("-") — one character, no digits
}
```

`parse_decimal_to_i64("-", ...)` then fails with "invalid numeric literal" and the entire INSERT/WHERE fails. In practice, SQL like `WHERE BALANCE > - 5` (with a space after the sign) triggers this. The benchmark doesn't generate such inputs, but it's a latent bug.

**Fix:** If `-` or `+` is not immediately followed by a digit, return it as `UNKNOWN` or a minus operator instead of entering the numeric branch.

```cpp
if (c >= '0' && c <= '9') { /* numeric */ }
else if ((c == '-' || c == '+') && _pos + 1 < _input.size() &&
         _input[_pos + 1] >= '0' && _input[_pos + 1] <= '9') {
    /* signed numeric */
}
```

---

### Bug 4: `error` message says "expected DECIMAL" for INT/DATETIME type mismatches

**File:** `src/engine/executor.cpp` — `encode_value()`

```cpp
if (v.kind != Value::Kind::INTEGER) {
    err = "type mismatch: expected DECIMAL";  // ❌ wrong message for INT/DATETIME columns
    return false;
}
```

Minor, but confusing in error output. Change to `"type mismatch: expected integer value"`.

---

## 🟠 DESIGN ISSUES — Not Bugs, But Risk Points

### Issue 1: Periodic checkpoint blocks ALL clients for the entire checkpoint duration

**File:** `src/network/sql_compat_server.cpp`

```cpp
// Background reaper thread:
std::unique_lock<std::shared_mutex> lk(exec_mu);      // ← exclusive lock
size_t removed = catalog.purge_expired_all(now_s);
if (removed > 0) {
    query_cache.clear();
    if (now_s - last_checkpoint_s >= 30) {
        (void)wal.checkpoint_from_catalog(catalog);   // ← writes entire DB to disk
    }
}
// exec_mu released here
```

`checkpoint_from_catalog` rewrites the entire WAL file synchronously — for 1M rows of BIG_USERS that is ~154MB of disk writes with `fsync`. While this runs, every client thread is blocked waiting for `exec_mu`. This is a liveness problem under production load. For the benchmark it only happens after the inserts are done (the benchmark inserts all rows, then runs unit tests), so it won't affect benchmark timing.

**Fix for the design doc:** Acknowledge the tradeoff. A production fix would be copy-on-write snapshots or an MVCC checkpoint that doesn't need exclusive lock for the entire write.

---

### Issue 2: `worker.cpp` is dead code that still compiles and links

**File:** `src/engine/worker.cpp`

`Worker` is never instantiated. `main.cpp` only creates a `SqlCompatServer`. `server.cpp` and `io_uring_reactor.cpp` are also dead code (compiled but never called). This wastes build time and could confuse a reviewer.

**Recommendation:** Either remove these files from compilation or add a clear comment at the top: `// Retained for reference — not used in SqlCompatServer path.`

---

### Issue 3: Column names in results are lowercase; may not match SQL standard

All column names are stored and returned in lowercase (`to_lower_copy` in Catalog and Parser). The benchmark's unit tests only check row values (e.g. `"Bob|450"`), not column names, so all tests pass. However, if the grader's benchmark is updated to check column names for case (e.g. `columnNames[0]` should be `"ID"` not `"id"`), the lowercase output would fail.

**Recommendation:** Store names in lowercase internally but return them in the case they were declared. Or normalize to uppercase for output.

---

### Issue 4: `try_fast_insert_big_users` doesn't validate column names

The fast path checks that BIG_USERS has exactly 5 columns (`schema.columns.size() != 5`) but does not verify the column names are `(id, name, email, balance, expires_at)`. If someone creates a `BIG_USERS` table with 5 columns in a different order, the fast path will silently write to the wrong column offsets. Since the benchmark always creates the same schema, this is not a practical risk, but it is architecturally fragile.

**Recommendation:** Add name checks:
```cpp
if (schema.columns[0].name != "id" || schema.columns[1].name != "name" || ...)
```

---

### Issue 5: WAL grows unboundedly between checkpoints; replay time increases linearly

The WAL accumulates all INSERTs from startup until the next checkpoint. With 1M rows at 154 bytes each plus headers, the WAL can reach ~200MB before the first checkpoint (30s after the first expiry purge). Replay on restart reads the entire WAL, which is proportional to all rows ever inserted, not just current state.

The `checkpoint_from_catalog` call compacts this correctly, but checkpoints only happen when `purge_expired` removes at least one row AND 30 seconds have passed since the last checkpoint. A table with far-future `EXPIRES_AT` (like the benchmark's year 2030) will **never trigger a checkpoint** during normal operation because `purge_expired` will never remove any rows.

**Result:** On every restart, the server replays the full WAL from scratch. For 1M rows, this adds ~1-2 seconds to startup time and is correct, but the WAL file grows forever.

**Fix:** Run a checkpoint on a time-based interval regardless of whether rows were expired, not just when `removed > 0`:
```cpp
int64_t now_s = now_epoch_seconds();
if (now_s - last_checkpoint_s >= 30) {
    (void)wal.checkpoint_from_catalog(catalog);
    last_checkpoint_s = now_s;
}
```

---

### Issue 6: SELECT result cache is invalidated on every single write, including empty inserts

```cpp
} else {
    query_cache.clear();  // ← called for ALL non-SELECT operations
}
```

`query_cache.clear()` is called for every INSERT, CREATE, and DROP, even if they affect a completely different table than whatever is cached. A `CREATE TABLE TEST_USERS` would clear cached results for `SELECT * FROM BIG_USERS`. For the benchmark this is fine, but a more precise implementation would invalidate only the entries referencing the modified table.

---

### Issue 7: The `expires_at <= now` semantics mark rows expiring exactly now as expired

```cpp
return exp <= now_s;
```

A row with `EXPIRES_AT = now` is treated as already expired. Whether `<=` or `<` is semantically correct depends on your definition ("expires at" vs "alive until"). This only matters for rows inserted with `EXPIRES_AT = current_time()` which the benchmark never does (it uses year 2030). Document your choice in the design doc.

---

## 🟡 MISSING FROM REQUIREMENTS (Lower Priority)

### 1. `PRIMARY KEY` and `NOT NULL` parsing not supported

The project PDF shows:
```sql
CREATE TABLE STUDENT(ID INT PRIMARY KEY NOT NULL, FIRST_NAME TEXT NOT NULL, ...);
```

The parser returns an error on `PRIMARY KEY` and `NOT NULL`. The **benchmark** uses none of these keywords, so all tests pass. But the project description requires this for the REPL interface. Add `PRIMARY KEY` as an optional ignored modifier after the column type (since you already effectively treat the first integer column as a primary key), and ignore `NOT NULL`.

### 2. `TEXT` type not recognised

The PDF examples use `TEXT`. The lexer only recognizes `DECIMAL`, `VARCHAR`, `INT`, `DATETIME`. `TEXT` is unrecognized → parse error. The benchmark doesn't use `TEXT`, but the REPL interactive mode would fail.

### 3. `FLOAT` / `REAL` types not supported

The benchmark's BALANCE column stores values like `1001.0` (floating point in the SQL). These are parsed and truncated to `int64_t`. This is correct for the benchmark (all values are whole numbers), but proper `FLOAT` support is missing.

### 4. No `PRIMARY KEY` column enforcement

The implementation treats column[0] as the primary key if it's an integer type, but there is no unique constraint enforcement. Two rows with `ID = 5` can both be inserted. The pk_index would just overwrite the old entry, pointing to the newer row, leaving the old row unreachable but still consuming space. This is an invisible duplicate-key problem.

### 5. No `AND`/`OR` in WHERE even though `AND`/`OR` are recognised as keywords

The lexer recognises `AND` and `OR` as keywords. The parser does not use them. The project spec says only one condition is needed, so this is by-spec, but worth noting in the design doc.

---

## ✅ Correctly Implemented Features (Full Checklist)

| Feature | Verdict |
|---|---|
| No SQLite anywhere | ✅ Confirmed — `grep sqlite` returns nothing in `src/` |
| SQL Lexer (all operators = < > <= >=) | ✅ |
| Parser: CREATE TABLE with DECIMAL/VARCHAR/INT/DATETIME | ✅ |
| Parser: DROP TABLE IF EXISTS | ✅ |
| Parser: INSERT INTO ... VALUES (...),(...) multi-row | ✅ |
| Parser: SELECT * / SELECT cols FROM table | ✅ |
| Parser: WHERE with all 5 operators | ✅ |
| Parser: INNER JOIN ... ON ... WHERE | ✅ |
| Fixed-size row slab storage (row-major) | ✅ |
| Dynamic slab growth without breaking pointers (resize) | ✅ |
| Primary key index on column[0] (integer types) | ✅ |
| Index-based O(1) lookup for PK equality WHERE | ✅ |
| LRU query cache (capacity 256, thread-safe) | ✅ |
| Cache invalidation on any write | ✅ |
| Cache key normalisation (whitespace + lowercase) | ✅ |
| Expiration filtering in SELECT (single table) | ✅ |
| Expiration filtering in INNER JOIN | ✅ |
| Background expiry purge thread (every 2s) | ✅ |
| WAL with magic numbers for record types | ✅ |
| WAL: CREATE TABLE record format | ✅ |
| WAL: DROP TABLE record format | ✅ |
| WAL: INSERT batch record format | ✅ |
| WAL replay on startup | ✅ |
| WAL checkpoint (compact to current state + fsync) | ✅ |
| Multithreaded server (thread-per-connection) | ✅ |
| Reader-writer lock (concurrent SELECTs allowed) | ✅ |
| TCP_NODELAY + large socket buffers | ✅ |
| 8MB recv buffer (handles 250k-row batches) | ✅ |
| Fast-path INSERT for BIG_USERS (no parse overhead) | ✅ |
| `INSERT_BATCH_SIZE` increased to 250000 | ✅ |
| Disk persistence (WAL survives process exit) | ✅ |
| Fault tolerance (replay reconstructs state on restart) | ✅ |
| Case-insensitive table/column names | ✅ |
| Error response for unknown table/column | ✅ |
| Error response for column count mismatch | ✅ |
| `flexql_server.cpp` correctly deprecated | ✅ |
| `flexql.h` / `flexql.cpp` client API unchanged | ✅ |

---

## Summary of Remaining Work

| Priority | Item |
|---|---|
| 🔴 Fix now | `writev` IOV_MAX overflow in `wal.cpp` |
| 🔴 Fix now | `encode_int64_slot` rejects INT/DATETIME in fast path |
| 🔴 Fix now | Lexer: `-` or `+` before whitespace produces bad token |
| 🟠 Before demo | Time-based WAL checkpoint (not just on expiry) |
| 🟠 Before demo | Document the dead `worker.cpp` / `server.cpp` code |
| 🟡 Design doc | Explain checkpoint blocking tradeoff |
| 🟡 Design doc | Explain `expires_at <= now` vs `< now` choice |
| 🟡 Design doc | Explain lowercase column name storage decision |
| 🟡 Nice to have | `TEXT`, `PRIMARY KEY`, `NOT NULL` parsing |
| 🟡 Nice to have | Unique constraint enforcement on primary key |
