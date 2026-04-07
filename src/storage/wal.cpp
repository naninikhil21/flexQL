#include "storage/wal.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits.h>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace {
bool read_full(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::read(fd, p + off, len - off);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool write_full(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool writev_full(int fd, std::vector<iovec> iov) {
    while (!iov.empty()) {
        ssize_t n = ::writev(fd, iov.data(), static_cast<int>(iov.size()));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        size_t consumed = static_cast<size_t>(n);
        size_t i = 0;
        while (i < iov.size() && consumed >= iov[i].iov_len) {
            consumed -= iov[i].iov_len;
            ++i;
        }
        if (i > 0) {
            iov.erase(iov.begin(), iov.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (!iov.empty() && consumed > 0) {
            iov[0].iov_base = static_cast<char*>(iov[0].iov_base) + consumed;
            iov[0].iov_len -= consumed;
        }
    }
    return true;
}

uint32_t read_u32_le(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64_le(const unsigned char* p) {
    return static_cast<uint64_t>(p[0]) |
           (static_cast<uint64_t>(p[1]) << 8) |
           (static_cast<uint64_t>(p[2]) << 16) |
           (static_cast<uint64_t>(p[3]) << 24) |
           (static_cast<uint64_t>(p[4]) << 32) |
           (static_cast<uint64_t>(p[5]) << 40) |
           (static_cast<uint64_t>(p[6]) << 48) |
           (static_cast<uint64_t>(p[7]) << 56);
}

void append_u32_le_local(std::vector<char>& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void append_u64_le_local(std::vector<char>& out, uint64_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 32) & 0xFF));
    out.push_back(static_cast<char>((v >> 40) & 0xFF));
    out.push_back(static_cast<char>((v >> 48) & 0xFF));
    out.push_back(static_cast<char>((v >> 56) & 0xFF));
}

std::vector<char> build_create_record(const Schema& schema, uint32_t magic_create) {
    std::vector<char> rec;
    size_t total = 4 + 4 + 4 + schema.table_name.size() + 4;
    for (const auto& c : schema.columns) {
        total += 4 + c.name.size() + 1 + 4;
    }
    total += 4; // pk_col_index
    rec.reserve(total);

    append_u32_le_local(rec, magic_create);

    // Placeholder for payload length (will be backfilled)
    size_t len_pos = rec.size();
    append_u32_le_local(rec, 0);

    append_u32_le_local(rec, static_cast<uint32_t>(schema.table_name.size()));
    rec.insert(rec.end(), schema.table_name.begin(), schema.table_name.end());
    append_u32_le_local(rec, static_cast<uint32_t>(schema.columns.size()));

    for (const auto& c : schema.columns) {
        append_u32_le_local(rec, static_cast<uint32_t>(c.name.size()));
        rec.insert(rec.end(), c.name.begin(), c.name.end());
        uint8_t t = 0;
        switch (c.type) {
            case ColDesc::Type::DECIMAL:
                t = 0;
                break;
            case ColDesc::Type::VARCHAR:
                t = 1;
                break;
            case ColDesc::Type::INT:
                t = 2;
                break;
            case ColDesc::Type::DATETIME:
                t = 3;
                break;
            case ColDesc::Type::FLOAT:
                t = 4;
                break;
        }
        rec.push_back(static_cast<char>(t));
        append_u32_le_local(rec, static_cast<uint32_t>(c.varchar_len));
    }

    // Serialize pk_col_index (0xFFFFFFFF = -1 sentinel)
    uint32_t pk_idx_val = (schema.pk_col_index >= 0)
        ? static_cast<uint32_t>(schema.pk_col_index)
        : 0xFFFFFFFFu;
    append_u32_le_local(rec, pk_idx_val);

    // Backfill payload length (everything after magic + length field)
    uint32_t payload_len = static_cast<uint32_t>(rec.size() - len_pos - 4);
    rec[len_pos]     = static_cast<char>(payload_len & 0xFF);
    rec[len_pos + 1] = static_cast<char>((payload_len >> 8) & 0xFF);
    rec[len_pos + 2] = static_cast<char>((payload_len >> 16) & 0xFF);
    rec[len_pos + 3] = static_cast<char>((payload_len >> 24) & 0xFF);

    return rec;
}

std::vector<char> build_insert_header(
    const std::string& table_name,
    uint64_t row_count,
    uint64_t row_size,
    uint32_t magic_insert
) {
    std::vector<char> hdr;
    hdr.reserve(4 + 4 + table_name.size() + 8 + 8);
    append_u32_le_local(hdr, magic_insert);
    append_u32_le_local(hdr, static_cast<uint32_t>(table_name.size()));
    hdr.insert(hdr.end(), table_name.begin(), table_name.end());
    append_u64_le_local(hdr, row_count);
    append_u64_le_local(hdr, row_size);
    return hdr;
}
} // namespace

WriteAheadLog::WriteAheadLog(const std::string& file_path) : _file_path(file_path) {
    _fd = open(file_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (_fd == -1) {
        throw std::runtime_error("Failed to open WAL file.");
    }
    const char* strict_env = std::getenv("FLEXQL_STRICT_DURABILITY");
    if (strict_env && strict_env[0] == '1') {
        _strict_durability.store(true, std::memory_order_relaxed);
    }
    _writer_thread = std::thread(&WriteAheadLog::run, this);
}

WriteAheadLog::~WriteAheadLog() {
    shutdown();
    if (_fd != -1) {
        close(_fd);
    }
}

void WriteAheadLog::shutdown() {
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _stop_flag = true;
    }
    _cv.notify_one();
    if (_writer_thread.joinable()) {
        _writer_thread.join();
    }
}

void WriteAheadLog::append(const std::vector<LogEntry>& entries) {
    std::vector<char> rec;
    size_t total = 0;
    for (const auto& e : entries) {
        total += e.len;
    }
    rec.reserve(total);
    for (const auto& e : entries) {
        if (e.data && e.len > 0) {
            rec.insert(rec.end(), e.data, e.data + e.len);
        }
    }

    {
        std::unique_lock<std::mutex> lock(_mutex);
        _pending_records.push_back(std::move(rec));
    }
    _cv.notify_one();
}

void WriteAheadLog::append_create_table(const Schema& schema) {
    std::vector<char> rec = build_create_record(schema, kMagicCreate);

    {
        std::unique_lock<std::mutex> lock(_mutex);
        _pending_records.push_back(std::move(rec));
    }
    _cv.notify_one();
}

void WriteAheadLog::append_drop_table(const std::string& table_name) {
    std::vector<char> rec;
    rec.reserve(4 + 4 + table_name.size());

    append_u32_le_local(rec, kMagicDrop);
    append_u32_le_local(rec, static_cast<uint32_t>(table_name.size()));
    rec.insert(rec.end(), table_name.begin(), table_name.end());

    {
        std::unique_lock<std::mutex> lock(_mutex);
        _pending_records.push_back(std::move(rec));
    }
    _cv.notify_one();
}


void WriteAheadLog::append_batch(const std::string& table_name, const char* rows_buf, size_t row_count, size_t row_size) {
    size_t payload_len = row_count * row_size;
    const size_t header_len = 4 + 4 + table_name.size() + 8 + 8;
    std::vector<char> rec;
    rec.resize(header_len + payload_len);

    char* p = rec.data();

    const uint32_t name_len = static_cast<uint32_t>(table_name.size());
    const uint64_t rc = static_cast<uint64_t>(row_count);
    const uint64_t rs = static_cast<uint64_t>(row_size);

    p[0] = static_cast<char>(kMagicInsert & 0xFF);
    p[1] = static_cast<char>((kMagicInsert >> 8) & 0xFF);
    p[2] = static_cast<char>((kMagicInsert >> 16) & 0xFF);
    p[3] = static_cast<char>((kMagicInsert >> 24) & 0xFF);
    p += 4;

    p[0] = static_cast<char>(name_len & 0xFF);
    p[1] = static_cast<char>((name_len >> 8) & 0xFF);
    p[2] = static_cast<char>((name_len >> 16) & 0xFF);
    p[3] = static_cast<char>((name_len >> 24) & 0xFF);
    p += 4;

    if (!table_name.empty()) {
        std::memcpy(p, table_name.data(), table_name.size());
        p += table_name.size();
    }

    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<char>((rc >> (8 * i)) & 0xFF);
    }
    p += 8;
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<char>((rs >> (8 * i)) & 0xFF);
    }
    p += 8;

    if (rows_buf && payload_len > 0) {
        std::memcpy(p, rows_buf, payload_len);
    }

    {
        std::unique_lock<std::mutex> lock(_mutex);
        _pending_records.push_back(std::move(rec));
    }
    _cv.notify_one();
}

