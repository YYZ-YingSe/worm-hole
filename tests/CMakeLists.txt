include(wh_artifact_targets)
include("${CMAKE_CURRENT_SOURCE_DIR}/test_profiles.cmake")

wh_setup_root_artifact_targets()
wh_setup_test_suite_targets()
wh_apply_project_test_profiles()

if(NOT WH_BUILD_UT AND NOT WH_BUILD_FT)
  message(STATUS "worm-hole tests enabled, but all test suites are disabled")
  return()
endif()

wh_require_source_path(
  "${WH_CATCH2_DIR}/extras/Catch.cmake"
  "Catch2 CMake helper"
  "sync thirdy_party submodules or pass -DWH_CATCH2_DIR=<path>")
wh_require_source_path(
  "${WH_CATCH2_DIR}/extras/catch_amalgamated.cpp"
  "Catch2 amalgamated source"
  "sync thirdy_party submodules or pass -DWH_CATCH2_DIR=<path>")
wh_require_source_path(
  "${WH_CATCH2_DIR}/src/catch2/catch_user_config.hpp.in"
  "Catch2 user config template"
  "sync thirdy_party submodules or pass -DWH_CATCH2_DIR=<path>")

list(APPEND CMAKE_MODULE_PATH "${WH_CATCH2_DIR}/extras")
include(Catch)
set(_CATCH_DISCOVER_TESTS_SCRIPT
    "${PROJECT_SOURCE_DIR}/cmake/wh_catch_add_tests.cmake"
    CACHE INTERNAL "worm-hole Catch2 discovery helper with zero-test support"
    FORCE)

wh_setup_test_support_targets()

function(wh_filter_test_sources_by_labels out_var)
  set(options)
  set(one_value_args SUITE_LABEL)
  set(multi_value_args SOURCES INCLUDE_LABELS EXCLUDE_LABELS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  if(NOT ARG_SUITE_LABEL)
    message(FATAL_ERROR "wh_filter_test_sources_by_labels missing SUITE_LABEL")
  endif()

  set(filtered_sources)
  foreach(source_file IN LISTS ARG_SOURCES)
    wh_compute_test_metadata(
      resolved_labels
      resolved_weight
      resolved_timeout_seconds
      SOURCE_FILE "${source_file}"
      LABELS "${ARG_SUITE_LABEL}")

    set(include_source TRUE)
    foreach(required_label IN LISTS ARG_INCLUDE_LABELS)
      if(NOT required_label IN_LIST resolved_labels)
        set(include_source FALSE)
      endif()
    endforeach()
    foreach(excluded_label IN LISTS ARG_EXCLUDE_LABELS)
      if(excluded_label IN_LIST resolved_labels)
        set(include_source FALSE)
      endif()
    endforeach()

    if(include_source)
      list(APPEND filtered_sources "${source_file}")
    endif()
  endforeach()

  set("${out_var}" "${filtered_sources}" PARENT_SCOPE)
endfunction()

if(WH_BUILD_FT)
  add_subdirectory(FT)
endif()

if(WH_BUILD_UT)
  add_subdirectory(UT)
endif()

wh_write_catch_test_manifest("${CMAKE_BINARY_DIR}/wh_test_manifest.tsv")
