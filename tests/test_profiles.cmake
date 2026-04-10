include_guard(GLOBAL)

function(wh_apply_project_test_profiles)
  set(ft_root "${PROJECT_SOURCE_DIR}/tests/FT")

  wh_set_test_source_profile(
    SOURCE_FILE "${ft_root}/wh/compose/graph/stress_test.cpp"
    ADD_LABELS "ci.nightly" "ci.heavy"
    REMOVE_LABELS "ci.pr" "sanitizer.safe"
    WEIGHT 32
    TIMEOUT_SECONDS 600)
endfunction()
