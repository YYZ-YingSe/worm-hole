cmake_minimum_required(VERSION 3.25)

project(worm_hole VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(WH_BUILD_TESTING "Build worm-hole tests" ON)
option(WH_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(WH_REQUIRE_GIT_LOCKED_THIRDY_PARTY "Require git-locked thirdy_party deps" ON)

set(WH_THIRDY_PARTY_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdy_party"
    CACHE PATH
    "thirdy_party root directory")

set(WH_STDEXEC_DIR "${WH_THIRDY_PARTY_DIR}/stdexec" CACHE PATH "stdexec source directory")
set(WH_RAPIDJSON_DIR "${WH_THIRDY_PARTY_DIR}/rapidjson" CACHE PATH "rapidjson source directory")
set(WH_CATCH2_DIR "${WH_THIRDY_PARTY_DIR}/catch2" CACHE PATH "catch2 source directory")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

  if(NOT git_rc EQUAL 0 OR NOT git_head_len EQUAL 40 OR NOT git_head_invalid STREQUAL "")
    message(FATAL_ERROR "${label} is not git-locked at a commit: ${path}")
  endif()

  message(STATUS "${label} pinned: ${git_head}")
endfunction()

wh_require_git_locked_dir("${WH_STDEXEC_DIR}" "stdexec")
wh_require_git_locked_dir("${WH_RAPIDJSON_DIR}" "rapidjson")

if(BUILD_TESTING AND WH_BUILD_TESTING)
  wh_require_git_locked_dir("${WH_CATCH2_DIR}" "catch2")
endif()

set(wh_stdexec_header "${WH_STDEXEC_DIR}/include/stdexec/execution.hpp")
if(NOT EXISTS "${wh_stdexec_header}")
  message(FATAL_ERROR
    "stdexec header missing: ${wh_stdexec_header}\n"
    "Hint: sync thirdy_party submodules or pass -DWH_STDEXEC_DIR=<path>")
endif()

set(wh_rapidjson_header "${WH_RAPIDJSON_DIR}/include/rapidjson/document.h")
if(NOT EXISTS "${wh_rapidjson_header}")
  message(FATAL_ERROR
    "rapidjson header missing: ${wh_rapidjson_header}\n"
    "Hint: sync thirdy_party submodules or pass -DWH_RAPIDJSON_DIR=<path>")
endif()

add_library(wh_stdexec INTERFACE)
add_library(STDEXEC::stdexec ALIAS wh_stdexec)
target_include_directories(
  wh_stdexec
  BEFORE
  INTERFACE
    $<BUILD_INTERFACE:${WH_STDEXEC_DIR}/include>
    $<INSTALL_INTERFACE:include/thirdy_party/stdexec>)

add_library(wh_rapidjson INTERFACE)
add_library(rapidjson::rapidjson ALIAS wh_rapidjson)
target_include_directories(
  wh_rapidjson
  BEFORE
  INTERFACE
    $<BUILD_INTERFACE:${WH_RAPIDJSON_DIR}/include>
    $<INSTALL_INTERFACE:include/thirdy_party/rapidjson>)

add_library(wh_core INTERFACE)
add_library(wh::core ALIAS wh_core)

target_include_directories(
  wh_core
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(wh_core INTERFACE cxx_std_20)
target_link_libraries(wh_core INTERFACE STDEXEC::stdexec rapidjson::rapidjson)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(
      wh_core
      INTERFACE
        --system-header-prefix=stdexec/
        --system-header-prefix=rapidjson/)
  endif()

  target_compile_options(
    wh_core
    INTERFACE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast)
  if(WH_WARNINGS_AS_ERRORS)
    target_compile_options(wh_core INTERFACE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(wh_core INTERFACE /W4)
  if(WH_WARNINGS_AS_ERRORS)
    target_compile_options(wh_core INTERFACE /WX)
  endif()
endif()

if(BUILD_TESTING AND WH_BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY "${WH_STDEXEC_DIR}/include/" DESTINATION include/thirdy_party/stdexec)
install(DIRECTORY "${WH_RAPIDJSON_DIR}/include/" DESTINATION include/thirdy_party/rapidjson)
install(TARGETS wh_core wh_stdexec wh_rapidjson EXPORT worm_hole_targets)
install(
  EXPORT worm_hole_targets
  NAMESPACE wh::
  DESTINATION lib/cmake/worm_hole)
