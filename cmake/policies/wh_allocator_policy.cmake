include_guard(GLOBAL)

function(wh_setup_allocator_policy_targets)
  if(TARGET wh_allocator_policy_runtime)
    return()
  endif()

  add_library(wh_allocator_policy_runtime INTERFACE)
  add_library(wh::allocator_policy_runtime ALIAS wh_allocator_policy_runtime)

  if(WH_EXPERIMENT_MIMALLOC)
    if(NOT TARGET mimalloc)
      message(FATAL_ERROR
              "WH_EXPERIMENT_MIMALLOC is ON but target 'mimalloc' was not configured")
    endif()
    target_link_libraries(wh_allocator_policy_runtime INTERFACE mimalloc)
  endif()
endfunction()
