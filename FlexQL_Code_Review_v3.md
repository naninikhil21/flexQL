# FlexQL Project — Code Review v3

---

## 1. Validation Report Status

The latest run (`scale_sql_clause_report_20260331_094755.json`, 20,000 rows) shows **all 23 tests passing**:

| Test | Result |
|---|---|
| COUNT(*) AS CNT | ✅ PASS |
| WHERE with AND range | ✅ PASS |
| WHERE with OR chain | ✅ PASS |
| Projection query (SELECT ID, NAME) | ✅ PASS |
| Float literal predicate (BALANCE >= 1000.0) | ✅ PASS |
| DROP TABLE IF EXISTS | ✅ PASS |
| CREATE TABLE | ✅ PASS |
| INSERT multi-row | ✅ PASS |
| JOIN with table aliases | ✅ PASS |
| Duplicate key reject | ✅ PASS |
| DROP TABLE cleanup | ✅ PASS |
| ORDER BY unsupported (expected fail) | ✅ PASS |
| GROUP BY unsupported (expected fail) | ✅ PASS |
| HAVING unsupported (expected fail) | ✅ PASS |
| DISTINCT unsupported (expected fail) | ✅ PASS |
| LIKE unsupported (expected fail) | ✅ PASS |
| IN unsupported (expected fail) | ✅ PASS |
| BETWEEN unsupported (expected fail) | ✅ PASS |
| LIMIT/OFFSET unsupported (expected fail) | ✅ PASS |
| Subquery unsupported (expected fail) | ✅ PASS |
| IS NOT NULL unsupported (expected fail) | ✅ PASS |
| CASE unsupported (expected fail) | ✅ PASS |
| MIN/MAX unsupported (expected fail) | ✅ PASS |

**Benchmark throughput at 20k rows: ~1,428,571 rows/sec (14 ms)**

---

## 2. What Was Fixed From v2 ✅

Every issue flagged in the v2 review has been addressed:

| v2 Issue | Fix in v3 |
|---|---|
| `COUNT(*) AS alias` not parsed — validation script fails | ✅ Fixed — `AS` added to lexer keywords; `count_alias` field in `SelectStmt`; executor uses alias as column name |
| `pk_col_index` lost on WAL replay/restart | ✅ Fixed — `pk_col_index` serialised as `uint32_t` (sentinel `0xFFFFFFFF` = -1) in WAL CREATE record; read back on replay |
| HashIndex `find()` dib probe off-by-one (wrong base) | ✅ Fixed — `find()` now starts probe counter at `dib = 1` to match inserted entries |
| REPL missing `.exit` command | ✅ Fixed — `.exit` added to quit handler |
| REPL no multi-line query support | ✅ Fixed — `accumulated` buffer collects lines until `;` is found; `...>` continuation prompt shown |
| Column `AS alias` not applied in SELECT projection | ✅ Fixed — `column_alias` field on `ColumnRef`; executor uses alias if set |

---

## 3. Overall Feature Status ✅

| Requirement | Status |
|---|---|
| `CREATE TABLE` (INT, DECIMAL, VARCHAR, TEXT, DATETIME, FLOAT) | ✅ |
| `INSERT INTO VALUES (…)` — single and multi-row batch | ✅ |
| `SELECT *` and `SELECT col1, col2` | ✅ |
| `SELECT col AS alias` | ✅ |
| `SELECT COUNT(*) AS alias` | ✅ |
| `WHERE` clause with `=, <, >, <=, >=` | ✅ |
| `WHERE` with AND chains | ✅ |
| `WHERE` with OR chains | ✅ |
| `INNER JOIN … ON` with implicit table aliases | ✅ |
| `DROP TABLE IF EXISTS` | ✅ |
| Primary key hash index (configurable column via `PRIMARY KEY`) | ✅ |
| LRU query cache with per-table invalidation | ✅ |
| Configurable cache size via `FLEXQL_CACHE_SIZE` | ✅ |
| Multithreaded server (thread-per-client) | ✅ |
| WAL persistence and crash recovery | ✅ |
| WAL checkpoint compaction | ✅ |
| `pk_col_index` preserved across WAL replay | ✅ |
| Expiration timestamp (`expires_at`) with background purge | ✅ |
| All 4 required client APIs (`open`, `close`, `exec`, `free`) | ✅ |
| Opaque `FlexQL` struct | ✅ |
| Interactive REPL client (`flexql_client`) | ✅ |
| Multi-line query accumulation in REPL | ✅ |
| `.exit`, `quit`, `\q` to exit REPL | ✅ |
| Unsupported clauses (ORDER BY, GROUP BY, etc.) correctly rejected | ✅ |

---

## 4. Remaining Bugs 🐛

### 4.1 Explicit `FROM table AS alias` silently breaks parsing — 🟠 Medium

Implicit table aliases work correctly: `FROM EMPLOYEES E` → `from_alias = "E"`.

However, when the `AS` keyword is placed explicitly before the alias — `FROM EMPLOYEES AS E` — the code breaks. Here is why:

```cpp
// sql_parser.cpp — FROM alias parsing
if (peek().type == TokenType::IDENTIFIER &&    // ← This is the problem
    !flexql_util::ieq_ascii(peek().text, "INNER") &&
    !flexql_util::ieq_ascii(peek().text, "WHERE")) {
    if (flexql_util::ieq_ascii(peek().text, "AS")) {  // ← dead code: AS is KEYWORD type
        next();
    }
    ...
}
```

`AS` was added to the lexer's keyword list, so its token type is `KEYWORD`, not `IDENTIFIER`. The outer `peek().type == TokenType::IDENTIFIER` guard evaluates to **false**, the block is never entered, and `AS` is left in the token stream. The new `parse()` end-of-input check then fires: `"unexpected tokens at end of query"`.

