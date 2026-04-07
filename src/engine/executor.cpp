#include "engine/executor.h"
#include "utils/string_util.h"

#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace {

int64_t now_epoch_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool ieq_ascii(std::string_view a, std::string_view b) {
    return flexql_util::ieq_ascii(a, b);
}

const ColDesc* find_expires_col(const Schema& schema) {
    for (const auto& c : schema.columns) {
        if (ieq_ascii(c.name, "expires_at") &&
            (c.type == ColDesc::Type::DECIMAL || c.type == ColDesc::Type::INT || c.type == ColDesc::Type::DATETIME)) {
            return &c;
        }
    }
    return nullptr;
}

bool row_is_expired(const ColDesc* expires_col, const char* row, int64_t now_s) {
    if (!expires_col) {
        return false;
    }
    int64_t exp = 0;
    std::memcpy(&exp, row + expires_col->offset, sizeof(exp));
    return exp < now_s;
}

bool is_integral_col(ColDesc::Type t) {
    return t == ColDesc::Type::DECIMAL || t == ColDesc::Type::INT || t == ColDesc::Type::DATETIME;
}

bool is_float_col(ColDesc::Type t) {
    return t == ColDesc::Type::FLOAT;
}

std::string format_float(double v) {
    std::ostringstream oss;
    oss << std::setprecision(15) << v;
    return oss.str();
}

bool cmp_num(double lhs, double rhs, WhereExpr::Op op) {
    switch (op) {
        case WhereExpr::Op::EQ:
            return lhs == rhs;
        case WhereExpr::Op::LT:
            return lhs < rhs;
        case WhereExpr::Op::GT:
            return lhs > rhs;
        case WhereExpr::Op::LE:
            return lhs <= rhs;
        case WhereExpr::Op::GE:
            return lhs >= rhs;
    }
    return false;
}

} // namespace

Executor::Executor(Catalog& catalog, WriteAheadLog& wal) : _catalog(catalog), _wal(wal) {}

Executor::Result Executor::execute(const Statement& stmt) {
    return std::visit(
        [this](const auto& s) -> Result {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, CreateTableStmt>) {
                return exec_create(s);
            } else if constexpr (std::is_same_v<T, DropTableStmt>) {
                return exec_drop(s);
            } else if constexpr (std::is_same_v<T, InsertStmt>) {
                return exec_insert(s);
            } else {
                return exec_select(s);
            }
        },
        stmt
    );
}

Executor::Result Executor::exec_create(const CreateTableStmt& stmt) {
    Schema schema;
    schema.table_name = stmt.table_name;

    size_t off = 0;
    for (const auto& c : stmt.columns) {
        ColDesc d;
        d.name = c.name;
        if (c.type == ColumnDef::Type::DECIMAL) {
            d.type = ColDesc::Type::DECIMAL;
        } else if (c.type == ColumnDef::Type::INT) {
            d.type = ColDesc::Type::INT;
        } else if (c.type == ColumnDef::Type::DATETIME) {
            d.type = ColDesc::Type::DATETIME;
        } else if (c.type == ColumnDef::Type::FLOAT) {
            d.type = ColDesc::Type::FLOAT;
        } else {
            d.type = ColDesc::Type::VARCHAR;
        }
        d.varchar_len = c.varchar_len;
        d.offset = off;
        if (d.type == ColDesc::Type::VARCHAR) {
            d.size = static_cast<size_t>(d.varchar_len + 1);
        } else if (d.type == ColDesc::Type::FLOAT) {
            d.size = sizeof(double);
        } else {
            d.size = sizeof(int64_t);
        }
        off += d.size;
        schema.columns.push_back(d);
    }
    schema.row_size = off;

    // Determine which column is the primary key
    schema.pk_col_index = -1; // default: will fall back to column 0 in RowStore
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
        if (stmt.columns[i].is_primary_key) {
            schema.pk_col_index = static_cast<int>(i);
            break;
        }
    }

    Schema wal_schema = schema;

    std::string err = _catalog.create_table(std::move(schema));
    if (!err.empty()) {
        return {false, err, {}, {}};
    }
    _wal.append_create_table(wal_schema);
    return {true, "", {}, {}};
}

Executor::Result Executor::exec_drop(const DropTableStmt& stmt) {
    std::string err = _catalog.drop_table(stmt.table_name, stmt.if_exists);
    if (!err.empty()) {
        return {false, err, {}, {}};
    }
    _wal.append_drop_table(stmt.table_name);
    return {true, "", {}, {}};
}

