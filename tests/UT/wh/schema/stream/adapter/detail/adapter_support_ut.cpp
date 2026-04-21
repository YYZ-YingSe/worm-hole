#include <catch2/catch_test_macros.hpp>

#include "wh/schema/stream/adapter/detail/adapter_support.hpp"
#include "wh/schema/stream/core/types.hpp"

namespace {

struct configurable_reader {
  bool automatic_close{true};
  bool closed{false};
  bool source_closed{false};
  int set_calls{0};
  int close_calls{0};
  wh::core::error_code close_error{};

  auto set_automatic_close(const wh::schema::stream::auto_close_options &options) -> void {
    automatic_close = options.enabled;
    ++set_calls;
  }

  auto is_source_closed() const -> bool { return source_closed; }

  auto close() -> wh::core::result<void> {
    ++close_calls;
    closed = true;
    source_closed = true;
    if (close_error.failed()) {
      return wh::core::result<void>::failure(close_error);
    }
    return {};
  }

  auto is_closed() const -> bool { return closed; }
};

struct plain_reader {
  bool closed{false};
  int close_calls{0};

  auto close() -> wh::core::result<void> {
    ++close_calls;
    closed = true;
    return {};
  }

  auto is_closed() const -> bool { return closed; }
};

static_assert(wh::schema::stream::detail::auto_close_settable<configurable_reader>);
static_assert(!wh::schema::stream::detail::auto_close_settable<plain_reader>);
static_assert(wh::schema::stream::detail::source_closed_queryable<configurable_reader>);
static_assert(!wh::schema::stream::detail::source_closed_queryable<plain_reader>);

} // namespace

TEST_CASE("adapter support applies optional auto-close hooks and close-state queries",
          "[UT][wh/schema/stream/adapter/detail/"
          "adapter_support.hpp][set_automatic_close_if_supported][condition][branch]") {
  configurable_reader configurable{};
  plain_reader plain{};

  wh::schema::stream::detail::set_automatic_close_if_supported(
      configurable, wh::schema::stream::auto_close_disabled);
  wh::schema::stream::detail::set_automatic_close_if_supported(
      plain, wh::schema::stream::auto_close_disabled);

  REQUIRE(configurable.set_calls == 1);
  REQUIRE_FALSE(configurable.automatic_close);
  REQUIRE_FALSE(wh::schema::stream::detail::is_source_closed_if_supported(configurable));
  REQUIRE_FALSE(wh::schema::stream::detail::is_source_closed_if_supported(plain));

  plain.closed = true;
  REQUIRE(wh::schema::stream::detail::is_source_closed_if_supported(plain));

  auto error_chunk =
      wh::schema::stream::detail::make_error_chunk<wh::schema::stream::stream_chunk<int>>(
          wh::core::errc::timeout);
  REQUIRE(error_chunk.error == wh::core::errc::timeout);
}

TEST_CASE("adapter support stream_adapter_state closes source once and tolerates channel_closed",
          "[UT][wh/schema/stream/adapter/detail/"
          "adapter_support.hpp][stream_adapter_state::close_source][branch][boundary]") {
  wh::schema::stream::detail::stream_adapter_state state{};
  configurable_reader configurable{};

  state.close_source_if_enabled(configurable);
  REQUIRE(configurable.close_calls == 1);
  REQUIRE(state.source_closed);

  auto repeated = state.close_source(configurable);
  REQUIRE(repeated.has_value());
  REQUIRE(configurable.close_calls == 1);

  wh::schema::stream::detail::stream_adapter_state disabled_state{};
  disabled_state.automatic_close = false;
  plain_reader plain{};
  disabled_state.close_source_if_enabled(plain);
  REQUIRE(plain.close_calls == 0);
  auto explicit_close = disabled_state.close_source(plain);
  REQUIRE(explicit_close.has_value());
  REQUIRE(plain.close_calls == 1);

  wh::schema::stream::detail::stream_adapter_state tolerant_state{};
  configurable_reader tolerant{};
  tolerant.close_error = wh::core::errc::channel_closed;
  auto tolerant_status = tolerant_state.close_source(tolerant);
  REQUIRE(tolerant_status.has_value());
  REQUIRE(tolerant.close_calls == 1);
}
