#ifndef SIMD_PARSER_H
#define SIMD_PARSER_H

#include <cstdint>
#include <string_view>
#include <vector>
#include <immintrin.h>

// A struct to hold the parsed row data.
// For this benchmark, we assume a simple schema: BIGINT, INT, INT
struct ParsedRow {
    uint64_t key;
    uint32_t val1;
    uint32_t val2;
};

namespace SIMDParser {

// Parses rows into a caller-provided buffer and returns number of parsed rows.
// This avoids per-call vector allocations on the hot path.
// consumed_bytes returns how many bytes were consumed from data.
size_t parse_batch_into(std::string_view data, ParsedRow* out_rows, size_t max_rows, size_t* consumed_bytes = nullptr);

// Parses a batch of CSV data into ParsedRow structs.
// This is a highly specialized function for the benchmark schema.
// It assumes the input is well-formed.
std::vector<ParsedRow> parse_batch(std::string_view data);

} // namespace SIMDParser

#endif // SIMD_PARSER_H
