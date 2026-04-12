#include <catch2/catch_test_macros.hpp>

#include "wh/tool/utils/common.hpp"

TEST_CASE("to_camel_case normalizes separators and capitalization",
          "[UT][wh/tool/utils/common.hpp][to_camel_case][branch][boundary]") {
  REQUIRE(wh::tool::utils::to_camel_case("") == "");
  REQUIRE(wh::tool::utils::to_camel_case("search") == "Search");
  REQUIRE(wh::tool::utils::to_camel_case("tool_name") == "ToolName");
  REQUIRE(wh::tool::utils::to_camel_case("tool-name test") ==
          "ToolNameTest");
}

TEST_CASE("to_camel_case skips repeated separators and preserves existing capitals",
          "[UT][wh/tool/utils/common.hpp][to_camel_case][condition][boundary]") {
  REQUIRE(wh::tool::utils::to_camel_case("__tool__name__") == "ToolName");
  REQUIRE(wh::tool::utils::to_camel_case("HTTP_server") == "HTTPServer");
}