The same issue affects `INNER JOIN table AS alias`.

**Fix:** Change the outer check to accept both IDENTIFIER and KEYWORD:

```cpp
// In the FROM alias block:
Token t = peek();
bool is_alias_candidate =
    (t.type == TokenType::IDENTIFIER ||
     (t.type == TokenType::KEYWORD && flexql_util::ieq_ascii(t.text, "AS")));

if (is_alias_candidate &&
    !flexql_util::ieq_ascii(t.text, "INNER") &&
    !flexql_util::ieq_ascii(t.text, "WHERE") &&
    !flexql_util::ieq_ascii(t.text, "ON")) {
    if (accept_kw("AS")) { /* consume explicit AS */ }
    auto alias = parse_identifier("expected table alias");
    if (alias) { stmt.from_alias = *alias; }
}
```

This bug does **not** affect the current test suite or validation script (all tests use implicit aliases like `FROM TABLE ALIAS`), but it will break any user or grader query written in the more standard `FROM table AS t` form.

---

### 4.2 WAL `pk_col_index` upgrade path corrupts replay of pre-v3 WAL files — 🟡 Low

The new WAL CREATE record appends a `uint32_t pk_col_index` at the end. The replay code reads it unconditionally with `read_full`:

```cpp
// wal.cpp replay
unsigned char pk_buf[4];
if (read_full(fd, pk_buf, sizeof(pk_buf))) {
    // interprets as pk_col_index
} else {
    schema.pk_col_index = -1;  // "legacy" fallback
}
```

The problem: `read_full` only returns `false` on EOF or an I/O error. If a **pre-v3 WAL file** exists (written without `pk_col_index`), the 4 bytes immediately after the column list belong to the **next record's magic number** (`0xF1E4D802`). `read_full` succeeds, consumes those bytes, and misinterprets the magic as a `pk_col_index` value. The next record's magic is now gone, and replay terminates early — causing data loss for anything after the first CREATE record in the WAL.

In practice, this is only a concern at the first server start after upgrading from v2. Because `checkpoint_from_catalog` runs immediately after `replay()` on startup, the WAL is rewritten in the new format right away. The risk window is narrow, but the data loss scenario is real.

**Fix:** Add a WAL format version byte at the start of the file, or embed the total CREATE record length as a frame header so the reader can skip unknown trailing fields.

---

### 4.3 `eval_where` ignores `table_alias` in single-table WHERE — 🟡 Low

```cpp
// executor.cpp
bool Executor::eval_where(const WhereExpr& w, const Schema& schema, const char* row_buf) const {
    const ColDesc* c = find_col(schema, w.lhs.column_name);  // table_alias ignored
```

For a query like `SELECT * FROM EMPLOYEES E WHERE E.SALARY > 100000`, the WHERE clause column ref has `table_alias = "E"` and `column_name = "SALARY"`. `eval_where` calls `find_col` with only the column name, ignoring the alias. This works correctly when there is no ambiguity, but will silently match the wrong column if two joined tables both have a column with the same name.

The `eval_join_term` function (used for JOIN queries) correctly calls `resolve_col` which respects `table_alias`. The inconsistency means single-table WHERE is slightly less safe than JOIN WHERE. No current test exposes this, but it is worth noting.

---

### 4.4 AND/OR mixed chain WHERE logic still incorrect — 🟡 Low (spec only requires 1 condition)

The `eval_single_where_chain` logic is unchanged from v1. For a pure AND chain or a pure OR chain it is correct, and all validation tests use one of those two forms. However, a mixed expression like `A OR B AND C` still evaluates left-to-right as `(A OR B) AND C` instead of the standard `A OR (B AND C)`. Since the spec states only one WHERE condition is required, this does not affect grading.

---

## 5. Minor Code Quality Notes

| Location | Note |
|---|---|
| `sql_parser.cpp` | The `INNER` and `WHERE` exclusion checks inside the FROM alias block are dead code — those tokens are type `KEYWORD`, so the outer `IDENTIFIER` guard prevents the block from ever being entered with those values. They can be removed for clarity. |
| `sql_parser.cpp` | `parse_identifier` accepts both `IDENTIFIER` and `KEYWORD` tokens. A table alias that is also a keyword (e.g., `FROM T WHERE`) would be consumed as an alias. This is benign given the outer type guard, but worth documenting. |
| `wal.cpp` | Duplicate helper functions `append_u32_le_local` and `append_u64_le_local` exist as both `static` free functions and as static member functions (`append_u32_le`, `append_u64_le`). The members are never called outside the class. Consolidate to one set. |
| `flexql_client.cpp` | The `accumulated` buffer is never cleared on a server-side error, so the next command is appended to the previous failed query's text. Add `accumulated.clear()` inside the error branch. |
| `implementation_deep_dive.txt` | The design document still describes SQLite as the "active durable storage" path, which is outdated. It should be updated to reflect the custom WAL + row store implementation. |

---

## 6. Summary

The project is in excellent shape. All 23 validation tests pass, throughput is strong, and every critical bug from the previous two reviews has been resolved. The two remaining items worth acting on before final submission are:

**Priority 1 — Fix the explicit `AS` table alias bug (§4.1).** It does not affect current tests but will fail on any standard SQL query that uses `FROM table AS t` syntax, which a grader may reasonably try.

**Priority 2 — Update `implementation_deep_dive.txt` (§5).** The design document is a required deliverable and currently describes an outdated SQLite-backed architecture rather than the actual custom engine. This could cost marks on the documentation component.

Everything else is low-risk and does not affect the benchmark or validation outcome.
