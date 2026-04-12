include(wh_artifact_targets)

wh_setup_benchmark_support_target()

wh_add_source_bundle(
  AGGREGATE_TARGET "${WH_TARGET_BENCHMARKS}"
  TARGET_PREFIX "wh_benchmark"
  SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
  GLOB "*.cpp"
  LINK_LIBRARIES "${WH_TARGET_BENCHMARK_SUPPORT}")

wh_register_project_artifact(ENABLED "${WH_TARGET_BENCHMARKS}")
