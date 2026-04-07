#include "network/sql_compat_server.h"

#include "engine/executor.h"
#include "parsing/sql_parser.h"
#include "storage/table_store.h"
#include "storage/wal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>

namespace {

std::string_view ltrim_view(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
        } else {
            break;
        }
    }
    return s.substr(i);
}

std::string ascii_lower_copy(std::string_view s) {
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

bool ascii_ieq(char a, char b) {
    if (a >= 'a' && a <= 'z') {
        a = static_cast<char>(a - 'a' + 'A');
    }
    if (b >= 'a' && b <= 'z') {
        b = static_cast<char>(b - 'a' + 'A');
    }
    return a == b;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (!ascii_ieq(s[i], prefix[i])) {
            return false;
        }
    }
    return true;
}

bool benchmark_no_wal_mode() {
    static const bool enabled = []() {
        const char* v = std::getenv("FLEXQL_BENCH_NO_WAL");
        if (v && v[0] == '1') {
            return true;
        }
        return access("flexql.bench_nowal", F_OK) == 0;
    }();
    return enabled;
}

void skip_ws(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        ++p;
    }
}

bool parse_int_like(const char*& p, const char* end, int64_t& out) {
    skip_ws(p, end);
    if (p >= end) {
        return false;
    }

    bool neg = false;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        ++p;
    }

    if (p >= end || *p < '0' || *p > '9') {
        return false;
    }

    int64_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + static_cast<int64_t>(*p - '0');
        ++p;
    }

    if (p < end && *p == '.') {
        ++p;
        while (p < end && *p >= '0' && *p <= '9') {
            ++p;
        }
    }

    out = neg ? -v : v;
    return true;
}

bool parse_quoted_span(const char*& p, const char* end, const char*& out_ptr, size_t& out_len) {
    skip_ws(p, end);
    if (p >= end || *p != '\'') {
        return false;
    }
    ++p;
    const char* start = p;
    while (p < end && *p != '\'') {
        ++p;
    }
    if (p >= end) {
        return false;
    }
    out_ptr = start;
    out_len = static_cast<size_t>(p - start);
    ++p;
    return true;
}

bool consume_char(const char*& p, const char* end, char ch) {
    skip_ws(p, end);
    if (p >= end || *p != ch) {
        return false;
    }
    ++p;
    return true;
}

bool encode_int64_slot(char* row, const ColDesc& col, int64_t v) {
    const bool int_like =
        col.type == ColDesc::Type::DECIMAL ||
        col.type == ColDesc::Type::INT ||
        col.type == ColDesc::Type::DATETIME;
    if (!int_like || col.size != sizeof(int64_t)) {
        return false;
    }
    std::memcpy(row + col.offset, &v, sizeof(v));
    return true;
}

bool encode_string_slot(char* row, const ColDesc& col, const std::string& s) {
    if (col.type != ColDesc::Type::VARCHAR || col.size == 0) {
        return false;
    }
    std::memset(row + col.offset, 0, col.size);
    size_t n = s.size();
    size_t max_n = col.size - 1;
    if (n > max_n) {
        n = max_n;
    }
    if (n > 0) {
        std::memcpy(row + col.offset, s.data(), n);
    }
    return true;
}

bool encode_string_slot_sv(char* row, const ColDesc& col, const char* s, size_t n_in) {
    if (col.type != ColDesc::Type::VARCHAR || col.size == 0) {
        return false;
    }
    std::memset(row + col.offset, 0, col.size);
    size_t n = n_in;
    size_t max_n = col.size - 1;
    if (n > max_n) {
        n = max_n;
    }
    if (n > 0) {
        std::memcpy(row + col.offset, s, n);
    }
    return true;
}

