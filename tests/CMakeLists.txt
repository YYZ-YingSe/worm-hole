include(wh_artifact_targets)

wh_setup_root_artifact_targets()
wh_setup_test_suite_targets()

if(NOT WH_BUILD_UT AND NOT WH_BUILD_FT)
  message(STATUS "worm-hole tests enabled, but all test suites are disabled")
  return()
endif()

wh_require_cmake_source_dir(
  "${WH_CATCH2_DIR}"
  "Catch2"
  "sync thirdy_party submodules or pass -DWH_CATCH2_DIR=<path>")

add_subdirectory("${WH_CATCH2_DIR}" "${CMAKE_BINARY_DIR}/thirdy_party/catch2" EXCLUDE_FROM_ALL)

list(APPEND CMAKE_MODULE_PATH "${WH_CATCH2_DIR}/extras")
include(Catch)

wh_setup_test_support_targets()

if(WH_BUILD_FT)
  add_subdirectory(FT)
endif()

if(WH_BUILD_UT)
  add_subdirectory(UT)
endif()
