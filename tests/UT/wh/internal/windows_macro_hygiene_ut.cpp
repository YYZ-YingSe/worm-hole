#include <catch2/catch_test_macros.hpp>

#include "wh/core/compiler.hpp"
#include "wh/core/run_context.hpp"
#include "wh/internal/serialization.hpp"

#if WH_OS_WINDOWS && defined(GetObject)
#error "Win32 GetObject macro leaked into project headers"
#endif

TEST_CASE("windows sdk wrapper does not leak GetObject macro into serialization headers",
          "[UT][wh/internal/windows_sdk.hpp][windows][macro][hygiene]") {
  SUCCEED();
}
