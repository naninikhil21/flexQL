#ifndef WORKER_H
#define WORKER_H

#include "utils/mpsc_ring_buffer.h"
#include "parsing/simd_parser.h"
#include "storage/hash_index.h"
#include "storage/wal.h"
#include <thread>
#include <atomic>

class Worker {
public:
    Worker(int id, MPSCRingBuffer<ParsedRow>& queue, WriteAheadLog& wal);
    ~Worker();

    void start();
    void stop();
    bool set_affinity(int core_id);
    uint64_t rows_inserted() const;

private:
    void run();

    int _id;
    MPSCRingBuffer<ParsedRow>& _queue;
    WriteAheadLog& _wal;
    std::thread _thread;
    std::atomic<bool> _stop{false};
    std::atomic<uint64_t> _rows_inserted{0};

    ArenaAllocator _allocator;
    HashIndex<uint64_t, const char*> _index;
};

#endif // WORKER_H