bool Executor::encode_value(const Value& v, const ColDesc& col, char* row_buf, std::string& err) {
    char* slot = row_buf + col.offset;

    if (is_integral_col(col.type)) {
        if (v.kind == Value::Kind::NULL_VAL) {
            int64_t z = 0;
            std::memcpy(slot, &z, sizeof(z));
            return true;
        }
        if (v.kind != Value::Kind::INTEGER) {
            err = "type mismatch: expected integer value";
            return false;
        }
        std::memcpy(slot, &v.int_val, sizeof(v.int_val));
        return true;
    }

    if (is_float_col(col.type)) {
        if (v.kind == Value::Kind::NULL_VAL) {
            double z = 0.0;
            std::memcpy(slot, &z, sizeof(z));
            return true;
        }
        double x = 0.0;
        if (v.kind == Value::Kind::INTEGER) {
            x = static_cast<double>(v.int_val);
        } else if (v.kind == Value::Kind::FLOAT) {
            x = v.float_val;
        } else {
            err = "type mismatch: expected numeric value";
            return false;
        }
        std::memcpy(slot, &x, sizeof(x));
        return true;
    }

    if (v.kind == Value::Kind::NULL_VAL) {
        slot[0] = '\0';
        return true;
    }

    if (v.kind != Value::Kind::STRING) {
        err = "type mismatch: expected VARCHAR";
        return false;
    }

    size_t maxlen = col.size > 0 ? col.size - 1 : 0;
    size_t n = v.str_val.size();
    if (n > maxlen) {
        n = maxlen;
    }
    std::memset(slot, 0, col.size);
    if (n > 0) {
        std::memcpy(slot, v.str_val.data(), n);
    }
    slot[n] = '\0';
    return true;
}

std::string Executor::decode_column(const ColDesc& col, const char* row_buf) const {
    const char* slot = row_buf + col.offset;
    if (is_integral_col(col.type)) {
        int64_t x = 0;
        std::memcpy(&x, slot, sizeof(x));
        return std::to_string(x);
    }

    if (is_float_col(col.type)) {
        double x = 0.0;
        std::memcpy(&x, slot, sizeof(x));
        return format_float(x);
    }

    size_t n = 0;
    while (n < col.size && slot[n] != '\0') {
        ++n;
    }
    return std::string(slot, n);
}

const ColDesc* Executor::find_col(const Schema& schema, const std::string& col_name) const {
    for (const auto& c : schema.columns) {
        if (ieq_ascii(c.name, col_name)) {
            return &c;
        }
    }
    return nullptr;
}

bool Executor::eval_where(const WhereExpr& w, const Schema& schema, const char* row_buf) const {
    // Validate table alias if present
    if (!w.lhs.table_alias.empty() &&
        !ieq_ascii(w.lhs.table_alias, schema.table_name)) {
        return false;
    }
    const ColDesc* c = find_col(schema, w.lhs.column_name);
    if (!c) {
        return false;
    }

    if (is_integral_col(c->type)) {
        int64_t lhs = 0;
        std::memcpy(&lhs, row_buf + c->offset, sizeof(lhs));

        if (w.rhs.kind == Value::Kind::INTEGER) {
            return cmp_num(static_cast<double>(lhs), static_cast<double>(w.rhs.int_val), w.op);
        }
        if (w.rhs.kind == Value::Kind::FLOAT) {
            return cmp_num(static_cast<double>(lhs), w.rhs.float_val, w.op);
        }
        return false;
    }

    if (is_float_col(c->type)) {
        double lhs = 0.0;
        std::memcpy(&lhs, row_buf + c->offset, sizeof(lhs));
        if (w.rhs.kind == Value::Kind::INTEGER) {
            return cmp_num(lhs, static_cast<double>(w.rhs.int_val), w.op);
        }
        if (w.rhs.kind == Value::Kind::FLOAT) {
            return cmp_num(lhs, w.rhs.float_val, w.op);
        }
        return false;
    }

    std::string lhs = decode_column(*c, row_buf);
    if (w.rhs.kind != Value::Kind::STRING) {
        return false;
    }
    const std::string& rhs = w.rhs.str_val;

    switch (w.op) {
        case WhereExpr::Op::EQ:
            return lhs == rhs;
        case WhereExpr::Op::LT:
            return lhs < rhs;
        case WhereExpr::Op::GT:
            return lhs > rhs;
        case WhereExpr::Op::LE:
            return lhs <= rhs;
        case WhereExpr::Op::GE:
            return lhs >= rhs;
    }

    return false;
}

