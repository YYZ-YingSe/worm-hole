include_guard(GLOBAL)

include(wh_compiler_policy)
include(wh_warning_policy)
include(wh_allocator_policy)

function(wh_setup_module_policy_targets)
  if(TARGET wh_module_policy_core)
    return()
  endif()

  wh_setup_compiler_policy_targets()
  wh_setup_warning_policy_targets()
  wh_setup_allocator_policy_targets()

  add_library(wh_module_policy_core INTERFACE)
  add_library(wh::module_policy_core ALIAS wh_module_policy_core)
  target_link_libraries(
    wh_module_policy_core
    INTERFACE
      wh::compiler_policy_platform
      wh::warning_policy_strict
      wh::external_headers_policy)

  add_library(wh_module_policy_tests INTERFACE)
  add_library(wh::module_policy_tests ALIAS wh_module_policy_tests)
  target_link_libraries(
    wh_module_policy_tests
    INTERFACE
      wh::module_policy_core
      wh::allocator_policy_runtime)

  add_library(wh_module_policy_examples INTERFACE)
  add_library(wh::module_policy_examples ALIAS wh_module_policy_examples)
  target_link_libraries(
    wh_module_policy_examples
    INTERFACE
      wh::module_policy_core
      wh::allocator_policy_runtime)

  add_library(wh_module_policy_benchmarks INTERFACE)
  add_library(wh::module_policy_benchmarks ALIAS wh_module_policy_benchmarks)
  target_link_libraries(
    wh_module_policy_benchmarks
    INTERFACE
      wh::module_policy_core
      wh::allocator_policy_runtime)

  add_library(wh_module_policy_analysis INTERFACE)
  add_library(wh::module_policy_analysis ALIAS wh_module_policy_analysis)
  target_link_libraries(
    wh_module_policy_analysis
    INTERFACE
      wh::compiler_policy_platform
      wh::warning_policy_project
      wh::external_headers_policy)
endfunction()
