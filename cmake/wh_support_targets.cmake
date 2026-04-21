include_guard(GLOBAL)

function(wh_define_support_target target_name)
  set(options)
  set(one_value_args)
  set(multi_value_args LINK_LIBRARIES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  if(TARGET "${target_name}")
    return()
  endif()

  add_library("${target_name}" INTERFACE)
  if(ARG_LINK_LIBRARIES)
    target_link_libraries("${target_name}" INTERFACE ${ARG_LINK_LIBRARIES})
  endif()
  if(ARG_INCLUDE_DIRECTORIES)
    target_include_directories("${target_name}" INTERFACE
                               ${ARG_INCLUDE_DIRECTORIES})
  endif()
  if(ARG_COMPILE_DEFINITIONS)
    target_compile_definitions("${target_name}" INTERFACE
                               ${ARG_COMPILE_DEFINITIONS})
  endif()
endfunction()
