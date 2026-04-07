#!/usr/bin/env bash
set -e

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Benchmark binary (uses the C client library)
g++ -std=c++17 flexql.cpp benchmark_flexql.cpp -O2 -I. -o build/benchmark -lpthread

echo "Build complete:"
echo "  build/flexql_server  — database server"
echo "  build/flexql_client  — interactive REPL client"
echo "  build/benchmark      — benchmark + unit test suite"