// DEAD CODE: Legacy ingestion path retained for reference; SqlCompatServer is the active server path.
// This Worker class and related Server/IOUringReactor are not reachable from main.cpp.
#include "engine/worker.h"
#include "utils/thread_affinity.h"
#include <iostream>
#include <cstring>

Worker::Worker(int id, MPSCRingBuffer<ParsedRow>& queue, WriteAheadLog& wal)
    : _id(id),
      _queue(queue),
      _wal(wal),
      _allocator(1024 * 1024 * 256), // 256MB arena
      _index(1024 * 1024, _allocator) {}

Worker::~Worker() {
    stop();
}

void Worker::start() {
    _thread = std::thread(&Worker::run, this);
}

void Worker::stop() {
    _stop.store(true);
    if (_thread.joinable()) {
        _thread.join();
    }
}

bool Worker::set_affinity(int core_id) {
    if (!_thread.joinable()) {
        return false;
    }
    return ThreadAffinity::set_thread_affinity(_thread, core_id);
}

uint64_t Worker::rows_inserted() const {
    return _rows_inserted.load(std::memory_order_relaxed);
}

void Worker::run() {
    ParsedRow row;
    std::vector<LogEntry> log_entries;
    log_entries.reserve(1024);

    while (!_stop) {
        bool processed_any = false;

        for (int i = 0; i < 256; ++i) {
            if (!_queue.try_pop(row)) {
                break;
            }
            processed_any = true;

            // For this benchmark, we just store a pointer to the key.
            // A real implementation would store the full row in the arena.
            char* data = (char*)_allocator.allocate(sizeof(row));
            if (data) {
                std::memcpy(data, &row, sizeof(row));
                _index.insert(row.key, data);
                log_entries.push_back({data, sizeof(row)});
                _rows_inserted.fetch_add(1, std::memory_order_relaxed);
            }

            if (log_entries.size() >= 1024) {
                _wal.append(log_entries);
                log_entries.clear();
            }
        }

        if (!processed_any) {
            // No work to do, maybe yield
            std::this_thread::yield();
        }
    }

    if (!log_entries.empty()) {
        _wal.append(log_entries);
    }
}
