if(WH_BUILD_FT AND WH_TEST_EXECUTABLE_LAYOUT STREQUAL "source")
  wh_add_source_bundle(
    RECURSE
    AGGREGATE_TARGET "${WH_TARGET_FT_TESTS}"
    TARGET_PREFIX "wh_ft"
    SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    GLOB "wh/*_test.cpp"
    TEST_PREFIX "FT::"
    LINK_LIBRARIES "${WH_TARGET_FT_SUPPORT}"
    LABELS "FT")

  wh_register_project_artifact(TESTS "${WH_TARGET_FT_TESTS}")
endif()
