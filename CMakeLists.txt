cmake_minimum_required(VERSION 3.22)
project(FlexQL CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Aggressive optimizations for release builds
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto=auto -fomit-frame-pointer -funroll-loops -fno-plt -ffast-math")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

# Add source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(flexql_server ${SOURCES})
target_compile_options(flexql_server PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native>
)

# Set include directory
target_include_directories(flexql_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Find and link liburing for io_uring support
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)
if (LIBURING_FOUND)
    target_include_directories(flexql_server PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(flexql_server PRIVATE ${LIBURING_LIBRARIES})
    message(STATUS "Found and linked liburing")
else()
    message(FATAL_ERROR "liburing not found. Please install it (e.g., sudo apt-get install liburing-dev).")
endif()

# Link against pthread
find_package(Threads REQUIRED)
target_link_libraries(flexql_server PRIVATE Threads::Threads)

message(STATUS "FlexQL server configured (no SQLite)")

# Client REPL binary
add_executable(flexql_client
    ${CMAKE_CURRENT_SOURCE_DIR}/flexql_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/flexql.cpp
)
target_include_directories(flexql_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(flexql_client PRIVATE Threads::Threads)
message(STATUS "FlexQL client REPL configured")