size_t WriteAheadLog::replay(Catalog& catalog) {
    int fd = open(_file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    size_t replayed = 0;
    while (true) {
        unsigned char hdr[4];
        ssize_t n = ::read(fd, hdr, sizeof(hdr));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n != 4) {
            break;
        }

        uint32_t magic = read_u32_le(hdr);
        if (magic == kMagicCreate) {
            // Try to read payload length (v4 format)
            unsigned char lenbuf[4];
            uint32_t payload_len = 0;
            bool has_payload_len = false;
            off_t pos_before_payload = lseek(fd, 0, SEEK_CUR);

            if (read_full(fd, lenbuf, sizeof(lenbuf))) {
                uint32_t candidate = read_u32_le(lenbuf);
                // Heuristic: a valid payload length must be reasonable (< 1MB)
                // and not look like a magic number. Legacy v3 format starts with
                // table_name_length which is also a u32, but will be very small.
                // We detect v4 by checking if the candidate payload_len, when
                // added to position, lands us at a valid record boundary.
                // For simplicity: if candidate > 1MB, it's likely not a length.
                if (candidate > 0 && candidate < (1u << 20)) {
                    has_payload_len = true;
                    payload_len = candidate;
                } else {
                    // Legacy format: candidate is actually table_name_length.
                    // Seek back to re-read it as table_name_length.
                    lseek(fd, pos_before_payload, SEEK_SET);
                }
            } else {
                break; // EOF
            }

            off_t payload_start = lseek(fd, 0, SEEK_CUR);

            unsigned char nbuf[4];
            if (!read_full(fd, nbuf, sizeof(nbuf))) {
                break;
            }
            uint32_t table_len = read_u32_le(nbuf);
            if (table_len == 0 || table_len > (1u << 20)) {
                break;
            }

            Schema schema;
            schema.table_name.resize(table_len);
            if (!read_full(fd, schema.table_name.data(), table_len)) {
                break;
            }

            if (!read_full(fd, nbuf, sizeof(nbuf))) {
                break;
            }
            uint32_t col_count = read_u32_le(nbuf);
            if (col_count == 0 || col_count > 1024) {
                break;
            }

            schema.columns.reserve(col_count);
            size_t off = 0;
            for (uint32_t i = 0; i < col_count; ++i) {
                if (!read_full(fd, nbuf, sizeof(nbuf))) {
                    col_count = 0;
                    break;
                }
                uint32_t name_len = read_u32_le(nbuf);
                if (name_len == 0 || name_len > (1u << 20)) {
                    col_count = 0;
                    break;
                }

                std::string col_name(name_len, '\0');
                if (!read_full(fd, col_name.data(), name_len)) {
                    col_count = 0;
                    break;
                }

                unsigned char tbuf[1];
                if (!read_full(fd, tbuf, sizeof(tbuf))) {
                    col_count = 0;
                    break;
                }

                if (!read_full(fd, nbuf, sizeof(nbuf))) {
                    col_count = 0;
                    break;
                }
                uint32_t varchar_len = read_u32_le(nbuf);

                ColDesc col;
                col.name = std::move(col_name);
                if (tbuf[0] == 0) {
                    col.type = ColDesc::Type::DECIMAL;
                } else if (tbuf[0] == 1) {
                    col.type = ColDesc::Type::VARCHAR;
                } else if (tbuf[0] == 2) {
                    col.type = ColDesc::Type::INT;
                } else if (tbuf[0] == 3) {
                    col.type = ColDesc::Type::DATETIME;
                } else if (tbuf[0] == 4) {
                    col.type = ColDesc::Type::FLOAT;
                } else {
                    col_count = 0;
                    break;
                }
                col.varchar_len = static_cast<int>(varchar_len);
                col.offset = off;
                if (col.type == ColDesc::Type::VARCHAR) {
                    col.size = static_cast<size_t>(col.varchar_len + 1);
                } else if (col.type == ColDesc::Type::FLOAT) {
                    col.size = sizeof(double);
                } else {
                    col.size = sizeof(int64_t);
                }
                off += col.size;
                schema.columns.push_back(std::move(col));
            }

            if (schema.columns.size() != col_count) {
                break;
            }

            schema.row_size = off;

            // Read pk_col_index if payload length indicates extra bytes
            if (has_payload_len) {
                off_t payload_consumed = lseek(fd, 0, SEEK_CUR) - payload_start;
                int64_t remaining = static_cast<int64_t>(payload_len) - payload_consumed;
                if (remaining >= 4) {
                    unsigned char pk_buf[4];
                    if (read_full(fd, pk_buf, sizeof(pk_buf))) {
                        uint32_t pk_idx_val = read_u32_le(pk_buf);
                        schema.pk_col_index = (pk_idx_val == 0xFFFFFFFFu)
                            ? -1
                            : static_cast<int>(pk_idx_val);
                    }
                    remaining -= 4;
                }
                // Skip any future unknown fields
                if (remaining > 0) {
                    lseek(fd, remaining, SEEK_CUR);
                }
            } else {
                // Legacy v3 format: try to read pk_col_index, but use lseek
                // to peek without consuming bytes from the next record
                off_t cur = lseek(fd, 0, SEEK_CUR);
                unsigned char pk_buf[4];
                if (read_full(fd, pk_buf, sizeof(pk_buf))) {
                    uint32_t pk_idx_val = read_u32_le(pk_buf);
                    // Validate: if this looks like a valid pk_col_index
                    // (either 0xFFFFFFFF or a small column index), keep it.
                    // If it looks like a magic number, seek back.
                    if (pk_idx_val == 0xFFFFFFFFu ||
                        pk_idx_val < static_cast<uint32_t>(schema.columns.size())) {
                        schema.pk_col_index = (pk_idx_val == 0xFFFFFFFFu)
                            ? -1
                            : static_cast<int>(pk_idx_val);
                    } else {
                        // Likely a magic number from next record; seek back
                        lseek(fd, cur, SEEK_SET);
                        schema.pk_col_index = -1;
                    }
                } else {
                    schema.pk_col_index = -1;
                }
            }

            (void)catalog.create_table(std::move(schema));
            continue;
        }

        if (magic == kMagicDrop) {
            unsigned char nbuf[4];
            if (!read_full(fd, nbuf, sizeof(nbuf))) {
                break;
            }
            uint32_t table_len = read_u32_le(nbuf);
            if (table_len == 0 || table_len > (1u << 20)) {
                break;
            }

            std::string table(table_len, '\0');
            if (!read_full(fd, table.data(), table_len)) {
                break;
            }
            (void)catalog.drop_table(table, true);
            continue;
        }

        if (magic == kMagicInsert) {
            unsigned char nbuf[4];
            if (!read_full(fd, nbuf, sizeof(nbuf))) {
                break;
            }
            uint32_t name_len = read_u32_le(nbuf);
            if (name_len == 0 || name_len > (1u << 20)) {
                break;
            }

            std::string table(name_len, '\0');
            if (!read_full(fd, table.data(), name_len)) {
                break;
            }

            unsigned char rb[8];
            if (!read_full(fd, rb, sizeof(rb))) {
                break;
            }
            uint64_t row_count = read_u64_le(rb);

            if (!read_full(fd, rb, sizeof(rb))) {
                break;
            }
            uint64_t row_size = read_u64_le(rb);

            if (row_size == 0 || row_count > (1ull << 32)) {
                break;
            }

            uint64_t data_len = row_count * row_size;
            std::vector<char> data;
            data.resize(static_cast<size_t>(data_len));
            if (data_len > 0 && !read_full(fd, data.data(), static_cast<size_t>(data_len))) {
                break;
            }

            RowStore* table_store = catalog.get_table(table);
            if (!table_store || table_store->schema().row_size != static_cast<size_t>(row_size)) {
                continue;
            }

            std::string pk_err;
            if (table_store->append_rows_checked(data.data(), static_cast<size_t>(row_count), pk_err)) {
                replayed += static_cast<size_t>(row_count);
            }
            // Silently ignore duplicate PK errors for idempotent replay
            continue;
        }

        break;
    }

    close(fd);
    return replayed;
}

