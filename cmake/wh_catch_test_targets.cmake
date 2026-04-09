include_guard(GLOBAL)

include(wh_single_source_targets)

function(wh_source_test_prefix out_var suite_prefix source_root source_file)
  cmake_path(ABSOLUTE_PATH source_root NORMALIZE OUTPUT_VARIABLE absolute_root)
  cmake_path(ABSOLUTE_PATH source_file NORMALIZE OUTPUT_VARIABLE absolute_source)
  cmake_path(RELATIVE_PATH absolute_source BASE_DIRECTORY "${absolute_root}"
             OUTPUT_VARIABLE relative_source)
  string(REGEX REPLACE "\\.[^.]+$" "" relative_stem "${relative_source}")
  string(REPLACE "\\" "/" relative_stem "${relative_stem}")
  set("${out_var}" "${suite_prefix}${relative_stem}::" PARENT_SCOPE)
endfunction()

function(wh_add_catch_test_source out_var)
  set(options)
  set(one_value_args TARGET_PREFIX SOURCE_ROOT SOURCE_FILE TEST_PREFIX)
  set(multi_value_args LINK_LIBRARIES INCLUDE_DIRECTORIES LABELS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  foreach(required_arg TARGET_PREFIX SOURCE_ROOT SOURCE_FILE TEST_PREFIX)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR "wh_add_catch_test_source missing ${required_arg}")
    endif()
  endforeach()

  wh_add_single_source_executable(
    target_name
    TARGET_PREFIX "${ARG_TARGET_PREFIX}"
    SOURCE_ROOT "${ARG_SOURCE_ROOT}"
    SOURCE_FILE "${ARG_SOURCE_FILE}"
    LINK_LIBRARIES ${ARG_LINK_LIBRARIES}
    INCLUDE_DIRECTORIES ${ARG_INCLUDE_DIRECTORIES})

  wh_source_test_prefix(discovery_prefix "${ARG_TEST_PREFIX}"
                        "${ARG_SOURCE_ROOT}" "${ARG_SOURCE_FILE}")

  if(ARG_LABELS)
    catch_discover_tests("${target_name}" TEST_PREFIX "${discovery_prefix}"
                         DISCOVERY_MODE PRE_TEST
                         PROPERTIES LABELS "${ARG_LABELS}")
  else()
    catch_discover_tests("${target_name}" TEST_PREFIX "${discovery_prefix}"
                         DISCOVERY_MODE PRE_TEST)
  endif()

  set("${out_var}" "${target_name}" PARENT_SCOPE)
endfunction()
