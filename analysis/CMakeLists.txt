include(wh_source_targets)

wh_add_source_bundle(
  AGGREGATE_TARGET "wh_analysis_drivers"
  TARGET_PREFIX "wh_analysis"
  SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
  GLOB "*.cpp"
  LINK_LIBRARIES wh::core)