bool WriteAheadLog::checkpoint_from_catalog(const Catalog& catalog) {
    shutdown();
    if (_fd != -1) {
        close(_fd);
        _fd = -1;
    }

    const std::string tmp_path = _file_path + ".tmp";
    int tfd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tfd < 0) {
        _fd = open(_file_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (_fd >= 0) {
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _stop_flag = false;
            }
            _writer_thread = std::thread(&WriteAheadLog::run, this);
        }
        return false;
    }

    bool ok = true;
    catalog.for_each_table([&](const RowStore& table) {
        if (!ok) {
            return;
        }

        const Schema& schema = table.schema();
        std::vector<char> cre = build_create_record(schema, kMagicCreate);
        if (!write_full(tfd, cre.data(), cre.size())) {
            ok = false;
            return;
        }

        const size_t row_count = table.row_count();
        const size_t row_size = schema.row_size;
        if (row_count == 0 || row_size == 0) {
            return;
        }

        static constexpr size_t kChunkRows = 100000;
        const char* base = table.raw_data();
        size_t off_rows = 0;
        while (off_rows < row_count) {
            const size_t chunk_rows = std::min(kChunkRows, row_count - off_rows);
            std::vector<char> hdr = build_insert_header(
                schema.table_name,
                static_cast<uint64_t>(chunk_rows),
                static_cast<uint64_t>(row_size),
                kMagicInsert
            );
            if (!write_full(tfd, hdr.data(), hdr.size())) {
                ok = false;
                return;
            }

            const char* chunk_ptr = base + off_rows * row_size;
            const size_t chunk_bytes = chunk_rows * row_size;
            if (!write_full(tfd, chunk_ptr, chunk_bytes)) {
                ok = false;
                return;
            }
            off_rows += chunk_rows;
        }
    });

    if (ok && fsync(tfd) != 0) {
        ok = false;
    }
    close(tfd);

    if (ok && rename(tmp_path.c_str(), _file_path.c_str()) != 0) {
        ok = false;
    }

    // Clean up stale .tmp file on failure
    if (!ok) {
        (void)unlink(tmp_path.c_str());
    }

    _fd = open(_file_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (_fd < 0) {
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(_mutex);
        _stop_flag = false;
    }
    _writer_thread = std::thread(&WriteAheadLog::run, this);
    return ok;
}

