#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/core/cursor_reader/detail/source.hpp"

namespace {

struct optional_source {
  using result_t = wh::core::result<int>;
  using try_t = std::optional<result_t>;

  [[nodiscard]] auto read() -> result_t { return result_t{7}; }
  [[nodiscard]] auto try_read() -> try_t { return try_t{result_t{9}}; }
  auto close() -> void {}
};

struct variant_source {
  using result_t = wh::core::result<int>;
  using try_t = std::variant<std::monostate, result_t>;

  [[nodiscard]] auto read() -> result_t { return result_t{11}; }
  [[nodiscard]] auto try_read() -> try_t { return try_t{result_t{13}}; }
  auto close() -> void {}
  [[nodiscard]] auto read_async() { return stdexec::just(result_t{17}); }
};

} // namespace

TEST_CASE("cursor reader source traits and try-result adapters cover direct optional and variant forms",
          "[UT][wh/core/cursor_reader/detail/source.hpp][try_result_traits][condition][branch][boundary]") {
  using result_t = wh::core::result<int>;
  using namespace wh::core::cursor_reader_detail;

  STATIC_REQUIRE(wh::core::cursor_reader_source<optional_source>);
  STATIC_REQUIRE(wh::core::cursor_reader_source<variant_source>);
  STATIC_REQUIRE(async_source<variant_source>);
  STATIC_REQUIRE(std::same_as<wh::core::cursor_reader_result_t<optional_source>,
                              result_t>);
  STATIC_REQUIRE(std::same_as<wh::core::cursor_reader_source_try_t<optional_source>,
                              std::optional<result_t>>);
  STATIC_REQUIRE(try_result_like<result_t, result_t>);
  STATIC_REQUIRE(try_result_like<std::optional<result_t>, result_t>);
  STATIC_REQUIRE(
      try_result_like<std::variant<std::monostate, result_t>, result_t>);

  REQUIRE_FALSE(try_result_traits<result_t, result_t>::is_pending(result_t{1}));
  REQUIRE(try_result_traits<result_t, result_t>::project(result_t{2}).value() == 2);

  REQUIRE(try_result_traits<std::optional<result_t>, result_t>::is_pending(
      std::optional<result_t>{}));
  REQUIRE_FALSE(try_result_traits<std::optional<result_t>, result_t>::is_pending(
      std::optional<result_t>{result_t{3}}));
  REQUIRE(try_result_traits<std::optional<result_t>, result_t>::project(
              std::optional<result_t>{result_t{4}})
              .value() == 4);

  using variant_t = std::variant<std::monostate, result_t>;
  REQUIRE(try_result_traits<variant_t, result_t>::is_pending(variant_t{}));
  REQUIRE_FALSE(try_result_traits<variant_t, result_t>::is_pending(
      variant_t{result_t{5}}));
  REQUIRE(
      try_result_traits<variant_t, result_t>::project(variant_t{result_t{6}})
          .value() == 6);
}

TEST_CASE("cursor reader source helpers normalize exceptions and default policy branches",
          "[UT][wh/core/cursor_reader/detail/source.hpp][default_policy][branch]") {
  using result_t = wh::core::result<int>;
  using policy_t =
      wh::core::cursor_reader_detail::default_policy<optional_source>;

  STATIC_REQUIRE(wh::core::cursor_reader_detail::policy_for<optional_source, policy_t>);

  REQUIRE_FALSE(policy_t::is_terminal(result_t{1}));
  REQUIRE(policy_t::is_terminal(result_t::failure(wh::core::errc::not_found)));
  REQUIRE(policy_t::is_pending(std::optional<result_t>{}));
  REQUIRE_FALSE(policy_t::is_pending(std::optional<result_t>{result_t{2}}));
  REQUIRE(policy_t::project_try(std::optional<result_t>{result_t{8}}).value() == 8);
  REQUIRE_FALSE(policy_t::pending().has_value());
  REQUIRE(policy_t::ready(result_t{9}).has_value());
  REQUIRE(policy_t::ready(result_t{9})->value() == 9);
  REQUIRE(policy_t::closed_result().has_error());
  REQUIRE(policy_t::closed_result().error() == wh::core::errc::channel_closed);
  REQUIRE(policy_t::internal_result().has_error());
  REQUIRE(policy_t::internal_result().error() == wh::core::errc::internal_error);

  optional_source source{};
  policy_t::set_automatic_close(source, true);
  policy_t::set_automatic_close(source, false);

  auto ptr = std::make_exception_ptr(std::runtime_error{"boom"});
  REQUIRE(wh::core::cursor_reader_detail::to_exception_ptr(ptr) == ptr);

  const auto wrapped =
      wh::core::cursor_reader_detail::to_exception_ptr(std::runtime_error{"boom"});
  REQUIRE(wrapped != nullptr);
}
