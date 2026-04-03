cmake_minimum_required(VERSION 3.20)
project(OrderBookEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags — cross-platform (MSVC vs GCC/Clang)
if(MSVC)
  # MSVC flags
  set(CMAKE_CXX_FLAGS         "/W4 /EHsc")
  set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
  set(CMAKE_CXX_FLAGS_DEBUG   "/Od /Zi /RTC1")
else()
  # GCC / Clang flags
  set(CMAKE_CXX_FLAGS         "-Wall -Wextra -Wpedantic -pthread")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
  set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -fsanitize=address,undefined")
endif()

# Options
option(BUILD_TESTS    "Build unit tests"        ON)
option(BUILD_BENCHMARKS "Build benchmarks"     ON)
option(ENABLE_PROFILING "Enable profiling"    OFF)

if(ENABLE_PROFILING)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
endif()

# ─── Third-party / Find packages ───────────────────────────────────────────────
find_package(Threads REQUIRED)

# hiredis (Redis C client) — optional, for market data ingestion
find_library(HIREDIS_LIB hiredis)
find_path(HIREDIS_INCLUDE hiredis/hiredis.h)

# libpq (PostgreSQL C client) — optional, for historical data
find_package(PostgreSQL QUIET)

# Google Test — optional
if(BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  enable_testing()
endif()

# Google Benchmark — optional
if(BUILD_BENCHMARKS)
  find_package(benchmark QUIET)
  if(NOT benchmark_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      benchmark
      URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
  endif()
endif()

# ─── Subdirectories ─────────────────────────────────────────────────────────────
add_subdirectory(src)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
