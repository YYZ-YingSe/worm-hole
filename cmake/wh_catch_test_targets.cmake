include_guard(GLOBAL)

include(wh_single_source_targets)

function(wh_escape_manifest_field out_var value)
  string(REPLACE "\\" "\\\\" escaped "${value}")
  string(REPLACE "\t" "\\t" escaped "${escaped}")
  string(REPLACE "\n" "\\n" escaped "${escaped}")
  set("${out_var}" "${escaped}" PARENT_SCOPE)
endfunction()

function(wh_source_test_prefix out_var suite_prefix source_root source_file)
  cmake_path(ABSOLUTE_PATH source_root NORMALIZE OUTPUT_VARIABLE absolute_root)
  cmake_path(ABSOLUTE_PATH source_file NORMALIZE OUTPUT_VARIABLE absolute_source)
  cmake_path(RELATIVE_PATH absolute_source BASE_DIRECTORY "${absolute_root}"
             OUTPUT_VARIABLE relative_source)
  string(REGEX REPLACE "\\.[^.]+$" "" relative_stem "${relative_source}")
  string(REPLACE "\\" "/" relative_stem "${relative_stem}")
  set("${out_var}" "${suite_prefix}${relative_stem}::" PARENT_SCOPE)
endfunction()

function(wh_normalize_test_source_path out_var source_file)
  cmake_path(ABSOLUTE_PATH source_file
             BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
             NORMALIZE
             OUTPUT_VARIABLE absolute_source)
  set("${out_var}" "${absolute_source}" PARENT_SCOPE)
endfunction()

function(wh_test_source_profile_property_name out_var source_file property_suffix)
  wh_normalize_test_source_path(normalized_source "${source_file}")
  string(MD5 source_key "${normalized_source}")
  set("${out_var}" "WH_TEST_PROFILE_${source_key}_${property_suffix}" PARENT_SCOPE)
endfunction()

function(wh_get_test_source_profile_property out_var)
  set(options)
  set(one_value_args SOURCE_FILE PROPERTY_NAME)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "" ${ARGN})

  foreach(required_arg SOURCE_FILE PROPERTY_NAME)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR
              "wh_get_test_source_profile_property missing ${required_arg}")
    endif()
  endforeach()

  wh_test_source_profile_property_name(property_name "${ARG_SOURCE_FILE}"
                                       "${ARG_PROPERTY_NAME}")
  get_property(property_set GLOBAL PROPERTY "${property_name}" SET)
  if(property_set)
    get_property(property_value GLOBAL PROPERTY "${property_name}")
  else()
    set(property_value "")
  endif()

  set("${out_var}" "${property_value}" PARENT_SCOPE)
endfunction()

function(wh_set_test_source_profile_property)
  set(options)
  set(one_value_args SOURCE_FILE PROPERTY_NAME)
  set(multi_value_args VALUES)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  foreach(required_arg SOURCE_FILE PROPERTY_NAME)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR
              "wh_set_test_source_profile_property missing ${required_arg}")
    endif()
  endforeach()

  wh_test_source_profile_property_name(property_name "${ARG_SOURCE_FILE}"
                                       "${ARG_PROPERTY_NAME}")
  set_property(GLOBAL PROPERTY "${property_name}" "${ARG_VALUES}")
endfunction()

function(wh_append_test_source_profile_property)
  set(options)
  set(one_value_args SOURCE_FILE PROPERTY_NAME)
  set(multi_value_args VALUES)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  foreach(required_arg SOURCE_FILE PROPERTY_NAME)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR
              "wh_append_test_source_profile_property missing ${required_arg}")
    endif()
  endforeach()

  wh_get_test_source_profile_property(existing_values
                                      SOURCE_FILE "${ARG_SOURCE_FILE}"
                                      PROPERTY_NAME "${ARG_PROPERTY_NAME}")
  set(merged_values ${existing_values} ${ARG_VALUES})
  list(REMOVE_DUPLICATES merged_values)

  wh_set_test_source_profile_property(
    SOURCE_FILE "${ARG_SOURCE_FILE}"
    PROPERTY_NAME "${ARG_PROPERTY_NAME}"
    VALUES ${merged_values})
endfunction()

