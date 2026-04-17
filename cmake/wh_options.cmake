include_guard(GLOBAL)

option(BUILD_TESTING "Enable CTest integration" ON)
option(WH_BUILD_TESTING "Build worm-hole tests" ON)
option(WH_BUILD_UT "Build worm-hole unit tests" ON)
option(WH_BUILD_FT "Build worm-hole functional tests" ON)
option(WH_BUILD_ANALYSIS "Build worm-hole static-analysis drivers" OFF)
option(WH_BUILD_EXAMPLES "Build curated worm-hole examples" OFF)
option(WH_BUILD_BENCHMARKS "Build worm-hole benchmark executables" OFF)
option(WH_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(WH_REQUIRE_GIT_LOCKED_THIRDY_PARTY
       "Require git-locked thirdy_party deps" ON)

set(WH_TEST_EXECUTABLE_LAYOUT
    "source"
    CACHE STRING
    "Test executable layout. Supported values: source")
set_property(CACHE WH_TEST_EXECUTABLE_LAYOUT PROPERTY STRINGS source)

set(_wh_supported_test_layouts source)
if(NOT WH_TEST_EXECUTABLE_LAYOUT IN_LIST _wh_supported_test_layouts)
  message(
    FATAL_ERROR
      "WH_TEST_EXECUTABLE_LAYOUT must be one of: ${_wh_supported_test_layouts}; got '${WH_TEST_EXECUTABLE_LAYOUT}'"
  )
endif()

set(WH_THIRDY_PARTY_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdy_party"
    CACHE PATH
    "thirdy_party root directory")

set(WH_THIRDY_PARTY_DIRECT_DIR
    "${WH_THIRDY_PARTY_DIR}"
    CACHE PATH
    "direct thirdy_party dependency root")
set(WH_THIRDY_PARTY_TRANSITIVE_DIR
    "${WH_THIRDY_PARTY_DIR}/dependencies"
    CACHE PATH
    "transitive thirdy_party dependency root")

set(WH_STDEXEC_DIR
    "${WH_THIRDY_PARTY_DIRECT_DIR}/stdexec"
    CACHE PATH
    "stdexec source directory")
set(WH_RAPIDJSON_DIR
    "${WH_THIRDY_PARTY_DIRECT_DIR}/rapidjson"
    CACHE PATH
    "rapidjson source directory")
set(WH_CATCH2_DIR
    "${WH_THIRDY_PARTY_DIRECT_DIR}/catch2"
    CACHE PATH
    "catch2 source directory")
set(WH_BENCHMARK_DIR
    "${WH_THIRDY_PARTY_DIRECT_DIR}/benchmark"
    CACHE PATH
    "benchmark source directory")
set(WH_MINJA_DIR
    "${WH_THIRDY_PARTY_DIRECT_DIR}/minja"
    CACHE PATH
    "minja source directory")
set(WH_NLOHMANN_JSON_DIR
    "${WH_THIRDY_PARTY_TRANSITIVE_DIR}/nlohmann_json"
    CACHE PATH
    "nlohmann_json source directory")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
