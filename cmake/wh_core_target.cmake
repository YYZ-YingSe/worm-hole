include_guard(GLOBAL)

function(wh_setup_core_target)
  if(TARGET wh_core)
    return()
  endif()

  add_library(wh_core INTERFACE)
  add_library(wh::core ALIAS wh_core)

  target_include_directories(
    wh_core
    INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)

  target_compile_features(wh_core INTERFACE cxx_std_20)
  target_link_libraries(
    wh_core
    INTERFACE
      STDEXEC::stdexec
      rapidjson::rapidjson
      nlohmann_json::nlohmann_json
      minja::minja)

  if(UNIX AND NOT APPLE)
    target_link_libraries(wh_core INTERFACE atomic)
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(
        wh_core
        INTERFACE
          --system-header-prefix=stdexec/
          --system-header-prefix=exec/
          --system-header-prefix=rapidjson/
          --system-header-prefix=minja/
          --system-header-prefix=nlohmann/)
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

  install(DIRECTORY include/ DESTINATION include)
  install(DIRECTORY "${WH_STDEXEC_DIR}/include/"
          DESTINATION include/thirdy_party/stdexec)
  install(DIRECTORY "${WH_RAPIDJSON_DIR}/include/"
          DESTINATION include/thirdy_party/rapidjson)
  install(DIRECTORY "${WH_MINJA_DIR}/include/"
          DESTINATION include/thirdy_party/minja)
  install(DIRECTORY "${WH_NLOHMANN_JSON_DIR}/include/"
          DESTINATION include/thirdy_party/dependencies/nlohmann_json)
  install(
    TARGETS wh_core wh_stdexec wh_rapidjson wh_nlohmann_json wh_minja
    EXPORT worm_hole_targets)
  install(
    EXPORT worm_hole_targets
    NAMESPACE wh::
    DESTINATION lib/cmake/worm_hole)
endfunction()