function(wh_set_test_source_profile)
  set(options)
  set(one_value_args SOURCE_FILE WEIGHT TIMEOUT_SECONDS)
  set(multi_value_args ADD_LABELS REMOVE_LABELS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  if(NOT ARG_SOURCE_FILE)
    message(FATAL_ERROR "wh_set_test_source_profile missing SOURCE_FILE")
  endif()

  if(ARG_ADD_LABELS)
    wh_append_test_source_profile_property(
      SOURCE_FILE "${ARG_SOURCE_FILE}"
      PROPERTY_NAME "ADD_LABELS"
      VALUES ${ARG_ADD_LABELS})
  endif()

  if(ARG_REMOVE_LABELS)
    wh_append_test_source_profile_property(
      SOURCE_FILE "${ARG_SOURCE_FILE}"
      PROPERTY_NAME "REMOVE_LABELS"
      VALUES ${ARG_REMOVE_LABELS})
  endif()

  if(ARG_WEIGHT)
    wh_set_test_source_profile_property(
      SOURCE_FILE "${ARG_SOURCE_FILE}"
      PROPERTY_NAME "WEIGHT"
      VALUES "${ARG_WEIGHT}")
  endif()

  if(ARG_TIMEOUT_SECONDS)
    wh_set_test_source_profile_property(
      SOURCE_FILE "${ARG_SOURCE_FILE}"
      PROPERTY_NAME "TIMEOUT_SECONDS"
      VALUES "${ARG_TIMEOUT_SECONDS}")
  endif()
endfunction()

function(wh_compute_test_metadata labels_out weight_out timeout_out)
  set(options)
  set(one_value_args SOURCE_FILE)
  set(multi_value_args LABELS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  if(NOT ARG_SOURCE_FILE)
    message(FATAL_ERROR "wh_compute_test_metadata missing SOURCE_FILE")
  endif()

  set(labels ${ARG_LABELS})
  set(weight 1)
  set(timeout_seconds 120)

  if("UT" IN_LIST labels)
    list(APPEND labels "kind.ut")
    set(weight 1)
    set(timeout_seconds 120)
  endif()

  if("FT" IN_LIST labels)
    list(APPEND labels "kind.ft")
    set(weight 4)
    set(timeout_seconds 180)
  endif()

  list(APPEND labels "ci.pr" "sanitizer.safe")

  # Test profile overrides must survive directory boundaries between
  # tests/test_profiles.cmake and the bundle registration helpers.
  wh_get_test_source_profile_property(add_labels
                                      SOURCE_FILE "${ARG_SOURCE_FILE}"
                                      PROPERTY_NAME "ADD_LABELS")
  if(add_labels)
    list(APPEND labels ${add_labels})
  endif()

  wh_get_test_source_profile_property(remove_labels
                                      SOURCE_FILE "${ARG_SOURCE_FILE}"
                                      PROPERTY_NAME "REMOVE_LABELS")
  if(remove_labels)
    list(REMOVE_ITEM labels ${remove_labels})
  endif()

  wh_get_test_source_profile_property(weight_override
                                      SOURCE_FILE "${ARG_SOURCE_FILE}"
                                      PROPERTY_NAME "WEIGHT")
  if(weight_override)
    set(weight "${weight_override}")
  endif()

  wh_get_test_source_profile_property(timeout_override
                                      SOURCE_FILE "${ARG_SOURCE_FILE}"
                                      PROPERTY_NAME "TIMEOUT_SECONDS")
  if(timeout_override)
    set(timeout_seconds "${timeout_override}")
  endif()

  list(REMOVE_DUPLICATES labels)

  set("${labels_out}" "${labels}" PARENT_SCOPE)
  set("${weight_out}" "${weight}" PARENT_SCOPE)
  set("${timeout_out}" "${timeout_seconds}" PARENT_SCOPE)
endfunction()

function(wh_register_catch_test_target)
  set(options)
  set(one_value_args TARGET_NAME SOURCE_FILE TEST_PREFIX WEIGHT TIMEOUT_SECONDS)
  set(multi_value_args LABELS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  foreach(required_arg TARGET_NAME SOURCE_FILE TEST_PREFIX)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR
              "wh_register_catch_test_target missing ${required_arg}")
    endif()
  endforeach()

  cmake_path(ABSOLUTE_PATH ARG_SOURCE_FILE NORMALIZE OUTPUT_VARIABLE absolute_source)
  cmake_path(RELATIVE_PATH absolute_source BASE_DIRECTORY "${PROJECT_SOURCE_DIR}"
             OUTPUT_VARIABLE relative_source)

  file(SIZE "${ARG_SOURCE_FILE}" source_size)

  string(REGEX REPLACE "::.*$" "" suite_name "${ARG_TEST_PREFIX}")

  wh_register_catch_manifest_entry(
    TARGET_NAME "${ARG_TARGET_NAME}"
    SUITE_NAME "${suite_name}"
    SOURCE_PATH "${relative_source}"
    SOURCE_SIZE "${source_size}"
    WEIGHT "${ARG_WEIGHT}"
    TIMEOUT_SECONDS "${ARG_TIMEOUT_SECONDS}"
    LABELS ${ARG_LABELS})
endfunction()

function(wh_register_catch_manifest_entry)
  set(options)
  set(one_value_args TARGET_NAME SUITE_NAME SOURCE_PATH SOURCE_SIZE WEIGHT TIMEOUT_SECONDS)
  set(multi_value_args LABELS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  foreach(required_arg TARGET_NAME SUITE_NAME SOURCE_PATH SOURCE_SIZE WEIGHT TIMEOUT_SECONDS)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR
              "wh_register_catch_manifest_entry missing ${required_arg}")
    endif()
  endforeach()

  string(REPLACE ";" "," labels_csv "${ARG_LABELS}")

  wh_escape_manifest_field(escaped_suite "${ARG_SUITE_NAME}")
  wh_escape_manifest_field(escaped_target "${ARG_TARGET_NAME}")
  wh_escape_manifest_field(escaped_source "${ARG_SOURCE_PATH}")
  wh_escape_manifest_field(escaped_labels "${labels_csv}")

  set(row
      "${escaped_suite}\t${escaped_target}\t$<TARGET_FILE:${ARG_TARGET_NAME}>\t${escaped_source}\t${ARG_SOURCE_SIZE}\t${ARG_WEIGHT}\t${ARG_TIMEOUT_SECONDS}\t${escaped_labels}\n")
  set_property(GLOBAL APPEND_STRING PROPERTY WH_CATCH_TEST_MANIFEST_ROWS
               "${row}")
endfunction()

function(wh_write_catch_test_manifest output_path)
  cmake_path(ABSOLUTE_PATH output_path NORMALIZE OUTPUT_VARIABLE absolute_output)

  get_property(rows GLOBAL PROPERTY WH_CATCH_TEST_MANIFEST_ROWS)
  if(NOT rows)
    set(rows "")
  endif()

  file(GENERATE
       OUTPUT "${absolute_output}"
       CONTENT
         "suite\ttarget\texecutable\tsource\tsource_size\tweight\ttimeout_seconds\tlabels\n${rows}")
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

  wh_compute_test_metadata(
    resolved_labels
    resolved_weight
    resolved_timeout_seconds
    SOURCE_FILE "${ARG_SOURCE_FILE}"
    LABELS ${ARG_LABELS})

  if(resolved_labels)
    catch_discover_tests("${target_name}" TEST_PREFIX "${discovery_prefix}"
                         DISCOVERY_MODE PRE_TEST
                         PROPERTIES
                           LABELS "${resolved_labels}"
                           TIMEOUT "${resolved_timeout_seconds}")
  else()
    catch_discover_tests("${target_name}" TEST_PREFIX "${discovery_prefix}"
                         DISCOVERY_MODE PRE_TEST
                         PROPERTIES TIMEOUT "${resolved_timeout_seconds}")
  endif()

  wh_register_catch_test_target(
    TARGET_NAME "${target_name}"
    SOURCE_FILE "${ARG_SOURCE_FILE}"
    TEST_PREFIX "${ARG_TEST_PREFIX}"
    WEIGHT "${resolved_weight}"
    TIMEOUT_SECONDS "${resolved_timeout_seconds}"
    LABELS ${resolved_labels})

  set("${out_var}" "${target_name}" PARENT_SCOPE)
endfunction()

function(wh_add_catch_test_bundle out_var)
  set(options)
  set(one_value_args TARGET_NAME TEST_PREFIX SUITE_NAME MANIFEST_SOURCE
                     WEIGHT TIMEOUT_SECONDS)
  set(multi_value_args SOURCES LINK_LIBRARIES INCLUDE_DIRECTORIES LABELS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  foreach(required_arg TARGET_NAME TEST_PREFIX SUITE_NAME MANIFEST_SOURCE
                       WEIGHT TIMEOUT_SECONDS)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR "wh_add_catch_test_bundle missing ${required_arg}")
    endif()
  endforeach()

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "wh_add_catch_test_bundle requires at least one source")
  endif()

  add_executable("${ARG_TARGET_NAME}" ${ARG_SOURCES})
  if(ARG_LINK_LIBRARIES)
    target_link_libraries("${ARG_TARGET_NAME}" PRIVATE ${ARG_LINK_LIBRARIES})
  endif()
  if(ARG_INCLUDE_DIRECTORIES)
    target_include_directories("${ARG_TARGET_NAME}" PRIVATE
                               ${ARG_INCLUDE_DIRECTORIES})
  endif()

  catch_discover_tests(
    "${ARG_TARGET_NAME}"
    TEST_PREFIX "${ARG_TEST_PREFIX}"
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      LABELS "${ARG_LABELS}"
      TIMEOUT "${ARG_TIMEOUT_SECONDS}")

  set(total_source_size 0)
  foreach(source_file IN LISTS ARG_SOURCES)
    file(SIZE "${source_file}" source_size)
    math(EXPR total_source_size "${total_source_size} + ${source_size}")
  endforeach()

  wh_register_catch_manifest_entry(
    TARGET_NAME "${ARG_TARGET_NAME}"
    SUITE_NAME "${ARG_SUITE_NAME}"
    SOURCE_PATH "${ARG_MANIFEST_SOURCE}"
    SOURCE_SIZE "${total_source_size}"
    WEIGHT "${ARG_WEIGHT}"
    TIMEOUT_SECONDS "${ARG_TIMEOUT_SECONDS}"
    LABELS ${ARG_LABELS})

  set("${out_var}" "${ARG_TARGET_NAME}" PARENT_SCOPE)
endfunction()