void WriteAheadLog::run() {
    std::deque<std::vector<char>> pending;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] { return _stop_flag || !_pending_records.empty(); });

            if (_stop_flag && _pending_records.empty()) {
                break;
            }
            pending.swap(_pending_records);
        }

        if (!pending.empty()) {
            constexpr size_t kFallbackMaxIov = 1024;
            const size_t kMaxIov = (IOV_MAX > 0) ? static_cast<size_t>(IOV_MAX) : kFallbackMaxIov;
            bool ok = true;

            size_t i = 0;
            while (i < pending.size()) {
                std::vector<iovec> iov;
                iov.reserve(std::min(kMaxIov, pending.size() - i));
                size_t used = 0;
                while (i < pending.size() && used < kMaxIov) {
                    auto& rec = pending[i];
                    if (!rec.empty()) {
                        iovec v;
                        v.iov_base = rec.data();
                        v.iov_len = rec.size();
                        iov.push_back(v);
                        ++used;
                    }
                    ++i;
                }

                if (!iov.empty() && !writev_full(_fd, std::move(iov))) {
                    ok = false;
                    break;
                }
            }

            if (!ok) {
                std::cerr << "Error writing to WAL" << std::endl;
            }
            if (_strict_durability.load(std::memory_order_relaxed)) {
                (void)fdatasync(_fd);
            }
            pending.clear();
        }
    }
}
