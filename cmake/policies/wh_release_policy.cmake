include_guard(GLOBAL)

include(CheckIPOSupported)

function(wh_enable_release_ipo target_name)
  if(NOT WH_ENABLE_RELEASE_IPO)
    return()
  endif()

  get_property(_ipo_checked GLOBAL PROPERTY WH_RELEASE_IPO_CHECKED)
  if(NOT _ipo_checked)
    check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_output LANGUAGES CXX)
    set_property(GLOBAL PROPERTY WH_RELEASE_IPO_CHECKED TRUE)
    set_property(GLOBAL PROPERTY WH_RELEASE_IPO_SUPPORTED "${_ipo_supported}")
    set_property(GLOBAL PROPERTY WH_RELEASE_IPO_OUTPUT "${_ipo_output}")

    if(NOT _ipo_supported)
      message(STATUS "Release IPO disabled: ${_ipo_output}")
    endif()
  endif()

  get_property(_ipo_supported GLOBAL PROPERTY WH_RELEASE_IPO_SUPPORTED)
  if(_ipo_supported)
    set_property(TARGET "${target_name}"
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()
endfunction()

function(wh_apply_release_optimizations target_name)
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR
            "wh_apply_release_optimizations target does not exist: ${target_name}")
  endif()

  get_target_property(target_type "${target_name}" TYPE)
  if(target_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()

  wh_enable_release_ipo("${target_name}")

  if(NOT WH_ENABLE_RELEASE_DEAD_STRIP)
    return()
  endif()

  if(MSVC)
    target_compile_options("${target_name}" PRIVATE "$<$<CONFIG:Release>:/Gy>")
  elseif(NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      "${target_name}" PRIVATE "$<$<CONFIG:Release>:-ffunction-sections>"
                               "$<$<CONFIG:Release>:-fdata-sections>")
  endif()

  if(NOT target_type STREQUAL "EXECUTABLE" AND
     NOT target_type STREQUAL "SHARED_LIBRARY" AND
     NOT target_type STREQUAL "MODULE_LIBRARY")
    return()
  endif()

  if(APPLE)
    target_link_options("${target_name}" PRIVATE
                        "$<$<CONFIG:Release>:LINKER:-dead_strip>")
  elseif(MSVC)
    target_link_options("${target_name}" PRIVATE
                        "$<$<CONFIG:Release>:/OPT:REF>")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT WIN32)
    target_link_options("${target_name}" PRIVATE
                        "$<$<CONFIG:Release>:LINKER:--gc-sections>")
  endif()
endfunction()
