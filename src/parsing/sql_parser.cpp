#include "parsing/sql_parser.h"

#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include "utils/string_util.h"

namespace {

bool parse_positive_int_sv(std::string_view s, int& out) {
    if (s.empty()) {
        return false;
    }
    int v = 0;
    auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size() || v <= 0) {
        return false;
    }
    out = v;
    return true;
}

bool parse_decimal_to_i64(std::string_view s, int64_t& out) {
    if (s.empty()) {
        return false;
    }

    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') {
        neg = (s[i] == '-');
        ++i;
        if (i >= s.size()) {
            return false;
        }
    }

    int64_t val = 0;
    bool has_digit = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            has_digit = true;
            val = val * 10 + static_cast<int64_t>(c - '0');
            continue;
        }
        if (c == '.') {
            ++i;
            for (; i < s.size(); ++i) {
                char f = s[i];
                if (f < '0' || f > '9') {
                    return false;
                }
            }
            out = neg ? -val : val;
            return has_digit;
        }
        return false;
    }

    if (!has_digit) {
        return false;
    }
    out = neg ? -val : val;
    return true;
}

bool parse_decimal_to_f64(std::string_view s, double& out) {
    if (s.empty()) {
        return false;
    }
    std::string tmp(s);
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(tmp.c_str(), &end);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

} // namespace

Parser::Parser(std::string_view sql) : _lexer(sql) {}

std::string Parser::to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool Parser::ieq(std::string_view a, const char* b) {
    size_t i = 0;
    for (; b[i] != '\0'; ++i) {
        if (i >= a.size()) {
            return false;
        }
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') {
            ca = static_cast<char>(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = static_cast<char>(cb - 'a' + 'A');
        }
        if (ca != cb) {
            return false;
        }
    }
    return i == a.size();
}

Token Parser::next() {
    return _lexer.next();
}

Token Parser::peek() {
    return _lexer.peek();
}

bool Parser::accept(TokenType type) {
    if (peek().type == type) {
        _lexer.consume();
        return true;
    }
    return false;
}

bool Parser::accept_kw(const char* kw) {
    Token t = peek();
    if (t.type == TokenType::KEYWORD && ieq(t.text, kw)) {
        _lexer.consume();
        return true;
    }
    return false;
}

bool Parser::expect(TokenType type, const char* msg) {
    if (!accept(type)) {
        _error = msg;
        return false;
    }
    return true;
}

bool Parser::expect_kw(const char* kw, const char* msg) {
    if (!accept_kw(kw)) {
        _error = msg;
        return false;
    }
    return true;
}

std::optional<std::string> Parser::parse_identifier(const char* msg) {
    Token t = peek();
    if (t.type != TokenType::IDENTIFIER && t.type != TokenType::KEYWORD) {
        _error = msg;
        return std::nullopt;
    }
    _lexer.consume();
    return std::string(t.text);
}

std::optional<Value> Parser::parse_value() {
    Token t = peek();
    if (t.type == TokenType::INTEGER_LITERAL) {
        _lexer.consume();
        bool has_dot = false;
        for (char c : t.text) {
            if (c == '.') {
                has_dot = true;
                break;
            }
        }
        if (has_dot) {
            double fv = 0.0;
            if (!parse_decimal_to_f64(t.text, fv)) {
                _error = "invalid numeric literal";
                return std::nullopt;
            }
            Value v;
            v.kind = Value::Kind::FLOAT;
            v.float_val = fv;
            return v;
        }

        int64_t iv = 0;
        if (!parse_decimal_to_i64(t.text, iv)) {
            _error = "invalid numeric literal";
            return std::nullopt;
        }
        Value v;
        v.kind = Value::Kind::INTEGER;
        v.int_val = iv;
        return v;
    }
    if (t.type == TokenType::STRING_LITERAL) {
        _lexer.consume();
        Value v;
        v.kind = Value::Kind::STRING;
        v.str_val.assign(t.text.data(), t.text.size());
        return v;
    }
    if (t.type == TokenType::KEYWORD && ieq(t.text, "NULL")) {
        _lexer.consume();
        Value v;
        v.kind = Value::Kind::NULL_VAL;
        return v;
    }

    _error = "expected value";
    return std::nullopt;
}

