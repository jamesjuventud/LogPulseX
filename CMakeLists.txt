cmake_minimum_required(VERSION 3.16)
project(LogPulseX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep artifacts for each configuration in a predictable subdirectory of
# the build tree (for example, build/Debug and build/Release).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")

add_library(logpulsex INTERFACE)
target_include_directories(logpulsex INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Expose the header-only library files in IDE project trees. These are source
# entries only; they do not change compilation or create a separate library.
file(GLOB_RECURSE LOGPULSEX_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/logpulsex/*.hpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include"
             PREFIX "Header Files"
             FILES ${LOGPULSEX_HEADERS})
target_sources(logpulsex INTERFACE ${LOGPULSEX_HEADERS})
target_compile_features(logpulsex INTERFACE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(logpulsex INTERFACE Threads::Threads)

# NetworkSink (and the test suite's socket_compat helper) use Winsock2 on
# Windows, which requires linking ws2_32.lib explicitly -- MSVC does not
# auto-link it. The #pragma comment(lib, "ws2_32.lib") in
# socket_compat.hpp covers non-CMake MSVC builds (e.g. a raw cl.exe
# invocation); this covers CMake-based builds (including MinGW, which
# ignores that pragma) uniformly.
if(WIN32)
    target_link_libraries(logpulsex INTERFACE ws2_32)
endif()

# Optional gzip compression support for DailyFileSink
# (compress_after_rotation=true), via zlib. OFF by default -- this
# library has zero mandatory third-party dependencies, and stays that
# way unless you explicitly opt in here. When OFF, requesting
# compress_after_rotation=true at runtime throws std::invalid_argument
# immediately rather than silently not compressing.
option(LOGPULSEX_ENABLE_GZIP "Enable gzip compression support (requires zlib)" OFF)
if(LOGPULSEX_ENABLE_GZIP)
    find_package(ZLIB REQUIRED)
    target_link_libraries(logpulsex INTERFACE ZLIB::ZLIB)
    target_compile_definitions(logpulsex INTERFACE LOGPULSEX_HAVE_ZLIB)
endif()

# --- Recommended warning / hardening flags --------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(logpulsex INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=2 -O2>
        $<$<CONFIG:Debug>:-fsanitize=address,undefined -g>
    )
    target_link_options(logpulsex INTERFACE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(logpulsex INTERFACE /W4 /permissive-)
endif()

# Set LOGPULSEX_MIN_LEVEL to strip lower-severity log calls from Release
# builds entirely (0=trace ... 5=fatal, 6=off).
target_compile_definitions(logpulsex INTERFACE
    $<$<CONFIG:Release>:LOGPULSEX_MIN_LEVEL=1>
)

option(LOGPULSEX_BUILD_EXAMPLES "Build example programs" ON)
if(LOGPULSEX_BUILD_EXAMPLES)
    add_executable(logpulsex_example examples/main.cpp)
    target_link_libraries(logpulsex_example PRIVATE logpulsex)
endif()

option(LOGPULSEX_BUILD_TESTS "Build unit tests" ON)
if(LOGPULSEX_BUILD_TESTS)
    enable_testing()
    add_executable(logpulsex_tests tests/test_main.cpp)
    target_link_libraries(logpulsex_tests PRIVATE logpulsex)
    add_test(NAME logpulsex_tests COMMAND logpulsex_tests)

    # Network sink integration test: spins up a real local TCP server via
    # tests/tcp_test_server.py to validate connect/send/reconnect/backlog
    # behavior end-to-end. Separated from the main sanitized suite because
    # it forks subprocesses and does real (loopback-only) networking.
    add_executable(logpulsex_network_test tests/test_network_sink.cpp)
    target_link_libraries(logpulsex_network_test PRIVATE logpulsex)
    # Tells the test the absolute location of tcp_test_server.py at
    # compile time, so it can find the script regardless of whatever
    # working directory the build system or IDE chooses at runtime.
    # This matters in practice: MSVC/Visual Studio's default CMake debug
    # working directory is the build output directory (e.g.
    # out/build/x64-Debug/logpulsex/), not the project root -- without this,
    # the test's relative-path fallbacks never resolve there.
    target_compile_definitions(logpulsex_network_test PRIVATE
        LOGPULSEX_TESTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME logpulsex_network_test COMMAND logpulsex_network_test
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Regression test for the LOGPULSEX_MIN_LEVEL vs. Level::raw interaction
    # bug (see tests/test_min_level_raw.cpp): built with a nonzero
    # LOGPULSEX_MIN_LEVEL, matching how this same CMakeLists.txt sets
    # LOGPULSEX_MIN_LEVEL=1 for Release above, to prove LOG_RAW survives
    # compile-time stripping while trace/debug are still correctly gated.
    add_executable(logpulsex_min_level_test tests/test_min_level_raw.cpp)
    target_link_libraries(logpulsex_min_level_test PRIVATE logpulsex)
    target_compile_definitions(logpulsex_min_level_test PRIVATE LOGPULSEX_MIN_LEVEL=2)
    add_test(NAME logpulsex_min_level_test COMMAND logpulsex_min_level_test)
endif()

# Performance benchmark suite (bench/): custom zero-dependency harness,
# off by default since it's not needed for normal builds/CI correctness
# checks. See README.md's "Benchmarking" section for usage.
option(LOGPULSEX_BUILD_BENCHMARKS "Build performance benchmark suite" OFF)
if(LOGPULSEX_BUILD_BENCHMARKS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(WARNING
            "LOGPULSEX_BUILD_BENCHMARKS is ON but CMAKE_BUILD_TYPE is not "
            "'Release' -- the Debug config enables -fsanitize=address,undefined "
            "(see the GNU|Clang compile options above), which will badly skew "
            "benchmark timings. Re-run CMake with -DCMAKE_BUILD_TYPE=Release.")
    endif()
    add_executable(logpulsex_bench
        bench/bench_main.cpp
        bench/micro_bench.cpp
        bench/throughput_bench.cpp
        bench/multithread_bench.cpp
        bench/sink_io_bench.cpp)
    target_link_libraries(logpulsex_bench PRIVATE logpulsex)
endif()
