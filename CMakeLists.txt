cmake_minimum_required(VERSION 3.24)
project(plgen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # lets clangd / VS Code understand the code

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PLGEN_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
if(PLGEN_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ThreadSanitizer finds data races between threads. Can't be combined with ASan.
option(PLGEN_TSAN "Build with ThreadSanitizer" OFF)
if(PLGEN_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# Warnings live on an INTERFACE target so we apply them only to our own code,
# never to third-party code like GoogleTest.
add_library(plgen_warnings INTERFACE)
target_compile_options(plgen_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion>)

# --- Core library: all real logic lives here ---
add_library(plgen_core
  src/core/csv.cpp
  src/core/normalizer.cpp
  src/core/feature_store.cpp
  src/core/dataset.cpp
  src/core/brute_force.cpp
  src/core/groundtruth.cpp
  src/core/recommender.cpp
  src/core/genre_graph.cpp
  src/core/hnsw.cpp
  src/core/http.cpp
  src/core/thread_pool.cpp
  src/core/http_server.cpp
  src/core/api.cpp)
target_include_directories(plgen_core PUBLIC include)
target_link_libraries(plgen_core PUBLIC Threads::Threads PRIVATE plgen_warnings)

# --- Executables: thin wrappers around the library ---
add_executable(plgen_bench apps/bench.cpp)
target_link_libraries(plgen_bench PRIVATE plgen_core plgen_warnings)

add_executable(plgen_server apps/server.cpp)
target_link_libraries(plgen_server PRIVATE plgen_core plgen_warnings)

add_executable(plgen_loadtest apps/loadtest.cpp)
target_link_libraries(plgen_loadtest PRIVATE plgen_core plgen_warnings)

add_executable(plgen_bench_hnsw apps/bench_hnsw.cpp)
target_link_libraries(plgen_bench_hnsw PRIVATE plgen_core plgen_warnings)

add_executable(plgen_similar apps/similar.cpp)
target_link_libraries(plgen_similar PRIVATE plgen_core plgen_warnings)

# --- Tests ---
option(PLGEN_BUILD_TESTS "Build unit tests" ON)
if(PLGEN_BUILD_TESTS)
  # Use an installed GoogleTest if there is one (e.g. `brew install googletest`),
  # otherwise download it at configure time.
  find_package(GTest 1.12 QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(plgen_tests
    tests/csv_test.cpp
    tests/normalizer_test.cpp
    tests/dataset_test.cpp
    tests/brute_force_test.cpp
    tests/groundtruth_test.cpp
    tests/recommender_test.cpp
    tests/genre_graph_test.cpp
    tests/hnsw_test.cpp
    tests/http_test.cpp
    tests/thread_pool_test.cpp
    tests/api_test.cpp
    tests/server_test.cpp)
  target_link_libraries(plgen_tests PRIVATE plgen_core plgen_warnings GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(plgen_tests)
endif()
