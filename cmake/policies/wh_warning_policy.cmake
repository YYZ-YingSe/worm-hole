include_guard(GLOBAL)

function(wh_setup_warning_policy_targets)
  if(TARGET wh_warning_policy_project)
    return()
  endif()

  add_library(wh_warning_policy_project INTERFACE)
  add_library(wh::warning_policy_project ALIAS wh_warning_policy_project)

  add_library(wh_warning_policy_werror INTERFACE)
  add_library(wh::warning_policy_werror ALIAS wh_warning_policy_werror)

  add_library(wh_warning_policy_strict INTERFACE)
  add_library(wh::warning_policy_strict ALIAS wh_warning_policy_strict)

  add_library(wh_external_headers_policy INTERFACE)
  add_library(wh::external_headers_policy ALIAS wh_external_headers_policy)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      wh_warning_policy_project
      INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast)

    if(WH_WARNINGS_AS_ERRORS)
      target_compile_options(wh_warning_policy_werror INTERFACE -Werror)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(
        wh_external_headers_policy
        INTERFACE
          --system-header-prefix=stdexec/
          --system-header-prefix=exec/
          --system-header-prefix=rapidjson/
          --system-header-prefix=minja/
          --system-header-prefix=nlohmann/
          --system-header-prefix=benchmark/
          --system-header-prefix=catch2/
          --system-header-prefix=mimalloc/)
    endif()
  elseif(MSVC)
    target_compile_options(wh_warning_policy_project INTERFACE /W4)

    if(WH_WARNINGS_AS_ERRORS)
      target_compile_options(wh_warning_policy_werror INTERFACE /WX)
    endif()

    target_compile_options(
      wh_external_headers_policy
      INTERFACE
        /external:W0
        /external:anglebrackets)
  endif()

  target_link_libraries(
    wh_warning_policy_strict
    INTERFACE
      wh::warning_policy_project
      wh::warning_policy_werror)
endfunction()
