#ifndef SQL_PARSER_H
#define SQL_PARSER_H

#include "parsing/sql_lexer.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct ColumnDef {
    std::string name;
    enum class Type { DECIMAL, VARCHAR, INT, DATETIME, FLOAT } type;
    int varchar_len = 0;
    bool is_primary_key = false;
};

struct CreateTableStmt {
    std::string table_name;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt {
    std::string table_name;
    bool if_exists = true;
};

struct Value {
    enum class Kind { INTEGER, FLOAT, STRING, NULL_VAL } kind;
    int64_t int_val = 0;
    double float_val = 0.0;
    std::string str_val;
};

struct InsertStmt {
    std::string table_name;
    std::vector<std::vector<Value>> rows;
};

struct ColumnRef {
    std::string table_alias;
    std::string column_name;
    std::string column_alias;  // AS alias
};

struct WhereExpr {
    ColumnRef lhs;
    enum class Op { EQ, LT, GT, LE, GE } op;
    Value rhs;
};

enum class WhereLogicOp { AND, OR };

struct SelectStmt {
    std::vector<ColumnRef> columns;
    std::string from_table;
    std::string from_alias;
    std::string join_table;
    std::string join_alias;
    ColumnRef join_lhs;
    ColumnRef join_rhs;
    std::vector<WhereExpr> where_terms;
    std::vector<WhereLogicOp> where_ops;
    bool is_count_star = false;
    std::string count_alias;  // alias from COUNT(*) AS <alias>
};

using Statement = std::variant<CreateTableStmt, DropTableStmt, InsertStmt, SelectStmt>;

class Parser {
public:
    explicit Parser(std::string_view sql);

    std::optional<Statement> parse();
    std::string error() const;

private:
    static std::string to_lower(std::string_view s);
    static bool ieq(std::string_view a, const char* b);

    bool accept(TokenType type);
    bool accept_kw(const char* kw);
    bool expect(TokenType type, const char* msg);
    bool expect_kw(const char* kw, const char* msg);

    std::optional<std::string> parse_identifier(const char* msg);
    std::optional<Value> parse_value();
    std::optional<ColumnRef> parse_column_ref();

    std::optional<Statement> parse_create();
    std::optional<Statement> parse_drop();
    std::optional<Statement> parse_insert();
    std::optional<Statement> parse_select();

    Token next();
    Token peek();

    Lexer _lexer;
    std::string _error;
};

#endif
