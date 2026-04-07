# FlexQL — Compilation and Execution Instructions

## Prerequisites

| Dependency       | Required Version | Installation (Ubuntu/Debian)                     |
|------------------|------------------|--------------------------------------------------|
| GCC / Clang      | C++17 or later   | `sudo apt install build-essential`               |
| CMake            | ≥ 3.16           | `sudo apt install cmake`                         |
| liburing         | ≥ 0.7            | `sudo apt install liburing-dev`                  |
| Python 3         | ≥ 3.8            | `sudo apt install python3` *(validation only)*   |

---

## 1. Building the Project

All binaries are built in Release mode using CMake.

```bash
# From the project root directory:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Or use the provided convenience script:

```bash
chmod +x compile.sh
./compile.sh
```

After a successful build, the following binaries are available:

| Binary                  | Description                                |
|-------------------------|--------------------------------------------|
| `./build/flexql_server` | The FlexQL database server (port 9000)     |
| `./build/flexql_client` | Interactive REPL client                    |
| `./benchmark`           | Insertion benchmark + unit test suite      |

---

## 2. Running the Server

```bash
# Start the server (listens on TCP port 9000 by default)
./build/flexql_server
```

Expected startup output:
```
FlexQL Server running on port 9000 (replayed rows: <N>, checkpoint: ok)
```

The server replays any existing WAL file on startup, restoring all tables and data automatically.

**Optional environment variable:**

```bash
# Set the query cache size (default: 64 entries)
FLEXQL_CACHE_SIZE=128 ./build/flexql_server
```

---

## 3. Connecting via the REPL Client

In a separate terminal, while the server is running:

```bash
./build/flexql_client 127.0.0.1 9000
```

You will see:
```
Connected to FlexQL server at 127.0.0.1:9000
Type SQL queries ending with ';'. Type '.exit' or 'quit' to exit.

flexql>
```

**Multi-line queries** are supported — keep typing and press Enter; the `...>` prompt appears until a `;` is found.

**Exit commands:** `.exit`, `quit`, `exit`, `\q`

---

## 4. Supported SQL Commands

```sql
-- Create a table (any column can be PRIMARY KEY)
CREATE TABLE USERS (ID DECIMAL PRIMARY KEY, NAME VARCHAR(64), SCORE FLOAT);

-- Insert a single row
INSERT INTO USERS VALUES (1, 'Alice', 98.5);

-- Insert multiple rows in one statement
INSERT INTO USERS VALUES (2, 'Bob', 75.0), (3, 'Charlie', 88.3);

-- Select all rows
SELECT * FROM USERS;

-- Select specific columns with aliases
SELECT NAME AS username, SCORE AS pts FROM USERS;

-- Aggregation
SELECT COUNT(*) AS total FROM USERS;

-- WHERE clause (=, <, >, <=, >=) with AND / OR chains
SELECT NAME FROM USERS WHERE SCORE >= 80.0 AND ID < 10;
SELECT NAME FROM USERS WHERE SCORE > 90.0 OR NAME = 'Bob';

-- INNER JOIN with table aliases (implicit or explicit AS)
CREATE TABLE ORDERS (ORDER_ID DECIMAL PRIMARY KEY, USER_ID DECIMAL, AMOUNT DECIMAL);
INSERT INTO ORDERS VALUES (101, 1, 500), (102, 2, 300);

SELECT U.NAME, O.AMOUNT
FROM USERS U
INNER JOIN ORDERS O ON U.ID = O.USER_ID;

-- With explicit AS:
SELECT U.NAME, O.AMOUNT
FROM USERS AS U
INNER JOIN ORDERS AS O ON U.ID = O.USER_ID;

-- Drop a table
DROP TABLE IF EXISTS USERS;
```

---

## 5. Running the Benchmark

The benchmark inserts 1,000,000 rows into a `BIG_USERS` table and also runs 23 unit tests.  
**The server must be running first.**

```bash
# Terminal 1 — start server
./build/flexql_server

# Terminal 2 — run benchmark
./benchmark 1000000
```

You can pass any row count as the argument:

```bash
./benchmark 500000    # 500K rows
./benchmark 2000000   # 2M rows
```

---

## 6. Running the SQL Clause Validation Suite

The Python validation script starts its own server internally, runs all SQL clause tests, and saves a JSON report.

```bash
# Make sure no other server is running first
python3 scale_sql_clause_validation.py --rows 20000
```

Reports are saved under `validation_reports/` as timestamped JSON files.

---

## 7. Connecting via the C Client Library

Link `flexql.cpp` and `flexql.h` with your own program:

```c
#include "flexql.h"

FlexQL* db = NULL;
flexql_open("127.0.0.1", 9000, &db);

char* errmsg = NULL;
flexql_exec(db, "SELECT * FROM USERS;", my_callback, NULL, &errmsg);
if (errmsg) {
    fprintf(stderr, "Error: %s\n", errmsg);
    flexql_free(errmsg);
}
flexql_close(db);
```

API summary:

| Function          | Description                                            |
|-------------------|--------------------------------------------------------|
| `flexql_open()`   | Connect to a running FlexQL server                     |
| `flexql_exec()`   | Execute SQL; row callback invoked for each result row  |
| `flexql_close()`  | Disconnect                                             |
| `flexql_free()`   | Free an error string returned by `flexql_exec()`       |

Return codes: `FLEXQL_OK` (0) on success, `FLEXQL_ERROR` (1) on failure.