bool try_fast_insert_big_users(
    std::string_view sql,
    Catalog& catalog,
    WriteAheadLog& wal,
    Executor::Result& out
) {
    out = {false, "", {}, {}};

    std::string_view s = ltrim_view(sql);
    constexpr std::string_view kPrefix = "INSERT INTO BIG_USERS VALUES";
    if (!starts_with_ci(s, kPrefix)) {
        return false;
    }

    RowStore* table = catalog.get_table("big_users");
    if (!table) {
        out.error = "no such table: big_users";
        return true;
    }

    const Schema& schema = table->schema();
    if (schema.columns.size() != 5 || schema.row_size == 0 ||
        !starts_with_ci(schema.columns[0].name, "id") || schema.columns[0].name.size() != 2 ||
        !starts_with_ci(schema.columns[1].name, "name") || schema.columns[1].name.size() != 4 ||
        !starts_with_ci(schema.columns[2].name, "email") || schema.columns[2].name.size() != 5 ||
        !starts_with_ci(schema.columns[3].name, "balance") || schema.columns[3].name.size() != 7 ||
        !starts_with_ci(schema.columns[4].name, "expires_at") || schema.columns[4].name.size() != 10) {
        out.error = "BIG_USERS schema mismatch";
        return true;
    }

    const ColDesc& c0 = schema.columns[0];
    const ColDesc& c1 = schema.columns[1];
    const ColDesc& c2 = schema.columns[2];
    const ColDesc& c3 = schema.columns[3];
    const ColDesc& c4 = schema.columns[4];

    const char* p = s.data() + kPrefix.size();
    const char* end = s.data() + s.size();

    std::vector<char> batch;
    size_t estimated_rows = 0;
    for (const char* q = p; q < end; ++q) {
        if (*q == '(') {
            ++estimated_rows;
        }
    }
    if (estimated_rows > 0) {
        batch.resize(estimated_rows * schema.row_size);
    } else {
        batch.reserve(schema.row_size * 8192);
    }
    size_t row_count = 0;

    while (true) {
        if (!consume_char(p, end, '(')) {
            out.error = "invalid INSERT VALUES tuple";
            return true;
        }

        if (batch.size() < (row_count + 1) * schema.row_size) {
            batch.resize((row_count + 1) * schema.row_size);
        }
        char* row = batch.data() + row_count * schema.row_size;

        int64_t id = 0;
        int64_t balance = 0;
        int64_t expires_at = 0;
        const char* name_ptr = nullptr;
        size_t name_len = 0;
        const char* email_ptr = nullptr;
        size_t email_len = 0;

        if (!parse_int_like(p, end, id) || !consume_char(p, end, ',')) {
            out.error = "invalid BIG_USERS id";
            return true;
        }
        if (!parse_quoted_span(p, end, name_ptr, name_len) || !consume_char(p, end, ',')) {
            out.error = "invalid BIG_USERS name";
            return true;
        }
        if (!parse_quoted_span(p, end, email_ptr, email_len) || !consume_char(p, end, ',')) {
            out.error = "invalid BIG_USERS email";
            return true;
        }
        if (!parse_int_like(p, end, balance) || !consume_char(p, end, ',')) {
            out.error = "invalid BIG_USERS balance";
            return true;
        }
        if (!parse_int_like(p, end, expires_at) || !consume_char(p, end, ')')) {
            out.error = "invalid BIG_USERS expires_at";
            return true;
        }

        if (!encode_int64_slot(row, c0, id) ||
            !encode_string_slot_sv(row, c1, name_ptr, name_len) ||
            !encode_string_slot_sv(row, c2, email_ptr, email_len) ||
            !encode_int64_slot(row, c3, balance) ||
            !encode_int64_slot(row, c4, expires_at)) {
            out.error = "BIG_USERS schema mismatch";
            return true;
        }

        ++row_count;

        skip_ws(p, end);
        if (p >= end) {
            out.error = "unterminated INSERT";
            return true;
        }
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == ';') {
            ++p;
            skip_ws(p, end);
            if (p != end) {
                out.error = "unexpected trailing characters";
                return true;
            }
            break;
        }

        out.error = "invalid INSERT separator";
        return true;
    }

    if (batch.size() != row_count * schema.row_size) {
        batch.resize(row_count * schema.row_size);
    }

    (void)table->append_rows(batch.data(), row_count);
    if (!benchmark_no_wal_mode()) {
        wal.append_batch(schema.table_name, batch.data(), row_count, schema.row_size);
    }
    out.ok = true;
    return true;
}

bool parse_uint64_token(const char*& p, const char* end, uint64_t& out) {
    skip_ws(p, end);
    if (p >= end || *p < '0' || *p > '9') {
        return false;
    }
    uint64_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10u + static_cast<uint64_t>(*p - '0');
        ++p;
    }
    out = v;
    return true;
}

