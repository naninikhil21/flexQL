#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "parsing/sql_parser.h"
#include "storage/table_store.h"
#include "storage/wal.h"

#include <string>
#include <vector>

class Executor {
public:
    explicit Executor(Catalog& catalog, WriteAheadLog& wal);

    struct Result {
        bool ok = false;
        std::string error;
        std::vector<std::string> col_names;
        std::vector<std::vector<std::string>> rows;
    };

    Result execute(const Statement& stmt);

private:
    Result exec_create(const CreateTableStmt& stmt);
    Result exec_drop(const DropTableStmt& stmt);
    Result exec_insert(const InsertStmt& stmt);
    Result exec_select(const SelectStmt& stmt);

    bool encode_value(const Value& v, const ColDesc& col, char* row_buf, std::string& err);
    std::string decode_column(const ColDesc& col, const char* row_buf) const;
    bool eval_where(const WhereExpr& w, const Schema& schema, const char* row_buf) const;

    const ColDesc* find_col(const Schema& schema, const std::string& col_name) const;

    Catalog& _catalog;
    WriteAheadLog& _wal;
};

#endif
