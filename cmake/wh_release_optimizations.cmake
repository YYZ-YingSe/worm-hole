include_guard(GLOBAL)

include(wh_release_policy)
include(wh_sanitizer_policy)

function(wh_apply_target_build_policies target_name)
  wh_apply_profile_instrumentation("${target_name}")
  wh_apply_release_optimizations("${target_name}")
endfunction()