std::optional<ColumnRef> Parser::parse_column_ref() {
    auto first = parse_identifier("expected column identifier");
    if (!first) {
        return std::nullopt;
    }

    ColumnRef ref;
    if (accept(TokenType::DOT)) {
        ref.table_alias = *first;
        auto second = parse_identifier("expected column name after '.'");
        if (!second) {
            return std::nullopt;
        }
        ref.column_name = *second;
    } else {
        ref.column_name = *first;
    }
    return ref;
}

std::optional<Statement> Parser::parse_create() {
    if (!expect_kw("CREATE", "expected CREATE")) {
        return std::nullopt;
    }
    if (!expect_kw("TABLE", "expected TABLE")) {
        return std::nullopt;
    }
    auto table = parse_identifier("expected table name");
    if (!table) {
        return std::nullopt;
    }
    if (!expect(TokenType::LPAREN, "expected '('") ) {
        return std::nullopt;
    }

    CreateTableStmt stmt;
    stmt.table_name = *table;

    while (true) {
        auto col = parse_identifier("expected column name");
        if (!col) {
            return std::nullopt;
        }

        Token tt = next();
        ColumnDef def;
        def.name = *col;
        if (tt.type == TokenType::KEYWORD && ieq(tt.text, "DECIMAL")) {
            def.type = ColumnDef::Type::DECIMAL;
        } else if (tt.type == TokenType::KEYWORD && ieq(tt.text, "INT")) {
            def.type = ColumnDef::Type::INT;
        } else if (tt.type == TokenType::KEYWORD && ieq(tt.text, "DATETIME")) {
            def.type = ColumnDef::Type::DATETIME;
        } else if (tt.type == TokenType::KEYWORD && (ieq(tt.text, "FLOAT") || ieq(tt.text, "REAL"))) {
            def.type = ColumnDef::Type::FLOAT;
        } else if (tt.type == TokenType::KEYWORD && ieq(tt.text, "TEXT")) {
            def.type = ColumnDef::Type::VARCHAR;
            def.varchar_len = 255;
        } else if (tt.type == TokenType::KEYWORD && ieq(tt.text, "VARCHAR")) {
            def.type = ColumnDef::Type::VARCHAR;
            if (!expect(TokenType::LPAREN, "expected '(' after VARCHAR")) {
                return std::nullopt;
            }
            Token n = next();
            if (n.type != TokenType::INTEGER_LITERAL) {
                _error = "expected VARCHAR length";
                return std::nullopt;
            }
            if (!parse_positive_int_sv(n.text, def.varchar_len)) {
                _error = "invalid VARCHAR length";
                return std::nullopt;
            }
            if (!expect(TokenType::RPAREN, "expected ')'")) {
                return std::nullopt;
            }
        } else {
            _error = "expected DECIMAL, INT, DATETIME, FLOAT, REAL, TEXT or VARCHAR";
            return std::nullopt;
        }

        // Accept and ignore optional column modifiers for compatibility.
        while (true) {
            if (accept_kw("PRIMARY")) {
                if (!expect_kw("KEY", "expected KEY after PRIMARY")) {
                    return std::nullopt;
                }
                def.is_primary_key = true;
                continue;
            }
            if (accept_kw("NOT")) {
                if (!expect_kw("NULL", "expected NULL after NOT")) {
                    return std::nullopt;
                }
                continue;
            }
            break;
        }

        stmt.columns.push_back(def);

        if (accept(TokenType::COMMA)) {
            continue;
        }
        break;
    }

    if (!expect(TokenType::RPAREN, "expected ')' at end of column list")) {
        return std::nullopt;
    }
    (void)accept(TokenType::SEMICOLON);
    return stmt;
}

