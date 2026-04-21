include_guard(GLOBAL)

include(wh_release_optimizations)

function(wh_normalize_target_token out_var value)
  string(MAKE_C_IDENTIFIER "${value}" normalized)
  string(REGEX REPLACE "_+" "_" normalized "${normalized}")
  string(REGEX REPLACE "^_+" "" normalized "${normalized}")
  string(REGEX REPLACE "_+$" "" normalized "${normalized}")
  set("${out_var}" "${normalized}" PARENT_SCOPE)
endfunction()

function(wh_source_target_name out_var target_prefix source_root source_file)
  cmake_path(ABSOLUTE_PATH source_root NORMALIZE OUTPUT_VARIABLE absolute_root)
  cmake_path(ABSOLUTE_PATH source_file NORMALIZE OUTPUT_VARIABLE absolute_source)
  cmake_path(RELATIVE_PATH absolute_source BASE_DIRECTORY "${absolute_root}"
             OUTPUT_VARIABLE relative_source)
  string(REGEX REPLACE "\\.[^.]+$" "" relative_stem "${relative_source}")
  wh_normalize_target_token(normalized_stem "${relative_stem}")

  if(normalized_stem STREQUAL "")
    message(FATAL_ERROR
            "Cannot derive target name from source file: ${source_file}")
  endif()

  set("${out_var}" "${target_prefix}_${normalized_stem}" PARENT_SCOPE)
endfunction()

function(wh_add_single_source_executable out_var)
  set(options)
  set(one_value_args TARGET_PREFIX SOURCE_ROOT SOURCE_FILE)
  set(multi_value_args LINK_LIBRARIES INCLUDE_DIRECTORIES)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  foreach(required_arg TARGET_PREFIX SOURCE_ROOT SOURCE_FILE)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR
              "wh_add_single_source_executable missing ${required_arg}")
    endif()
  endforeach()

  wh_source_target_name(target_name "${ARG_TARGET_PREFIX}" "${ARG_SOURCE_ROOT}"
                        "${ARG_SOURCE_FILE}")

  add_executable("${target_name}" "${ARG_SOURCE_FILE}")
  if(ARG_LINK_LIBRARIES)
    target_link_libraries("${target_name}" PRIVATE ${ARG_LINK_LIBRARIES})
  endif()
  if(ARG_INCLUDE_DIRECTORIES)
    target_include_directories("${target_name}" PRIVATE
                               ${ARG_INCLUDE_DIRECTORIES})
  endif()
  wh_apply_target_build_policies("${target_name}")

  set("${out_var}" "${target_name}" PARENT_SCOPE)
endfunction()
