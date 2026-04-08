include_guard(GLOBAL)

include(wh_single_source_targets)
include(wh_catch_test_targets)

function(wh_ensure_bundle_target target_name)
  if(TARGET "${target_name}")
    return()
  endif()

  add_custom_target("${target_name}")
endfunction()

function(wh_add_source_bundle)
  set(options RECURSE)
  set(one_value_args AGGREGATE_TARGET TARGET_PREFIX SOURCE_ROOT GLOB TEST_PREFIX)
  set(multi_value_args LINK_LIBRARIES INCLUDE_DIRECTORIES LABELS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  foreach(required_arg AGGREGATE_TARGET TARGET_PREFIX SOURCE_ROOT GLOB)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR "wh_add_source_bundle missing ${required_arg}")
    endif()
  endforeach()

  if(ARG_RECURSE)
    file(GLOB_RECURSE bundle_sources CONFIGURE_DEPENDS
         "${ARG_SOURCE_ROOT}/${ARG_GLOB}")
  else()
    file(GLOB bundle_sources CONFIGURE_DEPENDS
         "${ARG_SOURCE_ROOT}/${ARG_GLOB}")
  endif()
  list(SORT bundle_sources)

  wh_ensure_bundle_target("${ARG_AGGREGATE_TARGET}")

  foreach(source_file IN LISTS bundle_sources)
    if(ARG_TEST_PREFIX)
      wh_add_catch_test_source(
        bundle_target
        TARGET_PREFIX "${ARG_TARGET_PREFIX}"
        SOURCE_ROOT "${ARG_SOURCE_ROOT}"
        SOURCE_FILE "${source_file}"
        TEST_PREFIX "${ARG_TEST_PREFIX}"
        LINK_LIBRARIES ${ARG_LINK_LIBRARIES}
        INCLUDE_DIRECTORIES ${ARG_INCLUDE_DIRECTORIES}
        LABELS ${ARG_LABELS})
    else()
      wh_add_single_source_executable(
        bundle_target
        TARGET_PREFIX "${ARG_TARGET_PREFIX}"
        SOURCE_ROOT "${ARG_SOURCE_ROOT}"
        SOURCE_FILE "${source_file}"
        LINK_LIBRARIES ${ARG_LINK_LIBRARIES}
        INCLUDE_DIRECTORIES ${ARG_INCLUDE_DIRECTORIES})
    endif()
    add_dependencies("${ARG_AGGREGATE_TARGET}" "${bundle_target}")
  endforeach()
endfunction()