std::optional<Statement> Parser::parse_drop() {
    if (!expect_kw("DROP", "expected DROP")) {
        return std::nullopt;
    }
    if (!expect_kw("TABLE", "expected TABLE")) {
        return std::nullopt;
    }

    bool if_exists = false;
    if (accept_kw("IF")) {
        if (!expect_kw("EXISTS", "expected EXISTS after IF")) {
            return std::nullopt;
        }
        if_exists = true;
    }

    auto table = parse_identifier("expected table name");
    if (!table) {
        return std::nullopt;
    }
    (void)accept(TokenType::SEMICOLON);

    DropTableStmt stmt;
    stmt.table_name = *table;
    stmt.if_exists = if_exists;
    return stmt;
}

std::optional<Statement> Parser::parse_insert() {
    if (!expect_kw("INSERT", "expected INSERT")) {
        return std::nullopt;
    }
    if (!expect_kw("INTO", "expected INTO")) {
        return std::nullopt;
    }
    auto table = parse_identifier("expected table name");
    if (!table) {
        return std::nullopt;
    }
    if (!expect_kw("VALUES", "expected VALUES")) {
        return std::nullopt;
    }

    InsertStmt stmt;
    stmt.table_name = *table;

    while (true) {
        if (!expect(TokenType::LPAREN, "expected '(' in VALUES")) {
            return std::nullopt;
        }

        std::vector<Value> row;
        while (true) {
            auto v = parse_value();
            if (!v) {
                return std::nullopt;
            }
            row.push_back(std::move(*v));

            if (accept(TokenType::COMMA)) {
                continue;
            }
            break;
        }

        if (!expect(TokenType::RPAREN, "expected ')' in VALUES")) {
            return std::nullopt;
        }
        stmt.rows.push_back(std::move(row));

        if (accept(TokenType::COMMA)) {
            continue;
        }
        break;
    }

    (void)accept(TokenType::SEMICOLON);
    return stmt;
}

