#include "storage/table_store.h"
#include "utils/string_util.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string_view>

namespace {
std::string to_lower_copy(const std::string& s) {
    return flexql_util::to_lower_copy(s);
}

bool ieq_ascii(std::string_view a, std::string_view b) {
    return flexql_util::ieq_ascii(a, b);
}
} // namespace

RowStore::RowStore(Schema schema) : _schema(std::move(schema)) {
    if (_schema.row_size == 0) {
        for (auto& c : _schema.columns) {
            c.offset = _schema.row_size;
            if (c.type == ColDesc::Type::VARCHAR) {
                c.size = static_cast<size_t>(c.varchar_len + 1);
            } else if (c.type == ColDesc::Type::FLOAT) {
                c.size = sizeof(double);
            } else {
                c.size = sizeof(int64_t);
            }
            _schema.row_size += c.size;
        }
    }

    if (!_schema.columns.empty()) {
        // Determine which column to use as primary key
        int pk_idx = (_schema.pk_col_index >= 0 && _schema.pk_col_index < static_cast<int>(_schema.columns.size()))
                     ? _schema.pk_col_index : 0;
        const auto t = _schema.columns[pk_idx].type;
        if (t == ColDesc::Type::DECIMAL || t == ColDesc::Type::INT || t == ColDesc::Type::DATETIME) {
            _pk_index_enabled = true;
            _pk_offset = _schema.columns[pk_idx].offset;
            _pk_index.reserve(PREALLOC_ROWS);
        }
    }

    _capacity_rows = PREALLOC_ROWS;
    _slab.resize(_capacity_rows * _schema.row_size);
}

void RowStore::index_row_at(size_t row_idx) {
    if (!_pk_index_enabled) {
        return;
    }
    const char* row = _slab.data() + row_idx * _schema.row_size;
    int64_t key = 0;
    std::memcpy(&key, row + _pk_offset, sizeof(key));
    _pk_index[key] = row_idx;
}

void RowStore::rebuild_primary_index() {
    if (!_pk_index_enabled) {
        return;
    }
    _pk_index.clear();
    _pk_index.reserve(_row_count);
    for (size_t i = 0; i < _row_count; ++i) {
        index_row_at(i);
    }
}

void RowStore::ensure_capacity_for_next_row() {
    if (_row_count < _capacity_rows) {
        return;
    }
    size_t next_capacity = _capacity_rows * 2;
    if (next_capacity < _capacity_rows + 1) {
        next_capacity = _capacity_rows + 1;
    }
    _slab.resize(next_capacity * _schema.row_size);
    _capacity_rows = next_capacity;
}

void RowStore::ensure_capacity_for_rows(size_t additional_rows) {
    if (additional_rows == 0) {
        return;
    }
    size_t needed = _row_count + additional_rows;
    if (needed <= _capacity_rows) {
        return;
    }

    size_t next_capacity = _capacity_rows;
    while (next_capacity < needed) {
        size_t grown = next_capacity + (next_capacity >> 1);
        if (grown <= next_capacity) {
            grown = needed;
        }
        next_capacity = grown;
    }

    _slab.resize(next_capacity * _schema.row_size);
    _capacity_rows = next_capacity;
}

size_t RowStore::append_row(const char* row_buf) {
    std::lock_guard<std::mutex> lk(_row_mu);
    ensure_capacity_for_next_row();
    size_t off = _row_count * _schema.row_size;
    std::memcpy(_slab.data() + off, row_buf, _schema.row_size);
    ++_row_count;
    index_row_at(_row_count - 1);
    return off;
}

size_t RowStore::append_rows(const char* rows_buf, size_t row_count) {
    std::lock_guard<std::mutex> lk(_row_mu);
    if (row_count == 0) {
        return _row_count * _schema.row_size;
    }
    ensure_capacity_for_rows(row_count);

    size_t off = _row_count * _schema.row_size;
    const size_t bytes = row_count * _schema.row_size;
    std::memcpy(_slab.data() + off, rows_buf, bytes);
    const size_t first_idx = _row_count;
    _row_count += row_count;
    if (_pk_index_enabled) {
        _pk_index.reserve(_row_count);
        for (size_t i = 0; i < row_count; ++i) {
            index_row_at(first_idx + i);
        }
    }
    return off;
}

void RowStore::scan(const std::function<void(const char*)>& visitor) const {
    const char* base = _slab.data();
    for (size_t i = 0; i < _row_count; ++i) {
        visitor(base + i * _schema.row_size);
    }
}

void RowStore::scan_while(const std::function<bool(const char*)>& visitor) const {
    const char* base = _slab.data();
    for (size_t i = 0; i < _row_count; ++i) {
        if (!visitor(base + i * _schema.row_size)) {
            break;
        }
    }
}

const Schema& RowStore::schema() const {
    return _schema;
}

size_t RowStore::row_count() const {
    return _row_count;
}

const char* RowStore::raw_data() const {
    return _slab.data();
}

bool RowStore::has_primary_index() const {
    return _pk_index_enabled;
}

