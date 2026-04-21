include_guard(GLOBAL)

include(wh_atomic_runtime)
include(wh_module_policy)

function(wh_setup_core_target)
  if(TARGET wh_core)
    return()
  endif()

  wh_setup_atomic_runtime_target()
  wh_setup_module_policy_targets()

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
      wh::third_party_headers
      wh_atomic_runtime
      wh::module_policy_core)

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
    TARGETS wh_atomic_runtime
            wh_core
            wh_third_party_headers
            wh_stdexec
            wh_rapidjson
            wh_nlohmann_json
            wh_minja
            wh_compiler_policy_platform
            wh_warning_policy_project
            wh_warning_policy_werror
            wh_warning_policy_strict
            wh_external_headers_policy
            wh_allocator_policy_runtime
            wh_module_policy_core
            wh_module_policy_tests
            wh_module_policy_examples
            wh_module_policy_benchmarks
            wh_module_policy_analysis
    EXPORT worm_hole_targets)
  install(
    EXPORT worm_hole_targets
    NAMESPACE wh::
    DESTINATION lib/cmake/worm_hole)
endfunction()