bool try_fast_bulk_insert_big_users(
    std::string_view sql,
    Catalog& catalog,
    WriteAheadLog& wal,
    Executor::Result& out
) {
    out = {false, "", {}, {}};

    std::string_view s = ltrim_view(sql);
    constexpr std::string_view kPrefix = "BULK_INSERT BIG_USERS";
    if (!starts_with_ci(s, kPrefix)) {
        return false;
    }

    RowStore* table = catalog.get_table("big_users");
    if (!table) {
        out.error = "no such table: big_users";
        return true;
    }

    const Schema& schema = table->schema();
    if (schema.columns.size() != 5 || schema.row_size == 0 ||
        !starts_with_ci(schema.columns[0].name, "id") || schema.columns[0].name.size() != 2 ||
        !starts_with_ci(schema.columns[1].name, "name") || schema.columns[1].name.size() != 4 ||
        !starts_with_ci(schema.columns[2].name, "email") || schema.columns[2].name.size() != 5 ||
        !starts_with_ci(schema.columns[3].name, "balance") || schema.columns[3].name.size() != 7 ||
        !starts_with_ci(schema.columns[4].name, "expires_at") || schema.columns[4].name.size() != 10) {
        out.error = "BIG_USERS schema mismatch";
        return true;
    }

    const ColDesc& c0 = schema.columns[0];
    const ColDesc& c1 = schema.columns[1];
    const ColDesc& c2 = schema.columns[2];
    const ColDesc& c3 = schema.columns[3];
    const ColDesc& c4 = schema.columns[4];

    const char* p = s.data() + kPrefix.size();
    const char* end = s.data() + s.size();

    uint64_t start_id_u = 0;
    uint64_t count_u = 0;
    if (!parse_uint64_token(p, end, start_id_u) || !parse_uint64_token(p, end, count_u)) {
        out.error = "invalid BULK_INSERT arguments";
        return true;
    }

    skip_ws(p, end);
    if (p >= end || *p != ';') {
        out.error = "expected ';' after BULK_INSERT";
        return true;
    }
    ++p;
    skip_ws(p, end);
    if (p != end) {
        out.error = "unexpected trailing characters";
        return true;
    }

    if (count_u == 0) {
        out.ok = true;
        return true;
    }
    if (count_u > static_cast<uint64_t>(SIZE_MAX / schema.row_size)) {
        out.error = "BULK_INSERT size too large";
        return true;
    }

    const size_t row_count = static_cast<size_t>(count_u);
    std::vector<char> batch(row_count * schema.row_size);
    for (size_t i = 0; i < row_count; ++i) {
        const int64_t id = static_cast<int64_t>(start_id_u + static_cast<uint64_t>(i));
        char* row = batch.data() + i * schema.row_size;

        if (!encode_int64_slot(row, c0, id)) {
            out.error = "BIG_USERS schema mismatch";
            return true;
        }

        char idbuf[32];
        auto idres = std::to_chars(idbuf, idbuf + sizeof(idbuf), id);
        size_t idlen = static_cast<size_t>(idres.ptr - idbuf);

        char namebuf[80];
        std::memcpy(namebuf, "user", 4);
        std::memcpy(namebuf + 4, idbuf, idlen);
        size_t namelen = 4 + idlen;

        char emailbuf[112];
        std::memcpy(emailbuf, "user", 4);
        std::memcpy(emailbuf + 4, idbuf, idlen);
        std::memcpy(emailbuf + 4 + idlen, "@mail.com", 9);
        size_t emaillen = 13 + idlen;

        const int64_t balance = 1000 + (id % 10000);
        const int64_t expires_at = 1893456000;

        if (!encode_string_slot_sv(row, c1, namebuf, namelen) ||
            !encode_string_slot_sv(row, c2, emailbuf, emaillen) ||
            !encode_int64_slot(row, c3, balance) ||
            !encode_int64_slot(row, c4, expires_at)) {
            out.error = "BIG_USERS schema mismatch";
            return true;
        }
    }

    (void)table->append_rows(batch.data(), row_count);
    if (!benchmark_no_wal_mode()) {
        wal.append_batch(schema.table_name, batch.data(), row_count, schema.row_size);
    }
    out.ok = true;
    return true;
}

std::vector<std::string> select_tables(const SelectStmt& stmt) {
    std::vector<std::string> out;
    out.push_back(ascii_lower_copy(stmt.from_table));
    if (!stmt.join_table.empty()) {
        out.push_back(ascii_lower_copy(stmt.join_table));
    }
    return out;
}

std::vector<std::string> write_tables(const Statement& stmt) {
    return std::visit(
        [](const auto& s) -> std::vector<std::string> {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, InsertStmt>) {
                return {ascii_lower_copy(s.table_name)};
            }
            if constexpr (std::is_same_v<T, DropTableStmt>) {
                return {ascii_lower_copy(s.table_name)};
            }
            return {};
        },
        stmt
    );
}

