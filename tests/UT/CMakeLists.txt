if(WH_BUILD_UT)
  wh_add_source_bundle(
    RECURSE
    AGGREGATE_TARGET "${WH_TARGET_UT_TESTS}"
    TARGET_PREFIX "wh_ut"
    SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    GLOB "wh/*_ut.cpp"
    TEST_PREFIX "UT::"
    LINK_LIBRARIES "${WH_TARGET_UT_SUPPORT}"
    LABELS "UT")

  wh_register_project_artifact(TESTS "${WH_TARGET_UT_TESTS}")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
  add_custom_target(
    wh_ut_manifest_audit
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/testing/verify_ut_manifest.py"
      "--include-root" "${PROJECT_SOURCE_DIR}/include/wh"
      "--ut-root" "${PROJECT_SOURCE_DIR}/tests/UT"
      "--verify-existing"
      "--report-missing"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Auditing UT/header mapping coverage"
    VERBATIM)

  add_custom_target(
    wh_ut_quality_audit
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/testing/audit_ut_quality.py"
      "--include-root" "${PROJECT_SOURCE_DIR}/include/wh"
      "--ut-root" "${PROJECT_SOURCE_DIR}/tests/UT"
      "--strict"
      "--summary-json" "${CMAKE_BINARY_DIR}/wh_ut_quality_audit.json"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Auditing UT quality tags and boundary coverage"
    VERBATIM)
endif()
