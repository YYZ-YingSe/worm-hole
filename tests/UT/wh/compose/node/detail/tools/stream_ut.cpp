#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/compose/node/detail/tools/stream.hpp"

namespace {

[[nodiscard]] auto read_text(wh::compose::graph_value &value) -> std::string {
  auto *typed = wh::core::any_cast<std::string>(&value);
  REQUIRE(typed != nullptr);
  return *typed;
}

} // namespace

TEST_CASE("tools stream reader maps merged lanes through bindings supports stop and proxies close state",
          "[UT][wh/compose/node/detail/tools/stream.hpp][tool_event_stream_reader::read_async][condition][branch][concurrency]") {
  wh::compose::tools_options options{};
  options.middleware.push_back({
      .after =
          [](const wh::compose::tool_call &, wh::compose::graph_value &value,
             const wh::tool::call_scope &) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<std::string>(&value);
        REQUIRE(typed != nullptr);
        value = wh::compose::graph_value{*typed + "-after"};
        return {};
      },
  });

  auto [writer, source] = wh::compose::make_graph_stream(4U);
  std::vector<wh::schema::stream::named_stream_reader<wh::compose::graph_stream_reader>>
      lanes{};
  lanes.push_back({"call-1", std::move(source), false});

  wh::compose::detail::tool_stream_binding_map bindings{};
  bindings.emplace(
      "call-1",
      wh::compose::detail::tool_stream_binding{
          .call =
              wh::compose::tool_call{
                  .call_id = "call-1",
                  .tool_name = "echo",
                  .arguments = "payload",
              },
          .context = wh::core::run_context{},
      });

  wh::compose::detail::tool_event_stream_reader wrapped{
      wh::compose::detail::make_graph_merge_reader(std::move(lanes)),
      std::move(bindings),
      wh::compose::detail::collect_tool_afters(options)};

  auto pending = wrapped.try_read();
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_signal>(pending));
  REQUIRE(std::get<wh::schema::stream::stream_signal>(pending) ==
          wh::schema::stream::stream_pending);

  wh::testing::helper::sender_capture<> stopped{};
  stdexec::inplace_stop_source stop_source{};
  auto operation = stdexec::connect(
      wrapped.read_async(),
      wh::testing::helper::sender_capture_receiver{
          &stopped,
          wh::testing::helper::make_scheduler_env(stdexec::inline_scheduler{},
                                                  stop_source.get_token()),
      });
  stdexec::start(operation);
  stop_source.request_stop();

  REQUIRE(stopped.ready.try_acquire_for(std::chrono::milliseconds(100)));
  REQUIRE(stopped.terminal ==
          wh::testing::helper::sender_terminal_kind::stopped);

  REQUIRE(writer.try_write(wh::compose::graph_value{std::string{"chunk"}})
              .has_value());
  REQUIRE(writer.close().has_value());

  auto resumed = wrapped.read();
  REQUIRE(resumed.has_value());
  REQUIRE_FALSE(resumed.value().eof);
  REQUIRE(resumed.value().value.has_value());
  auto *event =
      wh::core::any_cast<wh::compose::tool_event>(&*resumed.value().value);
  REQUIRE(event != nullptr);
  REQUIRE(event->call_id == "call-1");
  REQUIRE(event->tool_name == "echo");
  REQUIRE(read_text(event->value) == "chunk-after");

  auto eof = wrapped.read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);

  REQUIRE_FALSE(wrapped.is_closed_impl());
  REQUIRE(wrapped.is_source_closed());
  wrapped.set_automatic_close({});
  REQUIRE(wrapped.close_impl().has_value());
  REQUIRE(wrapped.is_closed_impl());
}

TEST_CASE("tools stream helpers filter after middleware and surface missing binding or after error failures",
          "[UT][wh/compose/node/detail/tools/stream.hpp][make_tool_event_stream_reader][branch][boundary]") {
  wh::compose::tools_options options{};
  options.middleware.push_back({});
  options.middleware.push_back({
      .after =
          [](const wh::compose::tool_call &, wh::compose::graph_value &value,
             const wh::tool::call_scope &) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<int>(&value);
        REQUIRE(typed != nullptr);
        value = wh::compose::graph_value{*typed + 1};
        return {};
      },
  });

  auto afters = wh::compose::detail::collect_tool_afters(options);
  REQUIRE(afters.size() == 1U);

  auto [writer_ok, reader_ok] = wh::compose::make_graph_stream(2U);
  REQUIRE(writer_ok.try_write(wh::compose::graph_value{1}).has_value());
  REQUIRE(writer_ok.close().has_value());

  wh::compose::detail::tool_stream_binding_map ok_bindings{};
  ok_bindings.emplace(
      "call",
      wh::compose::detail::tool_stream_binding{
          .call = {.call_id = "call", .tool_name = "tool"},
          .context = {},
      });
  auto erased = wh::compose::detail::make_tool_event_stream_reader(
      wh::compose::detail::make_graph_merge_reader(
          [&] {
            std::vector<wh::schema::stream::named_stream_reader<
                wh::compose::graph_stream_reader>>
                lanes{};
            lanes.emplace_back("call", std::move(reader_ok), false);
            return lanes;
          }()),
      std::move(ok_bindings), std::move(afters));
  auto collected = wh::compose::collect_graph_stream_reader(std::move(erased));
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 1U);
  auto *ok_event =
      wh::core::any_cast<wh::compose::tool_event>(&collected.value().front());
  REQUIRE(ok_event != nullptr);
  REQUIRE(*wh::core::any_cast<int>(&ok_event->value) == 2);

  auto [writer_missing, reader_missing] = wh::compose::make_graph_stream(2U);
  REQUIRE(writer_missing.try_write(wh::compose::graph_value{5}).has_value());
  REQUIRE(writer_missing.close().has_value());
  wh::compose::detail::tool_event_stream_reader missing_binding{
      wh::compose::detail::make_graph_merge_reader(
          [&] {
            std::vector<wh::schema::stream::named_stream_reader<
                wh::compose::graph_stream_reader>>
                lanes{};
            lanes.emplace_back("missing", std::move(reader_missing), false);
            return lanes;
          }()),
      {}, {}};
  auto missing_status = missing_binding.read();
  REQUIRE(missing_status.has_error());
  REQUIRE(missing_status.error() == wh::core::errc::not_found);

  auto [writer_error, reader_error] = wh::compose::make_graph_stream(2U);
  REQUIRE(writer_error.try_write(wh::compose::graph_value{7}).has_value());
  REQUIRE(writer_error.close().has_value());
  wh::compose::detail::tool_stream_binding_map error_bindings{};
  error_bindings.emplace(
      "call",
      wh::compose::detail::tool_stream_binding{
          .call = {.call_id = "call", .tool_name = "tool"},
          .context = {},
      });
  wh::compose::detail::tool_event_stream_reader after_error{
      wh::compose::detail::make_graph_merge_reader(
          [&] {
            std::vector<wh::schema::stream::named_stream_reader<
                wh::compose::graph_stream_reader>>
                lanes{};
            lanes.emplace_back("call", std::move(reader_error), false);
            return lanes;
          }()),
      std::move(error_bindings),
      {[](const wh::compose::tool_call &, wh::compose::graph_value &,
          const wh::tool::call_scope &) -> wh::core::result<void> {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }}};
  auto error_status = after_error.read();
  REQUIRE(error_status.has_value());
  REQUIRE(error_status.value().error == wh::core::errc::invalid_argument);
}
