cmake_minimum_required(VERSION 3.25)

project(worm_hole VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/policies")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains")

include(wh_options)
include(wh_helpers)
include(wh_third_party)
include(wh_core_target)
include(wh_artifact_targets)

wh_setup_third_party()
wh_setup_core_target()
wh_setup_root_artifact_targets()

if(WH_BUILD_ANALYSIS)
  add_subdirectory(analysis)
endif()

if(BUILD_TESTING AND WH_BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

if(WH_BUILD_EXAMPLES)
  add_subdirectory(example)
endif()

if(WH_BUILD_BENCHMARKS)
  add_subdirectory(benchmark)
endif()
