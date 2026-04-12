include(wh_artifact_targets)

wh_setup_example_support_target()

wh_add_source_bundle(
  AGGREGATE_TARGET "${WH_TARGET_EXAMPLES}"
  TARGET_PREFIX "wh_example"
  SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
  GLOB "*.cpp"
  LINK_LIBRARIES "${WH_TARGET_EXAMPLE_SUPPORT}")

wh_register_project_artifact(ENABLED "${WH_TARGET_EXAMPLES}")
