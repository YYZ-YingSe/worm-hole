if(NOT EXISTS "${WH_CATCH2_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "Catch2 source is required at ${WH_CATCH2_DIR}\n"
    "Hint: sync thirdy_party submodules or pass -DWH_CATCH2_DIR=<path>")
endif()

add_subdirectory("${WH_CATCH2_DIR}" "${CMAKE_BINARY_DIR}/thirdy_party/catch2" EXCLUDE_FROM_ALL)

list(APPEND CMAKE_MODULE_PATH "${WH_CATCH2_DIR}/extras")
include(Catch)

add_executable(
  wh_core_tests
  core/compiler_contracts_test.cpp
  core/type_utils_metaprogramming_contracts_test.cpp
  core/type_utils_default_instance_test.cpp
  core/reflect_field_access_test.cpp
  core/type_name_registry_contracts_test.cpp)

target_link_libraries(wh_core_tests PRIVATE wh::core Catch2::Catch2WithMain)

catch_discover_tests(wh_core_tests)