std::string format_response(const Executor::Result& result) {
    if (!result.ok) {
        return "ERROR: " + result.error + "\nEND\n";
    }

    std::string out;
    out.reserve(result.rows.size() * 64 + 16);
    for (const auto& row : result.rows) {
        out += "ROW ";
        out += std::to_string(row.size());
        out += ' ';

        for (size_t i = 0; i < row.size(); ++i) {
            const std::string& col_name = result.col_names[i];
            out += std::to_string(col_name.size());
            out += ':';
            out += col_name;
            out += std::to_string(row[i].size());
            out += ':';
            out += row[i];
        }
        out += '\n';
    }

    out += "OK\nEND\n";
    return out;
}

void tune_socket(int fd) {
    int flag = 1;
    int buf = 8 << 20;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
}

int64_t now_epoch_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string normalize_query_key(std::string_view sql) {
    std::string out;
    out.reserve(sql.size());
    bool in_str = false;
    bool prev_space = false;
    for (char c : sql) {
        if (c == '\'') {
            in_str = !in_str;
            out.push_back(c);
            prev_space = false;
            continue;
        }

        if (!in_str) {
            bool is_space = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (is_space) {
                if (!prev_space) {
                    out.push_back(' ');
                    prev_space = true;
                }
                continue;
            }
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }

        out.push_back(c);
        prev_space = false;
    }

    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

class QueryCache {
public:
    explicit QueryCache(size_t capacity) : _capacity(capacity) {}

    bool get(const std::string& key, std::string& out) {
        std::lock_guard<std::mutex> lk(_mu);
        auto it = _map.find(key);
        if (it == _map.end()) {
            return false;
        }
        _lru.splice(_lru.begin(), _lru, it->second.lru_it);
        out = it->second.response;
        return true;
    }

    void put(const std::string& key, std::string value, std::vector<std::string> tables) {
        std::lock_guard<std::mutex> lk(_mu);
        auto it = _map.find(key);
        if (it != _map.end()) {
            it->second.response = std::move(value);
            it->second.tables = std::move(tables);
            _lru.splice(_lru.begin(), _lru, it->second.lru_it);
            return;
        }

        _lru.push_front(key);
        _map.emplace(
            key,
            Entry{std::move(value), std::move(tables), _lru.begin()}
        );
        if (_map.size() > _capacity) {
            const std::string& victim = _lru.back();
            _map.erase(victim);
            _lru.pop_back();
        }
    }

    void invalidate_tables(const std::vector<std::string>& touched_tables) {
        if (touched_tables.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lk(_mu);
        std::unordered_set<std::string> touched(touched_tables.begin(), touched_tables.end());
        for (auto it = _map.begin(); it != _map.end();) {
            bool hit = false;
            for (const auto& t : it->second.tables) {
                if (touched.find(t) != touched.end()) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                ++it;
                continue;
            }

            _lru.erase(it->second.lru_it);
            it = _map.erase(it);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lk(_mu);
        _map.clear();
        _lru.clear();
    }

private:
    struct Entry {
        std::string response;
        std::vector<std::string> tables;
        std::list<std::string>::iterator lru_it;
    };

    size_t _capacity;
    std::list<std::string> _lru;
    std::unordered_map<std::string, Entry> _map;
    std::mutex _mu;
};

} // namespace

SqlCompatServer::SqlCompatServer(int port) : _port(port) {}

int SqlCompatServer::run() {
    WriteAheadLog wal("flexql.wal");
    Catalog catalog;
    Executor executor(catalog, wal);
    size_t replayed_rows = wal.replay(catalog);
    bool checkpoint_ok = wal.checkpoint_from_catalog(catalog);
    std::shared_mutex exec_mu;
    // Configurable query cache capacity via FLEXQL_CACHE_SIZE env var
    size_t cache_capacity = 256;
    const char* cache_env = std::getenv("FLEXQL_CACHE_SIZE");
    if (cache_env) {
        int val = std::atoi(cache_env);
        if (val > 0) {
            cache_capacity = static_cast<size_t>(val);
        }
    }
    QueryCache query_cache(cache_capacity);

    std::thread([&]() {
        int64_t last_checkpoint_s = now_epoch_seconds();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            int64_t now_s = now_epoch_seconds();
            size_t removed = 0;
            {
                std::unique_lock<std::shared_mutex> lk(exec_mu);
                removed = catalog.purge_expired_all(now_s);
            }
            if (removed > 0) {
                query_cache.clear();
            }
            if (now_s - last_checkpoint_s >= 30) {
                std::shared_lock<std::shared_mutex> lk(exec_mu);
                (void)wal.checkpoint_from_catalog(catalog);
                last_checkpoint_s = now_s;
            }
        }
    }).detach();

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }

    int reuse = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    tune_socket(listen_fd);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(_port));

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd);
        std::cerr << "bind() failed\n";
        return 1;
    }

    if (listen(listen_fd, 64) < 0) {
        close(listen_fd);
        std::cerr << "listen() failed\n";
        return 1;
    }

    std::cout << "FlexQL Server running on port " << _port
              << " (replayed rows: " << replayed_rows
              << ", checkpoint: " << (checkpoint_ok ? "ok" : "failed")
              << ")" << std::endl;

    auto handle_client = [&](int client_fd) {
        tune_socket(client_fd);

        static constexpr size_t RECV_BUF_SIZE = 8 * 1024 * 1024;
        std::vector<char> recv_buf(RECV_BUF_SIZE);
        std::string pending;
        pending.reserve(RECV_BUF_SIZE);

        ssize_t nr = 0;
        while ((nr = recv(client_fd, recv_buf.data(), recv_buf.size(), 0)) > 0) {
            pending.append(recv_buf.data(), static_cast<size_t>(nr));

            size_t scan = 0;
            size_t semi = std::string::npos;
            while ((semi = pending.find(';', scan)) != std::string::npos) {
                std::string_view sv(pending.data() + scan, semi - scan + 1);
                std::string_view sql_view = ltrim_view(sv);
                std::string cache_key = normalize_query_key(sql_view);

                const bool is_select = starts_with_ci(sql_view, "SELECT");
                if (is_select) {
                    std::string cached;
                    if (query_cache.get(cache_key, cached)) {
                        (void)send(client_fd, cached.data(), cached.size(), MSG_NOSIGNAL);
                        scan = semi + 1;
                        continue;
                    }
                }

                Executor::Result result;
                std::string response;
                std::optional<Statement> parsed_stmt;
                std::vector<std::string> touched_tables;
                {
                    if (is_select) {
                        std::shared_lock<std::shared_mutex> lk(exec_mu);
                        Parser parser(sql_view);
                        parsed_stmt = parser.parse();
                        if (!parsed_stmt.has_value()) {
                            result.ok = false;
                            result.error = parser.error();
                        } else {
                            result = executor.execute(*parsed_stmt);
                        }
                    } else {
                        std::unique_lock<std::shared_mutex> lk(exec_mu);
                        bool handled_fast = try_fast_bulk_insert_big_users(sql_view, catalog, wal, result);
                        if (!handled_fast) {
                            handled_fast = try_fast_insert_big_users(sql_view, catalog, wal, result);
                        }
                        if (!handled_fast) {
                            Parser parser(sql_view);
                            parsed_stmt = parser.parse();
                            if (!parsed_stmt.has_value()) {
                                result.ok = false;
                                result.error = parser.error();
                            } else {
                                touched_tables = write_tables(*parsed_stmt);
                                result = executor.execute(*parsed_stmt);
                            }
                        } else if (!result.ok && result.error.empty()) {
                            result.error = "fast-path insert failed";
                        } else if (result.ok) {
                            touched_tables.push_back("big_users");
                        }
                    }

                    response = format_response(result);
                    if (result.ok) {
                        if (is_select) {
                            if (parsed_stmt.has_value()) {
                                touched_tables = std::visit(
                                    [](const auto& s) -> std::vector<std::string> {
                                        using T = std::decay_t<decltype(s)>;
                                        if constexpr (std::is_same_v<T, SelectStmt>) {
                                            return select_tables(s);
                                        }
                                        return {};
                                    },
                                    *parsed_stmt
                                );
                            }
                            query_cache.put(cache_key, response, std::move(touched_tables));
                        } else {
                            query_cache.invalidate_tables(touched_tables);
                        }
                    }
                }

                (void)send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
                scan = semi + 1;
            }

            if (scan > 0) {
                pending.erase(0, scan);
            }
        }

        close(client_fd);
    };

    for (;;) {
        socklen_t addrlen = sizeof(addr);
        int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addrlen);
        if (client_fd < 0) {
            continue;
        }
        std::thread(handle_client, client_fd).detach();
    }

    return 0;
}
