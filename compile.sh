cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
cmake --build build --target flexql_server -j"$(nproc)" && \
g++ flexql.cpp benchmark_flexql.cpp -O2 -o benchmark