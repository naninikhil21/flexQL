#ifndef WAL_H
#define WAL_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <sys/uio.h>

#include "storage/table_store.h"

struct LogEntry {
    const char* data;
    size_t len;
};

class Catalog;

class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& file_path);
    ~WriteAheadLog();

    // Non-copyable
    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    void append(const std::vector<LogEntry>& entries);
    void append_create_table(const Schema& schema);
    void append_drop_table(const std::string& table_name);
    void append_batch(const std::string& table_name, const char* rows_buf, size_t row_count, size_t row_size);
    size_t replay(Catalog& catalog);
    bool checkpoint_from_catalog(const Catalog& catalog);
    void shutdown();

private:
    void run();

    const std::string _file_path;
    int _fd;

    std::deque<std::vector<char>> _pending_records;
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _stop_flag{false};
    std::thread _writer_thread;
    std::atomic<bool> _strict_durability{false};

    static constexpr uint32_t kMagicInsert = 0xF1E4D802u;
    static constexpr uint32_t kMagicCreate = 0xF1E4D803u;
    static constexpr uint32_t kMagicDrop = 0xF1E4D804u;
};

#endif // WAL_H
