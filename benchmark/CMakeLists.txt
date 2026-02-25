if(NOT EXISTS "${WH_BENCHMARK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "Google Benchmark source is required at ${WH_BENCHMARK_DIR}\n"
    "Hint: sync thirdy_party submodules or pass -DWH_BENCHMARK_DIR=<path>")
endif()

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_EXCEPTIONS OFF CACHE BOOL "" FORCE)

if(NOT TARGET benchmark::benchmark)
  add_subdirectory(
    "${WH_BENCHMARK_DIR}"
    "${CMAKE_BINARY_DIR}/thirdy_party/google-benchmark"
    EXCLUDE_FROM_ALL)
endif()

add_executable(
  wh_core_benchmarks
  small_vector_vs_std_vector_benchmark.cpp)

target_link_libraries(wh_core_benchmarks PRIVATE wh::core benchmark::benchmark)

target_compile_features(wh_core_benchmarks PRIVATE cxx_std_20)

add_executable(
  wh_sender_notify_benchmarks
  sender_notify_benchmark.cpp)

target_link_libraries(
  wh_sender_notify_benchmarks
  PRIVATE wh::core benchmark::benchmark)

target_compile_features(wh_sender_notify_benchmarks PRIVATE cxx_std_20)

add_executable(
  wh_mpmc_queue_benchmarks
  mpmc_queue_benchmark.cpp)

target_link_libraries(
  wh_mpmc_queue_benchmarks
  PRIVATE wh::core benchmark::benchmark)

target_compile_features(wh_mpmc_queue_benchmarks PRIVATE cxx_std_20)

add_custom_target(
  run_wh_core_benchmarks
  COMMAND wh_core_benchmarks --benchmark_counters_tabular=true
  DEPENDS wh_core_benchmarks
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_custom_target(
  run_wh_sender_notify_benchmarks
  COMMAND wh_sender_notify_benchmarks --benchmark_counters_tabular=true
  DEPENDS wh_sender_notify_benchmarks
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_custom_target(
  run_wh_mpmc_queue_benchmarks
  COMMAND wh_mpmc_queue_benchmarks --benchmark_counters_tabular=true
  DEPENDS wh_mpmc_queue_benchmarks
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
