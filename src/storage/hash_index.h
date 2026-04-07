#ifndef HASH_INDEX_H
#define HASH_INDEX_H

#include "utils/arena_allocator.h"
#include <vector>
#include <cstdint>
#include <string_view>
#include <optional>

// A Robin Hood-style flat hash map with dynamic resize support.
// Designed to work with an arena allocator.
template<typename Key, typename T>
class HashIndex {
private:
    struct Entry {
        Key key;
        T value;
        uint32_t dib; // Distance from initial bucket (0 = empty)
    };

public:
    explicit HashIndex(size_t capacity, ArenaAllocator& allocator)
        : _capacity(capacity), _size(0), _buckets(capacity), _allocator(allocator) {}

    std::optional<T> find(const Key& key) {
        size_t pos = hash_key(key) & (_capacity - 1);
        uint32_t dib = 1;
        for (;;) {
            Entry& entry = _buckets[pos];
            if (entry.dib == 0 || dib > entry.dib) {
                return std::nullopt; // Not found
            }
            if (entry.key == key) {
                return entry.value;
            }
            pos = (pos + 1) & (_capacity - 1);
            dib++;
        }
    }

    bool insert(const Key& key, const T& value) {
        if (_size >= _capacity * 9 / 10) { // Load factor check (90%)
            resize(_capacity * 2);
        }

        return insert_internal(key, value);
    }

private:
    bool insert_internal(const Key& key, const T& value) {
        size_t pos = hash_key(key) & (_capacity - 1);
        Entry current_entry{key, value, 1}; // dib starts at 1 (occupied)

        for (;;) {
            Entry& bucket = _buckets[pos];
            if (bucket.dib == 0) { // Empty bucket
                bucket = current_entry;
                _size++;
                return true;
            }

            if (bucket.key == key) { // Key already exists
                bucket.value = value; // Update value
                return true;
            }

            // If current entry is "richer" (higher DIB), swap with the bucket
            if (current_entry.dib > bucket.dib) {
                std::swap(current_entry, bucket);
            }

            current_entry.dib++;
            pos = (pos + 1) & (_capacity - 1);
        }
    }

    void resize(size_t new_capacity) {
        // Ensure new_capacity is a power of 2
        size_t cap = 1;
        while (cap < new_capacity) {
            cap <<= 1;
        }
        new_capacity = cap;

        std::vector<Entry> old_buckets = std::move(_buckets);
        size_t old_capacity = _capacity;

        _capacity = new_capacity;
        _size = 0;
        _buckets.assign(new_capacity, Entry{});

        // Re-insert all existing entries
        for (size_t i = 0; i < old_capacity; ++i) {
            if (old_buckets[i].dib != 0) {
                insert_internal(old_buckets[i].key, old_buckets[i].value);
            }
        }
    }

    size_t hash_key(const Key& key) const {
        // Simple FNV-1a hash
        uint64_t hash = 14695981039346656037ULL;
        std::string_view view(reinterpret_cast<const char*>(&key), sizeof(Key));
        for (char c : view) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    size_t _capacity;
    size_t _size;
    std::vector<Entry> _buckets;
    ArenaAllocator& _allocator;
};

#endif // HASH_INDEX_H
