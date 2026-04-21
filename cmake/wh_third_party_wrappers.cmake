include_guard(GLOBAL)

function(wh_collect_header_candidates out_var source_root)
  set(candidates)
  foreach(pattern IN ITEMS "*.h" "*.hh" "*.hpp" "*.hxx" "*.inc" "*.ipp" "*.tcc")
    file(GLOB_RECURSE matched_headers
         RELATIVE "${source_root}"
         CONFIGURE_DEPENDS
         "${source_root}/${pattern}")
    list(APPEND candidates ${matched_headers})
  endforeach()

  list(REMOVE_DUPLICATES candidates)
  list(SORT candidates)
  set("${out_var}" "${candidates}" PARENT_SCOPE)
endfunction()

function(wh_normalize_wrapper_token out_var value)
  string(MAKE_C_IDENTIFIER "${value}" normalized)
  string(REGEX REPLACE "_+" "_" normalized "${normalized}")
  string(REGEX REPLACE "^_+" "" normalized "${normalized}")
  string(REGEX REPLACE "_+$" "" normalized "${normalized}")
  set("${out_var}" "${normalized}" PARENT_SCOPE)
endfunction()

function(wh_generate_system_header_wrapper_tree out_var)
  set(options)
  set(one_value_args NAME SOURCE_ROOT)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "" ${ARGN})

  foreach(required_arg NAME SOURCE_ROOT)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR
              "wh_generate_system_header_wrapper_tree missing ${required_arg}")
    endif()
  endforeach()

  wh_normalize_wrapper_token(wrapper_token "${ARG_NAME}")
  set(wrapper_root
      "${CMAKE_BINARY_DIR}/generated-third-party/${wrapper_token}")
  file(MAKE_DIRECTORY "${wrapper_root}")

  wh_collect_header_candidates(relative_headers "${ARG_SOURCE_ROOT}")

  foreach(relative_header IN LISTS relative_headers)
    get_filename_component(wrapper_dir "${wrapper_root}/${relative_header}"
                           DIRECTORY)
    file(MAKE_DIRECTORY "${wrapper_dir}")

    set(source_header "${ARG_SOURCE_ROOT}/${relative_header}")
    file(TO_CMAKE_PATH "${source_header}" source_header_cmake_path)

    file(
      WRITE "${wrapper_root}/${relative_header}" [[#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC system_header
#endif
]] "#include \"${source_header_cmake_path}\"\n")
  endforeach()

  set("${out_var}" "${wrapper_root}" PARENT_SCOPE)
endfunction()
