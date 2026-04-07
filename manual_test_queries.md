# FlexQL Manual Verification Test Suite

Since the server is already running, you can connect to it using the REPL client in a new terminal:
```bash
./build/flexql_client 127.0.0.1 9000
```

Once connected (you should see the `flexql>` prompt), copy and paste each of the SQL statements below.

---

### 1. Basic Table Creation & Insertion
**Query:**
```sql
CREATE TABLE DEPARTMENTS(ID DECIMAL PRIMARY KEY, NAME VARCHAR(32));
INSERT INTO DEPARTMENTS VALUES (1, 'Engineering');
INSERT INTO DEPARTMENTS VALUES (2, 'Sales');
SELECT * FROM DEPARTMENTS;
```

**Expected Output:**
```
ID = 1 | NAME = Engineering
ID = 2 | NAME = Sales
OK
```

---

### 2. Employees Table & More Inserts
**Query:**
```sql
CREATE TABLE EMPLOYEES(ID DECIMAL PRIMARY KEY, DEPT_ID DECIMAL, NAME VARCHAR(32), SALARY DECIMAL);
INSERT INTO EMPLOYEES VALUES (101, 1, 'Alice', 120000);
INSERT INTO EMPLOYEES VALUES (102, 1, 'Bob', 95000);
INSERT INTO EMPLOYEES VALUES (103, 2, 'Charlie', 80000);
INSERT INTO EMPLOYEES VALUES (104, 2, 'Diana', 110000);
SELECT NAME, SALARY FROM EMPLOYEES;
```

**Expected Output:**
```
NAME = Alice | SALARY = 120000
NAME = Bob | SALARY = 95000
NAME = Charlie | SALARY = 80000
NAME = Diana | SALARY = 110000
OK
```

---

### 3. Testing `WHERE` with `AND` & `OR` Chains
**Query:**
```sql
SELECT NAME FROM EMPLOYEES WHERE SALARY > 90000 AND DEPT_ID = 1;
```

**Expected Output:**
```
NAME = Alice
NAME = Bob
OK
```

**Query:**
```sql
SELECT NAME FROM EMPLOYEES WHERE SALARY > 115000 OR DEPT_ID = 2;
```

**Expected Output:**
```
NAME = Alice
NAME = Charlie
NAME = Diana
OK
```

---

### 4. Testing Aggregation: `COUNT(*) AS Alias`
**Query:**
```sql
SELECT COUNT(*) AS total_employees FROM EMPLOYEES;
```

**Expected Output:**
```
total_employees = 4
OK
```

---

### 5. Testing INNER JOIN with Table Aliases
**Query:**
```sql
SELECT E.NAME, D.NAME AS department_name
FROM EMPLOYEES E
INNER JOIN DEPARTMENTS D ON E.DEPT_ID = D.ID
WHERE E.SALARY >= 100000;
```

**Expected Output:**
```
NAME = Alice | department_name = Engineering
NAME = Diana | department_name = Sales
OK
```

---

### 6. Rejecting Unsupported Clauses (Expected Error)
**Query:**
```sql
SELECT * FROM EMPLOYEES ORDER BY SALARY;
```

**Expected Output:**
```
Error: unexpected tokens at end of query
```

---

### 7. Clean up and Exit
**Query:**
```sql
DROP TABLE DEPARTMENTS;
DROP TABLE EMPLOYEES;
.exit
```

**Expected Output:**
```
OK
OK
Bye!
```