Executor::Result Executor::exec_insert(const InsertStmt& stmt) {
    RowStore* table = _catalog.get_table(stmt.table_name);
    if (!table) {
        return {false, "no such table: " + stmt.table_name, {}, {}};
    }

    const Schema& schema = table->schema();
    const size_t row_size = schema.row_size;

    if (stmt.rows.empty()) {
        return {true, "", {}, {}};
    }

    std::vector<char> batch_buf(row_size * stmt.rows.size());
    char* ptr = batch_buf.data();

    for (const auto& row_vals : stmt.rows) {
        if (row_vals.size() != schema.columns.size()) {
            return {false, "column count mismatch", {}, {}};
        }

        std::memset(ptr, 0, row_size);
        for (size_t c = 0; c < schema.columns.size(); ++c) {
            std::string err;
            if (!encode_value(row_vals[c], schema.columns[c], ptr, err)) {
                return {false, err, {}, {}};
            }
        }
        ptr += row_size;
    }

    std::string append_err;
    if (!table->append_rows_checked(batch_buf.data(), stmt.rows.size(), append_err)) {
        return {false, append_err, {}, {}};
    }
    _wal.append_batch(stmt.table_name, batch_buf.data(), stmt.rows.size(), row_size);
    return {true, "", {}, {}};
}

