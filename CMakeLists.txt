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

# Find liburing for io_uring support (optional — only used in legacy reactor path)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(LIBURING liburing)
endif()

if (LIBURING_FOUND)
    target_include_directories(flexql_server PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(flexql_server PRIVATE ${LIBURING_LIBRARIES})
    target_compile_definitions(flexql_server PRIVATE HAVE_LIBURING)
    message(STATUS "Found and linked liburing")
else()
    message(STATUS "liburing not found — building without io_uring support (active SQL path unaffected)")
    message(STATUS "  To enable: sudo apt install liburing-dev")
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
