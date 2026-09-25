cmake_minimum_required(VERSION 3.20)

# x86-64 only. On an Apple Silicon Mac, build x86_64 and run under Rosetta 2
# (executes SSE/AVX2, not AVX-512). This must be set before project().
if(APPLE)
    execute_process(COMMAND uname -m OUTPUT_VARIABLE _host_arch OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_host_arch STREQUAL "arm64")
        if(NOT CMAKE_OSX_ARCHITECTURES)
            set(CMAKE_OSX_ARCHITECTURES x86_64 CACHE STRING "Target architecture" FORCE)
        endif()
        if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
            set(ITCH_ROSETTA ON)
        endif()
    endif()
endif()

project(itch_feed_handler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# 128-bit by default: wide vector instructions can change the core's frequency
# license and add jitter (see include/itch/platform.hpp). The AVX2 variant is
# built next to it, as separate *_avx2 targets, so the two can be measured
# against each other from one build directory.
set(ITCH_MARCH "x86-64-v2" CACHE STRING "ISA of the default targets (-march=). x86-64-v2 = SSE4.2, no AVX.")
option(ITCH_BUILD_AVX2_VARIANT "Also build *_avx2 targets with -march=x86-64-v3" ON)
set(ITCH_PREFER_VECTOR_WIDTH "128" CACHE STRING "Widest vector the compiler may auto-vectorise with (-mprefer-vector-width)")
set(ITCH_LEVEL_LINEAR_CHUNKS "4" CACHE STRING "8-key chunks scanned from the top of book before binary search (see feed_handler --depth-profile)")
option(ITCH_FORCE_SCALAR "Use the scalar/SWAR reference kernels everywhere (for A/B comparison)" OFF)
option(ITCH_WERROR       "Treat warnings as errors" ON)
option(ITCH_LTO          "Link-time optimisation" OFF)
set(ITCH_PGO "" CACHE STRING "Profile-guided optimisation: '', 'generate' or 'use' (see tools/pgo.sh)")
set(ITCH_PGO_DATA "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "PGO profile: a directory (GCC) or a .profdata file (Clang)")
option(ITCH_BOLT_READY   "Link executables with --emit-relocs so llvm-bolt can rewrite them (Linux)" OFF)
option(ITCH_BUILD_TESTS  "Build unit tests" ON)
option(ITCH_BUILD_BENCHMARKS "Build benchmarks" ON)
set(ITCH_SANITIZE "" CACHE STRING "Sanitizers: 'address,undefined' or 'thread'")

# ---------------------------------------------------------------------------
# Flags shared by every variant (ISA flags are added per variant below)
# ---------------------------------------------------------------------------
add_library(itch_common INTERFACE)
target_compile_options(itch_common INTERFACE
    -mprefer-vector-width=${ITCH_PREFER_VECTOR_WIDTH}
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    # GCC's -Wnull-dereference reports false positives on inlined lookups at -O3
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wnull-dereference>
    -fno-omit-frame-pointer           # keep stacks usable for perf(1)
    $<$<BOOL:${ITCH_WERROR}>:-Werror>)
target_compile_definitions(itch_common INTERFACE ITCH_LEVEL_LINEAR_CHUNKS=${ITCH_LEVEL_LINEAR_CHUNKS})
if(ITCH_FORCE_SCALAR)
    target_compile_definitions(itch_common INTERFACE ITCH_FORCE_SCALAR)
endif()
if(ITCH_SANITIZE)
    target_compile_options(itch_common INTERFACE -fsanitize=${ITCH_SANITIZE} -fno-sanitize-recover=all -g)
    target_link_options(itch_common INTERFACE -fsanitize=${ITCH_SANITIZE})
endif()

if(ITCH_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo OUTPUT _ipo_msg)
    if(_ipo)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO not supported: ${_ipo_msg}")
    endif()
endif()

if(ITCH_PGO)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(ITCH_PGO STREQUAL "generate")
            target_compile_options(itch_common INTERFACE -fprofile-instr-generate)
            target_link_options(itch_common INTERFACE -fprofile-instr-generate)
        elseif(ITCH_PGO STREQUAL "use")
            target_compile_options(itch_common INTERFACE -fprofile-instr-use=${ITCH_PGO_DATA}
                -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
        endif()
    else()  # GCC
        if(ITCH_PGO STREQUAL "generate")
            # atomic counters: the feed handler is multi-threaded
            target_compile_options(itch_common INTERFACE -fprofile-generate=${ITCH_PGO_DATA} -fprofile-update=atomic)
            target_link_options(itch_common INTERFACE -fprofile-generate=${ITCH_PGO_DATA})
        elseif(ITCH_PGO STREQUAL "use")
            target_compile_options(itch_common INTERFACE -fprofile-use=${ITCH_PGO_DATA}
                -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch)
        endif()
    endif()
    if(NOT ITCH_PGO MATCHES "^(generate|use)$")
        message(FATAL_ERROR "ITCH_PGO must be '', 'generate' or 'use'")
    endif()
endif()

if(ITCH_BOLT_READY)
    target_link_options(itch_common INTERFACE -Wl,--emit-relocs)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Third-party test/benchmark libraries: system package if present, otherwise
# fetched at a pinned, hash-checked version.
# ---------------------------------------------------------------------------
include(FetchContent)
if(ITCH_BUILD_TESTS)
    find_package(GTest 1.12 QUIET)
    if(NOT GTest_FOUND)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
            URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
    enable_testing()
    include(GoogleTest)
endif()

# Can this machine run AVX2 binaries? Decides whether *_avx2 tests are
# registered with CTest (building them is always fine).
# (Rosetta 2 executes AVX2 but hides it from CPUID, so trust it there.)
if(ITCH_ROSETTA)
    set(ITCH_HOST_RUNS_AVX2 TRUE)
else()
    include(CheckCXXSourceRuns)
    check_cxx_source_runs("int main() { __builtin_cpu_init(); return __builtin_cpu_supports(\"avx2\") ? 0 : 1; }"
                          ITCH_HOST_RUNS_AVX2)
endif()

set(ITCH_LIB_SOURCES
    src/itch/memory.cpp)
set(ITCH_TEST_SOURCES
    tests/test_group.cpp
    tests/test_order_map.cpp
    tests/test_messages.cpp
    tests/test_parser.cpp
    tests/test_add_order_decode.cpp)

# One complete set of targets per ISA level. Each variant is its own set of
# binaries: mixing ISAs inside one binary would let the linker pick the wide
# copy of an inline function for the narrow code path (ODR/COMDAT folding).
function(itch_add_variant suffix march)
    add_library(itch_options${suffix} INTERFACE)
    target_compile_options(itch_options${suffix} INTERFACE -march=${march})
    target_link_libraries(itch_options${suffix} INTERFACE itch_common)

    add_library(itch${suffix} STATIC ${ITCH_LIB_SOURCES})
    target_include_directories(itch${suffix} PUBLIC include)
    target_link_libraries(itch${suffix} PUBLIC itch_options${suffix} Threads::Threads)

    if(ITCH_BUILD_TESTS)
        add_executable(itch_tests${suffix} ${ITCH_TEST_SOURCES})
        target_link_libraries(itch_tests${suffix} PRIVATE itch${suffix} GTest::gtest_main)
        if(suffix STREQUAL "_avx2" AND NOT ITCH_HOST_RUNS_AVX2)
            message(STATUS "itch: this machine cannot run AVX2; itch_tests_avx2 is built but not registered with CTest")
        else()
            gtest_discover_tests(itch_tests${suffix} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60
                                 TEST_PREFIX "${march}.")
        endif()
    endif()

endfunction()

itch_add_variant("" ${ITCH_MARCH})
if(ITCH_BUILD_AVX2_VARIANT)
    itch_add_variant("_avx2" x86-64-v3)
endif()

message(STATUS "itch: CXX=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, build=${CMAKE_BUILD_TYPE}, "
               "default -march=${ITCH_MARCH}, avx2 variant=${ITCH_BUILD_AVX2_VARIANT}, "
               "vector width<=${ITCH_PREFER_VECTOR_WIDTH}, scalar=${ITCH_FORCE_SCALAR}, "
               "sanitize='${ITCH_SANITIZE}', lto=${ITCH_LTO}, pgo='${ITCH_PGO}'")
if(ITCH_ROSETTA)
    message(STATUS "itch: Apple Silicon host -> x86_64 build under Rosetta 2: correctness only, timings are not meaningful")
endif()
