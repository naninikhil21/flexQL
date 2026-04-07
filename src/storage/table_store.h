#ifndef TABLE_STORE_H
#define TABLE_STORE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ColDesc {
    std::string name;
    enum class Type { DECIMAL, VARCHAR, INT, DATETIME, FLOAT } type;
    int varchar_len = 0;
    size_t offset = 0;
    size_t size = 0;
};

struct Schema {
    std::string table_name;
    std::vector<ColDesc> columns;
    size_t row_size = 0;
    int pk_col_index = -1;  // -1 means default to column 0
};

class RowStore {
public:
    explicit RowStore(Schema schema);

    size_t append_row(const char* row_buf);
    size_t append_rows(const char* rows_buf, size_t row_count);
    void scan(const std::function<void(const char*)>& visitor) const;
    void scan_while(const std::function<bool(const char*)>& visitor) const;

    const Schema& schema() const;
    size_t row_count() const;
    const char* raw_data() const;
    bool has_primary_index() const;
    bool lookup_primary_key(int64_t key, const char*& row_out) const;
    bool append_rows_checked(const char* rows_buf, size_t row_count, std::string& err);
    size_t purge_expired(int64_t now_epoch_seconds);

private:
    void ensure_capacity_for_next_row();
    void ensure_capacity_for_rows(size_t additional_rows);
    bool validate_primary_keys(const char* rows_buf, size_t row_count, std::string& err) const;
    void index_row_at(size_t row_idx);
    void rebuild_primary_index();

    Schema _schema;
    std::vector<char> _slab;
    size_t _row_count = 0;
    size_t _capacity_rows = 0;
    bool _pk_index_enabled = false;
    size_t _pk_offset = 0;
    std::unordered_map<int64_t, size_t> _pk_index;
    mutable std::mutex _row_mu;  // per-table lock for write operations
    static constexpr size_t PREALLOC_ROWS = 2'000'000;
};

class Catalog {
public:
    std::string create_table(Schema schema);
    std::string drop_table(const std::string& name, bool if_exists);
    RowStore* get_table(const std::string& name);
    const RowStore* get_table_const(const std::string& name) const;
    void for_each_table(const std::function<void(const RowStore&)>& visitor) const;
    size_t purge_expired_all(int64_t now_epoch_seconds);
    void clear();

private:
    static std::string norm_name(const std::string& name);

    std::unordered_map<std::string, std::unique_ptr<RowStore>> _tables;
    mutable std::shared_mutex _mu;
};

#endif
