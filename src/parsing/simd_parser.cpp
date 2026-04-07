#include "parsing/simd_parser.h"

// A simple, non-SIMD integer parser for baseline.
// A full SIMD version is a project in itself.
static bool parse_int_bounded(const char*& ptr, const char* end, uint64_t& out) {
    if (ptr >= end) {
        return false;
    }

    uint64_t val = 0;
    bool has_digit = false;
    while (ptr < end && *ptr >= '0' && *ptr <= '9') {
        has_digit = true;
        val = val * 10 + (*ptr - '0');
        ptr++;
    }

    if (!has_digit) {
        return false;
    }

    if (ptr < end && (*ptr == ',' || *ptr == '\n')) {
        ptr++;
    }

    out = val;
    return true;
}

namespace SIMDParser {

size_t parse_batch_into(std::string_view data, ParsedRow* out_rows, size_t max_rows, size_t* consumed_bytes) {
    size_t count = 0;
    const char* ptr = data.data();
    const char* end = data.data() + data.size();

    while (ptr < end && count < max_rows) {
        uint64_t key = 0;
        uint64_t v1 = 0;
        uint64_t v2 = 0;

        const char* row_start = ptr;
        if (!parse_int_bounded(ptr, end, key) || !parse_int_bounded(ptr, end, v1) || !parse_int_bounded(ptr, end, v2)) {
            // Keep progress monotonic to avoid caller infinite loops on bad input.
            ptr = (row_start < end) ? row_start + 1 : row_start;
            break;
        }

        out_rows[count++] = ParsedRow{key, static_cast<uint32_t>(v1), static_cast<uint32_t>(v2)};
    }

    if (consumed_bytes) {
        *consumed_bytes = static_cast<size_t>(ptr - data.data());
    }

    return count;
}

std::vector<ParsedRow> parse_batch(std::string_view data) {
    // Compatibility wrapper for non-hot-path callers.
    std::vector<ParsedRow> rows(data.size() / 8 + 1);
    size_t parsed = parse_batch_into(data, rows.data(), rows.size(), nullptr);
    rows.resize(parsed);
    return rows;
}

} // namespace SIMDParser