std::optional<Statement> Parser::parse_select() {
    if (!expect_kw("SELECT", "expected SELECT")) {
        return std::nullopt;
    }

    SelectStmt stmt;

    // Detect COUNT(*) pattern
    if (peek().type == TokenType::KEYWORD && ieq(peek().text, "COUNT")) {
        _lexer.consume();
        if (!expect(TokenType::LPAREN, "expected '(' after COUNT")) {
            return std::nullopt;
        }
        if (!expect(TokenType::STAR, "expected '*' in COUNT(*)")) {
            return std::nullopt;
        }
        if (!expect(TokenType::RPAREN, "expected ')' after COUNT(*")) {
            return std::nullopt;
        }
        stmt.is_count_star = true;
        // Consume optional AS alias
        if (accept_kw("AS")) {
            auto alias = parse_identifier("expected alias after AS");
            if (!alias) {
                return std::nullopt;
            }
            stmt.count_alias = *alias;
        }
    } else if (accept(TokenType::STAR)) {
        // SELECT * => empty columns vector means all columns.
    } else {
        while (true) {
            auto c = parse_column_ref();
            if (!c) {
                return std::nullopt;
            }
            stmt.columns.push_back(std::move(*c));
            // Consume optional AS alias for column
            if (accept_kw("AS")) {
                auto alias = parse_identifier("expected alias after AS");
                if (!alias) {
                    return std::nullopt;
                }
                stmt.columns.back().column_alias = *alias;
            }
            if (accept(TokenType::COMMA)) {
                continue;
            }
            break;
        }
    }

    if (!expect_kw("FROM", "expected FROM")) {
        return std::nullopt;
    }
    auto from = parse_identifier("expected table name after FROM");
    if (!from) {
        return std::nullopt;
    }
    stmt.from_table = *from;

    // Optional table alias for FROM: explicit `AS alias` or implicit `alias`
    if (accept_kw("AS")) {
        auto alias = parse_identifier("expected alias after AS");
        if (alias) {
            stmt.from_alias = *alias;
        }
    } else if (peek().type == TokenType::IDENTIFIER &&
               !flexql_util::ieq_ascii(peek().text, "INNER") &&
               !flexql_util::ieq_ascii(peek().text, "WHERE")) {
        auto alias = parse_identifier("expected table alias");
        if (alias) {
            stmt.from_alias = *alias;
        }
    }

    if (accept_kw("INNER")) {
        if (!expect_kw("JOIN", "expected JOIN")) {
            return std::nullopt;
        }
        auto jt = parse_identifier("expected join table");
        if (!jt) {
            return std::nullopt;
        }
        stmt.join_table = *jt;

        // Optional table alias for JOIN: explicit `AS alias` or implicit `alias`
        if (accept_kw("AS")) {
            auto alias = parse_identifier("expected alias after AS");
            if (alias) {
                stmt.join_alias = *alias;
            }
        } else if (peek().type == TokenType::IDENTIFIER &&
                   !flexql_util::ieq_ascii(peek().text, "ON")) {
            auto alias = parse_identifier("expected table alias");
            if (alias) {
                stmt.join_alias = *alias;
            }
        }

        if (!expect_kw("ON", "expected ON")) {
            return std::nullopt;
        }

        auto lhs = parse_column_ref();
        if (!lhs) {
            return std::nullopt;
        }
        if (!expect(TokenType::EQ, "expected '=' in join condition")) {
            return std::nullopt;
        }
        auto rhs = parse_column_ref();
        if (!rhs) {
            return std::nullopt;
        }
        stmt.join_lhs = *lhs;
        stmt.join_rhs = *rhs;
    }

    if (accept_kw("WHERE")) {
        auto parse_predicate = [this]() -> std::optional<WhereExpr> {
            WhereExpr w;
            auto lhs = parse_column_ref();
            if (!lhs) {
                return std::nullopt;
            }
            w.lhs = *lhs;

            Token op = next();
            if (op.type == TokenType::EQ) {
                w.op = WhereExpr::Op::EQ;
            } else if (op.type == TokenType::LT) {
                w.op = WhereExpr::Op::LT;
            } else if (op.type == TokenType::GT) {
                w.op = WhereExpr::Op::GT;
            } else if (op.type == TokenType::LE) {
                w.op = WhereExpr::Op::LE;
            } else if (op.type == TokenType::GE) {
                w.op = WhereExpr::Op::GE;
            } else {
                _error = "expected WHERE operator (=, <, >, <=, >=)";
                return std::nullopt;
            }

            auto rhs = parse_value();
            if (!rhs) {
                return std::nullopt;
            }
            w.rhs = *rhs;
            return w;
        };

        auto first = parse_predicate();
        if (!first) {
            return std::nullopt;
        }
        stmt.where_terms.push_back(*first);

        while (true) {
            if (accept_kw("AND")) {
                stmt.where_ops.push_back(WhereLogicOp::AND);
            } else if (accept_kw("OR")) {
                stmt.where_ops.push_back(WhereLogicOp::OR);
            } else {
                break;
            }

            auto next_term = parse_predicate();
            if (!next_term) {
                return std::nullopt;
            }
            stmt.where_terms.push_back(*next_term);
        }
    }

    (void)accept(TokenType::SEMICOLON);
    return stmt;
}

std::optional<Statement> Parser::parse() {
    Token t = peek();
    if (t.type == TokenType::END_OF_INPUT) {
        _error = "empty SQL";
        return std::nullopt;
    }

    std::optional<Statement> stmt;
    if (t.type == TokenType::KEYWORD && ieq(t.text, "CREATE")) {
        stmt = parse_create();
    } else if (t.type == TokenType::KEYWORD && ieq(t.text, "DROP")) {
        stmt = parse_drop();
    } else if (t.type == TokenType::KEYWORD && ieq(t.text, "INSERT")) {
        stmt = parse_insert();
    } else if (t.type == TokenType::KEYWORD && ieq(t.text, "SELECT")) {
        stmt = parse_select();
    } else {
        _error = "unsupported SQL statement";
        return std::nullopt;
    }

    if (!stmt) {
        return std::nullopt;
    }

    if (peek().type != TokenType::END_OF_INPUT) {
        _error = "unexpected tokens at end of query";
        return std::nullopt;
    }

    return stmt;
}

std::string Parser::error() const {
    return _error.empty() ? "parse error" : _error;
}
