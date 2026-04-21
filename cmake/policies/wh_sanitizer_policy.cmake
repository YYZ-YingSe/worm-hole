include_guard(GLOBAL)

function(wh_apply_profile_instrumentation target_name)
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR
            "wh_apply_profile_instrumentation target does not exist: ${target_name}")
  endif()

  get_target_property(target_type "${target_name}" TYPE)
  if(target_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()

  set(linkable_target FALSE)
  if(target_type STREQUAL "EXECUTABLE" OR target_type STREQUAL "SHARED_LIBRARY" OR
     target_type STREQUAL "MODULE_LIBRARY")
    set(linkable_target TRUE)
  endif()

  if(WH_BUILD_PROFILE STREQUAL "asan-ubsan")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      message(FATAL_ERROR
              "asan-ubsan profile requires a Clang or GNU-compatible compiler")
    endif()
    target_compile_options(
      "${target_name}" PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
    if(linkable_target)
      target_link_options("${target_name}" PRIVATE -fsanitize=address,undefined
                          -fno-omit-frame-pointer)
    endif()
    return()
  endif()

  if(WH_BUILD_PROFILE STREQUAL "tsan")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      message(FATAL_ERROR "tsan profile requires a Clang or GNU-compatible compiler")
    endif()
    target_compile_options(
      "${target_name}" PRIVATE -fsanitize=thread -fno-omit-frame-pointer -O1 -g)
    if(linkable_target)
      target_link_options("${target_name}" PRIVATE -fsanitize=thread
                          -fno-omit-frame-pointer)
    endif()
    return()
  endif()

  if(WH_BUILD_PROFILE STREQUAL "coverage")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "coverage profile requires Clang-compatible instrumentation")
    endif()
    target_compile_options(
      "${target_name}" PRIVATE -fprofile-instr-generate -fcoverage-mapping -O0 -g)
    if(linkable_target)
      target_link_options("${target_name}" PRIVATE -fprofile-instr-generate
                          -fcoverage-mapping)
    endif()
  endif()
endfunction()
