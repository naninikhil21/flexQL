#ifndef ARENA_ALLOCATOR_H
#define ARENA_ALLOCATOR_H

#include <vector>
#include <cstddef>
#include <stdexcept>
#include <memory>

class ArenaAllocator {
public:
    explicit ArenaAllocator(size_t size_bytes)
        : _buffer(size_bytes), _current_offset(0) {
        if (size_bytes == 0) {
            throw std::invalid_argument("Arena size cannot be zero.");
        }
    }

    // Non-copyable and non-movable
    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        size_t aligned_offset = align_up(_current_offset, alignment);
        if (aligned_offset + size > _buffer.size()) {
            // Out of memory
            return nullptr;
        }
        _current_offset = aligned_offset + size;
        return _buffer.data() + aligned_offset;
    }

    void reset() {
        _current_offset = 0;
    }

    size_t used_memory() const {
        return _current_offset;
    }

    size_t capacity() const {
        return _buffer.size();
    }

private:
    static size_t align_up(size_t offset, size_t alignment) {
        return (offset + alignment - 1) & ~(alignment - 1);
    }

    std::vector<char> _buffer;
    size_t _current_offset;
};

#endif // ARENA_ALLOCATOR_H