bool RowStore::lookup_primary_key(int64_t key, const char*& row_out) const {
    if (!_pk_index_enabled) {
        return false;
    }
    auto it = _pk_index.find(key);
    if (it == _pk_index.end()) {
        return false;
    }
    row_out = _slab.data() + it->second * _schema.row_size;
    return true;
}

bool RowStore::validate_primary_keys(const char* rows_buf, size_t row_count, std::string& err) const {
    if (!_pk_index_enabled || row_count == 0) {
        return true;
    }

    std::unordered_set<int64_t> batch_keys;
    batch_keys.reserve(row_count);
    for (size_t i = 0; i < row_count; ++i) {
        const char* row = rows_buf + i * _schema.row_size;
        int64_t key = 0;
        std::memcpy(&key, row + _pk_offset, sizeof(key));
        if (_pk_index.find(key) != _pk_index.end()) {
            err = "duplicate primary key";
            return false;
        }
        auto [_, inserted] = batch_keys.insert(key);
        if (!inserted) {
            err = "duplicate primary key";
            return false;
        }
    }

    return true;
}

bool RowStore::append_rows_checked(const char* rows_buf, size_t row_count, std::string& err) {
    std::lock_guard<std::mutex> lk(_row_mu);
    if (!validate_primary_keys(rows_buf, row_count, err)) {
        return false;
    }
    // Call internal append without re-locking
    if (row_count == 0) {
        return true;
    }
    ensure_capacity_for_rows(row_count);
    size_t off = _row_count * _schema.row_size;
    const size_t bytes = row_count * _schema.row_size;
    std::memcpy(_slab.data() + off, rows_buf, bytes);
    const size_t first_idx = _row_count;
    _row_count += row_count;
    if (_pk_index_enabled) {
        _pk_index.reserve(_row_count);
        for (size_t i = 0; i < row_count; ++i) {
            index_row_at(first_idx + i);
        }
    }
    return true;
}

size_t RowStore::purge_expired(int64_t now_epoch_seconds) {
    std::lock_guard<std::mutex> lk(_row_mu);
    const ColDesc* exp_col = nullptr;
    for (const auto& c : _schema.columns) {
        if (ieq_ascii(c.name, "expires_at") &&
            (c.type == ColDesc::Type::DECIMAL || c.type == ColDesc::Type::INT || c.type == ColDesc::Type::DATETIME)) {
            exp_col = &c;
            break;
        }
    }
    if (!exp_col || _row_count == 0) {
        return 0;
    }

    const size_t row_sz = _schema.row_size;
    char* base = _slab.data();
    size_t write_i = 0;
    size_t removed = 0;

    for (size_t read_i = 0; read_i < _row_count; ++read_i) {
        char* row = base + read_i * row_sz;
        int64_t exp = 0;
        std::memcpy(&exp, row + exp_col->offset, sizeof(exp));
        if (exp < now_epoch_seconds) {
            ++removed;
            continue;
        }
        if (write_i != read_i) {
            std::memmove(base + write_i * row_sz, row, row_sz);
        }
        ++write_i;
    }

    _row_count = write_i;
    if (removed > 0 && _pk_index_enabled) {
        rebuild_primary_index();
    }
    return removed;
}

std::string Catalog::norm_name(const std::string& name) {
    return to_lower_copy(name);
}

std::string Catalog::create_table(Schema schema) {
    std::unique_lock<std::shared_mutex> lk(_mu);
    schema.table_name = norm_name(schema.table_name);
    const std::string key = schema.table_name;

    if (_tables.find(key) != _tables.end()) {
        return "table already exists";
    }

    _tables[key] = std::make_unique<RowStore>(std::move(schema));
    return "";
}

std::string Catalog::drop_table(const std::string& name, bool if_exists) {
    std::unique_lock<std::shared_mutex> lk(_mu);
    std::string n = norm_name(name);
    auto it = _tables.find(n);
    if (it == _tables.end()) {
        if (if_exists) {
            return "";
        }
        return "no such table: " + name;
    }
    _tables.erase(it);
    return "";
}

RowStore* Catalog::get_table(const std::string& name) {
    std::shared_lock<std::shared_mutex> lk(_mu);
    auto it = _tables.find(norm_name(name));
    if (it == _tables.end()) {
        return nullptr;
    }
    return it->second.get();
}

const RowStore* Catalog::get_table_const(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lk(_mu);
    auto it = _tables.find(norm_name(name));
    if (it == _tables.end()) {
        return nullptr;
    }
    return it->second.get();
}

void Catalog::for_each_table(const std::function<void(const RowStore&)>& visitor) const {
    std::shared_lock<std::shared_mutex> lk(_mu);
    for (const auto& kv : _tables) {
        visitor(*kv.second);
    }
}

size_t Catalog::purge_expired_all(int64_t now_epoch_seconds) {
    std::unique_lock<std::shared_mutex> lk(_mu);
    size_t total_removed = 0;
    for (auto& kv : _tables) {
        total_removed += kv.second->purge_expired(now_epoch_seconds);
    }
    return total_removed;
}

void Catalog::clear() {
    std::unique_lock<std::shared_mutex> lk(_mu);
    _tables.clear();
}
