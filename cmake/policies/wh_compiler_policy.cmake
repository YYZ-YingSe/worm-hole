include_guard(GLOBAL)

function(wh_setup_compiler_policy_targets)
  if(TARGET wh_compiler_policy_platform)
    return()
  endif()

  add_library(wh_compiler_policy_platform INTERFACE)
  add_library(wh::compiler_policy_platform ALIAS wh_compiler_policy_platform)

  target_compile_features(wh_compiler_policy_platform INTERFACE cxx_std_20)

  if(WIN32)
    target_compile_definitions(
      wh_compiler_policy_platform
      INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
  endif()
endfunction()
