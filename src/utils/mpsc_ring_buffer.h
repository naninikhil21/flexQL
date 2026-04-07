#ifndef MPSC_RING_BUFFER_H
#define MPSC_RING_BUFFER_H

#include <atomic>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

// Cache line size on modern x86 processors is 64 bytes
constexpr size_t CACHE_LINE_SIZE = 64;

template<typename T>
class MPSCRingBuffer {
public:
    explicit MPSCRingBuffer(size_t capacity)
        : _capacity(capacity), _mask(capacity - 1), _buffer(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("Capacity cannot be zero.");
        }
        if ((capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be a power of two.");
        }

        for (size_t i = 0; i < _capacity; ++i) {
            _buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // Non-copyable and non-movable
    MPSCRingBuffer(const MPSCRingBuffer&) = delete;
    MPSCRingBuffer& operator=(const MPSCRingBuffer&) = delete;

    // Multi-producer push. Returns false if full.
    bool try_push(T&& item) {
        size_t pos = _enqueue_pos.load(std::memory_order_relaxed);

        for (;;) {
            Cell& cell = _buffer[pos & _mask];
            size_t seq = cell.sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (dif == 0) {
                if (_enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell.value = std::move(item);
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = _enqueue_pos.load(std::memory_order_relaxed);
            }
        }
    }

    // Single-consumer pop. Returns false if empty.
    bool try_pop(T& item) {
        size_t pos = _dequeue_pos.load(std::memory_order_relaxed);
        Cell& cell = _buffer[pos & _mask];
        size_t seq = cell.sequence.load(std::memory_order_acquire);
        intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

        if (dif == 0) {
            _dequeue_pos.store(pos + 1, std::memory_order_relaxed);
            item = std::move(cell.value);
            cell.sequence.store(pos + _capacity, std::memory_order_release);
            return true;
        }

        return false;
    }

private:
    struct Cell {
        std::atomic<size_t> sequence;
        T value;
    };

    const size_t _capacity;
    const size_t _mask;
    std::vector<Cell> _buffer;

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> _enqueue_pos{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> _dequeue_pos{0};
};

#endif // MPSC_RING_BUFFER_H
