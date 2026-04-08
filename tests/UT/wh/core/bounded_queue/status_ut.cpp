#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "wh/core/bounded_queue/status.hpp"

TEST_CASE("bounded_queue_status to_string covers every known enum branch and unknown fallback",
          "[UT][wh/core/bounded_queue/status.hpp][to_string][condition][branch][boundary]") {
  REQUIRE(wh::core::to_string(wh::core::bounded_queue_status::success) ==
          "success");
  REQUIRE(wh::core::to_string(wh::core::bounded_queue_status::empty) == "empty");
  REQUIRE(wh::core::to_string(wh::core::bounded_queue_status::full) == "full");
  REQUIRE(wh::core::to_string(wh::core::bounded_queue_status::closed) ==
          "closed");
  REQUIRE(wh::core::to_string(wh::core::bounded_queue_status::busy) == "busy");
  REQUIRE(wh::core::to_string(wh::core::bounded_queue_status::busy_async) ==
          "busy_async");
  REQUIRE(wh::core::to_string(static_cast<wh::core::bounded_queue_status>(255)) ==
          "unknown");
}

TEST_CASE("bounded_queue_status stream output delegates to the symbolic formatter",
          "[UT][wh/core/bounded_queue/status.hpp][operator<<][branch]") {
  std::ostringstream stream{};
  stream << wh::core::bounded_queue_status::closed << ','
         << wh::core::bounded_queue_status::busy_async;
  REQUIRE(stream.str() == "closed,busy_async");
}
