#include <array>
#include <span>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/address.hpp"

namespace {

using wh::core::address;
using wh::core::make_address;

} // namespace

TEST_CASE("address constructors preserve ordered path segments",
          "[UT][wh/core/address.hpp][address][branch][boundary]") {
  const address variadic{"graph", "planner", "tool"};

  constexpr const char *c_segments[] = {"graph", "worker"};
  const address from_c_array{c_segments};

  constexpr std::string_view sv_segments[] = {"root", "child", "leaf"};
  const address from_sv_array{sv_segments};

  REQUIRE(variadic.size() == 3U);
  REQUIRE(variadic.to_string() == "graph/planner/tool");
  REQUIRE(from_c_array.to_string() == "graph/worker");
  REQUIRE(from_sv_array.to_string() == "root/child/leaf");
}

TEST_CASE("address factories build from spans and initializer lists",
          "[UT][wh/core/address.hpp][make_address][branch]") {
  constexpr std::array<std::string_view, 3> segments{"alpha", "beta", "gamma"};

  const auto from_span =
      address::from_segments(std::span<const std::string_view>{segments.data(), segments.size()});
  const auto built_from_span =
      make_address(std::span<const std::string_view>{segments.data(), segments.size()});
  const auto built_from_list = make_address({"alpha", "beta", "gamma"});

  REQUIRE(from_span == built_from_span);
  REQUIRE(built_from_span == built_from_list);
  REQUIRE(from_span.to_string(".") == "alpha.beta.gamma");
}

TEST_CASE("address append parent and starts_with preserve hierarchy",
          "[UT][wh/core/address.hpp][address::append][condition][branch][boundary]") {
  const address root{"root"};
  const auto child = root.append("child");
  const auto leaf = child.append("leaf");

  REQUIRE(root.to_string() == "root");
  REQUIRE(child.to_string() == "root/child");
  REQUIRE(leaf.to_string() == "root/child/leaf");

  REQUIRE(child.parent() == root);
  REQUIRE(leaf.parent() == child);
  REQUIRE(root.parent().empty());

  REQUIRE(leaf.starts_with(root));
  REQUIRE(leaf.starts_with(child));
  REQUIRE(leaf.starts_with(leaf));
  REQUIRE_FALSE(root.starts_with(leaf));
  REQUIRE_FALSE(leaf.starts_with(address{"other"}));
}

TEST_CASE("address exposes emptiness segment views and equality",
          "[UT][wh/core/address.hpp][address::segments][branch][boundary]") {
  const address empty{};
  const address left{"a", "b"};
  const address same{"a", "b"};
  const address different{"a", "c"};

  REQUIRE(empty.empty());
  REQUIRE(empty.size() == 0U);
  REQUIRE(empty.to_string().empty());
  REQUIRE(empty.segments().empty());

  const auto segments = left.segments();
  REQUIRE(segments.size() == 2U);
  REQUIRE(segments[0] == "a");
  REQUIRE(segments[1] == "b");

  REQUIRE(left == same);
  REQUIRE(left != different);
}