Executor::Result Executor::exec_select(const SelectStmt& stmt) {
    RowStore* left = _catalog.get_table(stmt.from_table);
    if (!left) {
        return {false, "no such table: " + stmt.from_table, {}, {}};
    }

    Executor::Result out;
    out.ok = true;

    const Schema& ls = left->schema();
    const ColDesc* left_expires = find_expires_col(ls);
    const int64_t now_s = now_epoch_seconds();

    auto eval_single_where_chain = [&](const char* row) -> bool {
        if (stmt.where_terms.empty()) {
            return true;
        }
        auto eval_term = [&](size_t idx) -> bool {
            return eval_where(stmt.where_terms[idx], ls, row);
        };

        bool group = eval_term(0);
        bool accum = false;
        for (size_t i = 0; i < stmt.where_ops.size(); ++i) {
            bool rhs = eval_term(i + 1);
            if (stmt.where_ops[i] == WhereLogicOp::AND) {
                group = group && rhs;
            } else {
                accum = accum || group;
                group = rhs;
            }
        }
        return accum || group;
    };

    // Handle COUNT(*) queries
    if (stmt.is_count_star) {
        size_t count = 0;
        if (stmt.join_table.empty()) {
            left->scan_while([&](const char* row) {
                if (row_is_expired(left_expires, row, now_s)) {
                    return true;
                }
                if (!eval_single_where_chain(row)) {
                    return true;
                }
                ++count;
                return true;
            });
        } else {
            // COUNT(*) with JOIN is unusual but handle it
            RowStore* right = _catalog.get_table(stmt.join_table);
            if (!right) {
                return {false, "no such table: " + stmt.join_table, {}, {}};
            }
            // For simplicity, return error for COUNT(*) with JOIN
            return {false, "COUNT(*) with JOIN is not supported", {}, {}};
        }
        out.col_names.push_back(stmt.count_alias.empty() ? "COUNT(*)" : stmt.count_alias);
        out.rows.push_back({std::to_string(count)});
        return out;
    }

    auto project_single = [&](const char* row) {
        std::vector<std::string> vals;
        if (stmt.columns.empty()) {
            if (out.col_names.empty()) {
                for (const auto& c : ls.columns) {
                    out.col_names.push_back(c.name);
                }
            }
            vals.reserve(ls.columns.size());
            for (const auto& c : ls.columns) {
                vals.push_back(decode_column(c, row));
            }
        } else {
            if (out.col_names.empty()) {
                for (const auto& cr : stmt.columns) {
                    const ColDesc* col = find_col(ls, cr.column_name);
                    if (!col) {
                        out.ok = false;
                        out.error = "unknown column: " + cr.column_name;
                        return;
                    }
                    out.col_names.push_back(
                        cr.column_alias.empty() ? col->name : cr.column_alias
                    );
                }
            }
            vals.reserve(stmt.columns.size());
            for (const auto& cr : stmt.columns) {
                const ColDesc* col = find_col(ls, cr.column_name);
                if (!col) {
                    out.ok = false;
                    out.error = "unknown column: " + cr.column_name;
                    return;
                }
                vals.push_back(decode_column(*col, row));
            }
        }
        out.rows.push_back(std::move(vals));
    };

    if (stmt.join_table.empty()) {
        if (stmt.where_terms.size() == 1 &&
            stmt.where_ops.empty() &&
            stmt.where_terms[0].op == WhereExpr::Op::EQ &&
            stmt.where_terms[0].rhs.kind == Value::Kind::INTEGER &&
            left->has_primary_index() &&
            !ls.columns.empty()) {
            // Determine which column is the PK
            int pk_idx = (ls.pk_col_index >= 0 && ls.pk_col_index < static_cast<int>(ls.columns.size()))
                         ? ls.pk_col_index : 0;
            if (ieq_ascii(stmt.where_terms[0].lhs.column_name, ls.columns[pk_idx].name)) {
            const char* row = nullptr;
            if (left->lookup_primary_key(stmt.where_terms[0].rhs.int_val, row) &&
                !row_is_expired(left_expires, row, now_s) &&
                eval_single_where_chain(row)) {
                project_single(row);
            }
            return out;
            }
        }

        left->scan_while([&](const char* row) {
            if (!out.ok) {
                return false;
            }
            if (row_is_expired(left_expires, row, now_s)) {
                return true;
            }
            if (!eval_single_where_chain(row)) {
                return true;
            }
            project_single(row);
            return out.ok;
        });
        return out;
    }

    RowStore* right = _catalog.get_table(stmt.join_table);
    if (!right) {
        return {false, "no such table: " + stmt.join_table, {}, {}};
    }

    const Schema& rs = right->schema();
    const ColDesc* right_expires = find_expires_col(rs);

    auto resolve_col = [&](const Schema& a, const Schema& b, const ColumnRef& ref, bool* in_left) -> const ColDesc* {
        if (!ref.table_alias.empty()) {
            if (ieq_ascii(ref.table_alias, ls.table_name) || (!stmt.from_alias.empty() && ieq_ascii(ref.table_alias, stmt.from_alias))) {
                if (in_left) {
                    *in_left = true;
                }
                return find_col(a, ref.column_name);
            }
            if (ieq_ascii(ref.table_alias, rs.table_name) || (!stmt.join_alias.empty() && ieq_ascii(ref.table_alias, stmt.join_alias))) {
                if (in_left) {
                    *in_left = false;
                }
                return find_col(b, ref.column_name);
            }
        }

        const ColDesc* lc = find_col(a, ref.column_name);
        const ColDesc* rc = find_col(b, ref.column_name);
        if (lc && !rc) {
            if (in_left) {
                *in_left = true;
            }
            return lc;
        }
        if (!lc && rc) {
            if (in_left) {
                *in_left = false;
            }
            return rc;
        }
        return nullptr;
    };

    bool lhs_in_left = true;
    bool rhs_in_left = false;
    const ColDesc* jl = resolve_col(ls, rs, stmt.join_lhs, &lhs_in_left);
    const ColDesc* jr = resolve_col(ls, rs, stmt.join_rhs, &rhs_in_left);
    if (!jl || !jr) {
        return {false, "unknown join column", {}, {}};
    }

    auto eval_join_term = [&](const WhereExpr& w, const char* lrow, const char* rrow) -> bool {
        bool where_in_left = true;
        const ColDesc* wc = resolve_col(ls, rs, w.lhs, &where_in_left);
        if (!wc) {
            out.ok = false;
            out.error = "unknown column in WHERE: " + w.lhs.column_name;
            return false;
        }

        const char* wrow = where_in_left ? lrow : rrow;
        if (is_integral_col(wc->type)) {
            int64_t lhs = 0;
            std::memcpy(&lhs, wrow + wc->offset, sizeof(lhs));
            if (w.rhs.kind == Value::Kind::INTEGER) {
                return cmp_num(static_cast<double>(lhs), static_cast<double>(w.rhs.int_val), w.op);
            }
            if (w.rhs.kind == Value::Kind::FLOAT) {
                return cmp_num(static_cast<double>(lhs), w.rhs.float_val, w.op);
            }
            return false;
        }

        if (is_float_col(wc->type)) {
            double lhs = 0.0;
            std::memcpy(&lhs, wrow + wc->offset, sizeof(lhs));
            if (w.rhs.kind == Value::Kind::INTEGER) {
                return cmp_num(lhs, static_cast<double>(w.rhs.int_val), w.op);
            }
            if (w.rhs.kind == Value::Kind::FLOAT) {
                return cmp_num(lhs, w.rhs.float_val, w.op);
            }
            return false;
        }

        std::string lhs_s = decode_column(*wc, wrow);
        if (w.rhs.kind != Value::Kind::STRING) {
            return false;
        }
        const std::string& rhs_s = w.rhs.str_val;
        switch (w.op) {
            case WhereExpr::Op::EQ:
                return lhs_s == rhs_s;
            case WhereExpr::Op::LT:
                return lhs_s < rhs_s;
            case WhereExpr::Op::GT:
                return lhs_s > rhs_s;
            case WhereExpr::Op::LE:
                return lhs_s <= rhs_s;
            case WhereExpr::Op::GE:
                return lhs_s >= rhs_s;
        }
        return false;
    };

    auto eval_join_where_chain = [&](const char* lrow, const char* rrow) -> bool {
        if (stmt.where_terms.empty()) {
            return true;
        }
        auto eval_term = [&](size_t idx) -> bool {
            return eval_join_term(stmt.where_terms[idx], lrow, rrow);
        };

        bool group = eval_term(0);
        if (!out.ok) {
            return false;
        }
        bool accum = false;
        for (size_t i = 0; i < stmt.where_ops.size(); ++i) {
            bool rhs = eval_term(i + 1);
            if (!out.ok) {
                return false;
            }
            if (stmt.where_ops[i] == WhereLogicOp::AND) {
                group = group && rhs;
            } else {
                accum = accum || group;
                group = rhs;
            }
        }
        return accum || group;
    };

    left->scan_while([&](const char* lrow) {
        if (!out.ok) {
            return false;
        }

        right->scan_while([&](const char* rrow) {
            if (!out.ok) {
                return false;
            }
            if (row_is_expired(left_expires, lrow, now_s) || row_is_expired(right_expires, rrow, now_s)) {
                return true;
            }

            const char* arow = lhs_in_left ? lrow : rrow;
            const char* brow = rhs_in_left ? lrow : rrow;

            const bool left_num = is_integral_col(jl->type) || is_float_col(jl->type);
            const bool right_num = is_integral_col(jr->type) || is_float_col(jr->type);
            if (left_num && right_num) {
                double lv = 0.0;
                double rv = 0.0;
                if (is_float_col(jl->type)) {
                    std::memcpy(&lv, arow + jl->offset, sizeof(lv));
                } else {
                    int64_t x = 0;
                    std::memcpy(&x, arow + jl->offset, sizeof(x));
                    lv = static_cast<double>(x);
                }
                if (is_float_col(jr->type)) {
                    std::memcpy(&rv, brow + jr->offset, sizeof(rv));
                } else {
                    int64_t x = 0;
                    std::memcpy(&x, brow + jr->offset, sizeof(x));
                    rv = static_cast<double>(x);
                }
                if (lv != rv) {
                    return true;
                }
            } else {
                std::string lval = decode_column(*jl, arow);
                std::string rval = decode_column(*jr, brow);
                if (lval != rval) {
                    return true;
                }
            }

            if (!eval_join_where_chain(lrow, rrow)) {
                return out.ok;
            }

            std::vector<std::string> vals;
            if (stmt.columns.empty()) {
                if (out.col_names.empty()) {
                    for (const auto& c : ls.columns) {
                        out.col_names.push_back(c.name);
                    }
                    for (const auto& c : rs.columns) {
                        out.col_names.push_back(c.name);
                    }
                }
                vals.reserve(ls.columns.size() + rs.columns.size());
                for (const auto& c : ls.columns) {
                    vals.push_back(decode_column(c, lrow));
                }
                for (const auto& c : rs.columns) {
                    vals.push_back(decode_column(c, rrow));
                }
            } else {
                if (out.col_names.empty()) {
                    for (const auto& cr : stmt.columns) {
                        bool in_left = true;
                        const ColDesc* c = resolve_col(ls, rs, cr, &in_left);
                        if (!c) {
                            out.ok = false;
                            out.error = "unknown column: " + cr.column_name;
                            return false;
                        }
                        out.col_names.push_back(c->name);
                    }
                }
                vals.reserve(stmt.columns.size());
                for (const auto& cr : stmt.columns) {
                    bool in_left = true;
                    const ColDesc* c = resolve_col(ls, rs, cr, &in_left);
                    if (!c) {
                        out.ok = false;
                        out.error = "unknown column: " + cr.column_name;
                        return false;
                    }
                    vals.push_back(decode_column(*c, in_left ? lrow : rrow));
                }
            }

            out.rows.push_back(std::move(vals));
            return true;
        });

        return out.ok;
    });

    return out;
}
