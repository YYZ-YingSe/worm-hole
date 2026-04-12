include_guard(GLOBAL)

function(wh_require_git_locked_dir path label)
  if(NOT WH_REQUIRE_GIT_LOCKED_THIRDY_PARTY)
    return()
  endif()

  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "${label} missing: ${path}")
  endif()

  execute_process(
    COMMAND git -C "${path}" rev-parse --verify HEAD
    OUTPUT_VARIABLE git_head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE git_rc)

  string(LENGTH "${git_head}" git_head_len)
  string(REGEX MATCH "[^0-9a-f]" git_head_invalid "${git_head}")

  if(NOT git_rc EQUAL 0 OR NOT git_head_len EQUAL 40 OR
     NOT git_head_invalid STREQUAL "")
    message(FATAL_ERROR "${label} is not git-locked at a commit: ${path}")
  endif()

  message(STATUS "${label} pinned: ${git_head}")
endfunction()

function(wh_require_source_header header_path label hint)
  if(EXISTS "${header_path}")
    return()
  endif()

  message(FATAL_ERROR
          "${label} missing: ${header_path}\n"
          "Hint: ${hint}")
endfunction()

function(wh_add_interface_include_library target_name alias_name build_include
         install_include)
  if(TARGET "${target_name}")
    return()
  endif()

  add_library("${target_name}" INTERFACE)
  add_library("${alias_name}" ALIAS "${target_name}")
  target_include_directories(
    "${target_name}"
    BEFORE
    INTERFACE
      $<BUILD_INTERFACE:${build_include}>
      $<INSTALL_INTERFACE:${install_include}>)
endfunction()

function(wh_setup_third_party)
  wh_require_git_locked_dir("${WH_STDEXEC_DIR}" "stdexec")
  wh_require_git_locked_dir("${WH_RAPIDJSON_DIR}" "rapidjson")
  wh_require_git_locked_dir("${WH_MINJA_DIR}" "minja")
  wh_require_git_locked_dir("${WH_NLOHMANN_JSON_DIR}" "nlohmann_json")

  if(BUILD_TESTING AND WH_BUILD_TESTING)
    wh_require_git_locked_dir("${WH_CATCH2_DIR}" "catch2")
  endif()

  wh_require_source_header(
    "${WH_STDEXEC_DIR}/include/stdexec/execution.hpp"
    "stdexec header"
    "sync thirdy_party submodules or pass -DWH_STDEXEC_DIR=<path>")
  wh_require_source_header(
    "${WH_RAPIDJSON_DIR}/include/rapidjson/document.h"
    "rapidjson header"
    "sync thirdy_party submodules or pass -DWH_RAPIDJSON_DIR=<path>")
  wh_require_source_header(
    "${WH_MINJA_DIR}/include/minja/minja.hpp"
    "minja header"
    "sync thirdy_party submodules or pass -DWH_MINJA_DIR=<path>")
  wh_require_source_header(
    "${WH_NLOHMANN_JSON_DIR}/include/nlohmann/json.hpp"
    "nlohmann_json header"
    "sync thirdy_party submodules or pass -DWH_NLOHMANN_JSON_DIR=<path>")

  wh_add_interface_include_library(
    wh_stdexec
    STDEXEC::stdexec
    "${WH_STDEXEC_DIR}/include"
    "include/thirdy_party/stdexec")
  wh_add_interface_include_library(
    wh_rapidjson
    rapidjson::rapidjson
    "${WH_RAPIDJSON_DIR}/include"
    "include/thirdy_party/rapidjson")
  wh_add_interface_include_library(
    wh_nlohmann_json
    nlohmann_json::nlohmann_json
    "${WH_NLOHMANN_JSON_DIR}/include"
    "include/thirdy_party/dependencies/nlohmann_json")
  wh_add_interface_include_library(
    wh_minja
    minja::minja
    "${WH_MINJA_DIR}/include"
    "include/thirdy_party/minja")

  target_link_libraries(wh_minja INTERFACE nlohmann_json::nlohmann_json)
endfunction()
