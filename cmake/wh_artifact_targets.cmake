include_guard(GLOBAL)

include(wh_support_targets)
include(wh_source_targets)

set(WH_TARGET_TEST_ARTIFACTS "wh_test_artifacts")
set(WH_TARGET_ENABLED_ARTIFACTS "wh_enabled_artifacts")

set(WH_TARGET_UT_TESTS "wh_ut_tests")
set(WH_TARGET_FT_TESTS "wh_ft_tests")
set(WH_TARGET_EXAMPLES "wh_examples")
set(WH_TARGET_BENCHMARKS "wh_benchmarks")

set(WH_TARGET_TEST_SUPPORT "wh_test_support")
set(WH_TARGET_UT_SUPPORT "wh_ut_support")
set(WH_TARGET_FT_SUPPORT "wh_ft_support")
set(WH_TARGET_EXAMPLE_SUPPORT "wh_example_support")
set(WH_TARGET_BENCHMARK_SUPPORT "wh_benchmark_support")
set(WH_TARGET_CATCH2_AMALGAMATED "wh_catch2_amalgamated")

function(wh_ensure_custom_target target_name)
  if(TARGET "${target_name}")
    return()
  endif()

  add_custom_target("${target_name}")
endfunction()

function(wh_setup_root_artifact_targets)
  wh_ensure_custom_target("${WH_TARGET_TEST_ARTIFACTS}")
  wh_ensure_custom_target("${WH_TARGET_ENABLED_ARTIFACTS}")
endfunction()

function(wh_setup_test_suite_targets)
  wh_ensure_custom_target("${WH_TARGET_UT_TESTS}")
  wh_ensure_custom_target("${WH_TARGET_FT_TESTS}")
endfunction()

function(wh_ensure_catch2_amalgamated_target)
  if(TARGET "${WH_TARGET_CATCH2_AMALGAMATED}")
    return()
  endif()

  set(catch2_amalgamated_cpp "${WH_CATCH2_DIR}/extras/catch_amalgamated.cpp")
  set(catch2_user_config_in "${WH_CATCH2_DIR}/src/catch2/catch_user_config.hpp.in")
  set(catch2_generated_dir "${PROJECT_BINARY_DIR}/generated-includes/catch2")
  set(catch2_user_config_out "${catch2_generated_dir}/catch_user_config.hpp")
  if(NOT EXISTS "${catch2_amalgamated_cpp}")
    message(FATAL_ERROR
            "Catch2 amalgamated source is required at ${catch2_amalgamated_cpp}")
  endif()
  if(NOT EXISTS "${catch2_user_config_in}")
    message(FATAL_ERROR
            "Catch2 user config template is required at ${catch2_user_config_in}")
  endif()

  file(MAKE_DIRECTORY "${catch2_generated_dir}")
  set(CATCH_CONFIG_NO_COUNTER ON)
  configure_file("${catch2_user_config_in}" "${catch2_user_config_out}")

  add_library("${WH_TARGET_CATCH2_AMALGAMATED}" STATIC
              "${catch2_amalgamated_cpp}")
  target_include_directories("${WH_TARGET_CATCH2_AMALGAMATED}"
                             PUBLIC "${WH_CATCH2_DIR}/src"
                                    "${PROJECT_BINARY_DIR}/generated-includes")
endfunction()

function(wh_register_project_artifact group target_name)
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR
            "wh_register_project_artifact target does not exist: ${target_name}")
  endif()

  wh_setup_root_artifact_targets()

  if(group STREQUAL "TESTS")
    add_dependencies("${WH_TARGET_TEST_ARTIFACTS}" "${target_name}")
    add_dependencies("${WH_TARGET_ENABLED_ARTIFACTS}" "${target_name}")
    return()
  endif()

  if(group STREQUAL "ENABLED")
    add_dependencies("${WH_TARGET_ENABLED_ARTIFACTS}" "${target_name}")
    return()
  endif()

  message(FATAL_ERROR
          "wh_register_project_artifact expects group TESTS or ENABLED, got: ${group}")
endfunction()

function(wh_setup_test_support_targets)
  wh_ensure_catch2_amalgamated_target()

  wh_define_support_target(
    "${WH_TARGET_TEST_SUPPORT}"
    LINK_LIBRARIES wh::core "${WH_TARGET_CATCH2_AMALGAMATED}"
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/tests")

  wh_define_support_target(
    "${WH_TARGET_UT_SUPPORT}"
    LINK_LIBRARIES "${WH_TARGET_TEST_SUPPORT}"
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/tests/UT")
  wh_define_support_target(
    "${WH_TARGET_FT_SUPPORT}"
    LINK_LIBRARIES "${WH_TARGET_TEST_SUPPORT}"
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/tests/FT")
endfunction()

function(wh_setup_example_support_target)
  wh_define_support_target("${WH_TARGET_EXAMPLE_SUPPORT}" LINK_LIBRARIES wh::core)
endfunction()

function(wh_setup_benchmark_support_target)
  wh_define_support_target("${WH_TARGET_BENCHMARK_SUPPORT}"
                           LINK_LIBRARIES wh::core benchmark::benchmark_main)
endfunction()
