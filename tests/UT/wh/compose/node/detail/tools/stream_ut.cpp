#include <chrono>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "wh/compose/node/detail/tools/output.hpp"

namespace {

[[nodiscard]] auto read_text(wh::compose::graph_value &value) -> std::string {
  auto *typed = wh::core::any_cast<std::string>(&value);
  REQUIRE(typed != nullptr);
  return *typed;
}

[[nodiscard]] auto
materialize_stream_output(wh::compose::tools_options options,
                          std::vector<wh::compose::detail::stream_completion> completions)
    -> wh::core::result<wh::compose::graph_stream_reader> {
  wh::compose::detail::tools_state state{};
  state.options = &options;
  state.afters = wh::compose::detail::make_tool_after_chain(options);

  auto output = wh::compose::detail::build_stream_output(state, std::move(completions));
  if (output.has_error()) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(output.error());
  }

  auto *reader = wh::core::any_cast<wh::compose::graph_stream_reader>(&output.value());
  if (reader == nullptr) {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(
        wh::core::errc::type_mismatch);
  }
  return std::move(*reader);
}

} // namespace

TEST_CASE("tools stream output maps per-call streams through final tool events and preserves stop "
          "and close behavior",
          "[UT][wh/compose/node/detail/tools/"
          "output.hpp][build_stream_output][condition][branch][concurrency]") {
  wh::compose::tools_options options{};
  options.middleware.push_back({
      .after = [](const wh::compose::tool_call &, wh::compose::graph_value &value,
                  const wh::tool::call_scope &) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<std::string>(&value);
        REQUIRE(typed != nullptr);
        value = wh::compose::graph_value{*typed + "-after"};
        return {};
      },
  });

  auto [writer, source] = wh::compose::make_graph_stream(4U);
  std::vector<wh::compose::detail::stream_completion> wrapped_inputs{};
  wrapped_inputs.push_back(
      wh::compose::detail::stream_completion{.index = 0U,
                                             .call =
                                                 wh::compose::tool_call{
                                                     .call_id = "call-1",
                                                     .tool_name = "echo",
                                                     .arguments = "payload",
                                                 },
                                             .stream = std::move(source),
                                             .after_context = wh::core::run_context{},
                                             .rerun_extra = {}});
  auto wrapped = materialize_stream_output(options, std::move(wrapped_inputs));
  REQUIRE(wrapped.has_value());

  auto pending = wrapped.value().try_read();
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_signal>(pending));
  REQUIRE(std::get<wh::schema::stream::stream_signal>(pending) ==
          wh::schema::stream::stream_pending);

  wh::testing::helper::sender_capture<> stopped{};
  stdexec::inplace_stop_source stop_source{};
  auto operation = stdexec::connect(wrapped.value().read_async(),
                                    wh::testing::helper::sender_capture_receiver{
                                        &stopped,
                                        wh::testing::helper::make_scheduler_env(
                                            stdexec::inline_scheduler{}, stop_source.get_token()),
                                    });
  stdexec::start(operation);
  stop_source.request_stop();

  REQUIRE(stopped.ready.try_acquire_for(std::chrono::milliseconds(100)));
  REQUIRE(stopped.terminal == wh::testing::helper::sender_terminal_kind::stopped);

  REQUIRE(writer.try_write(wh::compose::graph_value{std::string{"chunk"}}).has_value());
  REQUIRE(writer.close().has_value());

  auto resumed = wrapped.value().read();
  REQUIRE(resumed.has_value());
  REQUIRE_FALSE(resumed.value().eof);
  REQUIRE(resumed.value().value.has_value());
  auto *event = wh::core::any_cast<wh::compose::tool_event>(&*resumed.value().value);
  REQUIRE(event != nullptr);
  REQUIRE(event->call_id == "call-1");
  REQUIRE(event->tool_name == "echo");
  REQUIRE(read_text(event->value) == "chunk-after");

  auto eof = wrapped.value().read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);

  REQUIRE(wrapped.value().is_closed());
  REQUIRE(wrapped.value().is_source_closed());
  wrapped.value().set_automatic_close({});
  REQUIRE(wrapped.value().close().has_value());
  REQUIRE(wrapped.value().is_closed());
}

TEST_CASE(
    "tools stream async sender keeps transformed stream state alive after reader destruction",
    "[UT][wh/compose/node/detail/tools/output.hpp][build_stream_output][lifetime][concurrency]") {
  wh::compose::tools_options options{};
  options.middleware.push_back({
      .after = [](const wh::compose::tool_call &, wh::compose::graph_value &value,
                  const wh::tool::call_scope &) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<std::string>(&value);
        REQUIRE(typed != nullptr);
        value = wh::compose::graph_value{*typed + "-owned"};
        return {};
      },
  });

  auto [writer, source] = wh::compose::make_graph_stream(4U);
  auto sender = [&]() {
    std::vector<wh::compose::detail::stream_completion> sender_inputs{};
    sender_inputs.push_back(
        wh::compose::detail::stream_completion{.index = 0U,
                                               .call = {.call_id = "call-1", .tool_name = "echo"},
                                               .stream = std::move(source),
                                               .after_context = wh::core::run_context{},
                                               .rerun_extra = {}});
    auto wrapped = materialize_stream_output(options, std::move(sender_inputs));
    REQUIRE(wrapped.has_value());
    auto reader = std::move(wrapped).value();
    return std::move(reader).read_async();
  }();

  using result_t =
      wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<wh::compose::graph_value>>;
  wh::testing::helper::sender_capture<result_t> capture{};
  auto operation = stdexec::connect(
      std::move(sender), wh::testing::helper::sender_capture_receiver{
                             &capture,
                             wh::testing::helper::make_scheduler_env(stdexec::inline_scheduler{}),
                         });
  stdexec::start(operation);

  REQUIRE(writer.try_write(wh::compose::graph_value{std::string{"chunk"}}).has_value());
  REQUIRE(writer.close().has_value());
  REQUIRE(capture.ready.try_acquire_for(std::chrono::milliseconds(100)));
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(capture.value->has_value());
  REQUIRE(capture.value->value().value.has_value());
  auto *event = wh::core::any_cast<wh::compose::tool_event>(&*capture.value->value().value);
  REQUIRE(event != nullptr);
  REQUIRE(read_text(event->value) == "chunk-owned");
}

TEST_CASE("tools stream output surfaces after-middleware failures as terminal error chunks",
          "[UT][wh/compose/node/detail/tools/output.hpp][build_stream_output][failure][boundary]") {
  auto [writer_ok, reader_ok] = wh::compose::make_graph_stream(2U);
  REQUIRE(writer_ok.try_write(wh::compose::graph_value{1}).has_value());
  REQUIRE(writer_ok.close().has_value());

  wh::compose::tools_options ok_options{};
  ok_options.middleware.push_back({
      .after = [](const wh::compose::tool_call &, wh::compose::graph_value &value,
                  const wh::tool::call_scope &) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<int>(&value);
        REQUIRE(typed != nullptr);
        value = wh::compose::graph_value{*typed + 1};
        return {};
      },
  });

  std::vector<wh::compose::detail::stream_completion> ok_inputs{};
  ok_inputs.push_back(
      wh::compose::detail::stream_completion{.index = 0U,
                                             .call = {.call_id = "call", .tool_name = "tool"},
                                             .stream = std::move(reader_ok),
                                             .after_context = wh::core::run_context{},
                                             .rerun_extra = {}});
  auto ok_reader = materialize_stream_output(ok_options, std::move(ok_inputs));
  REQUIRE(ok_reader.has_value());

  auto collected = wh::compose::collect_graph_stream_reader(std::move(ok_reader).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 1U);
  auto *ok_event = wh::core::any_cast<wh::compose::tool_event>(&collected.value().front());
  REQUIRE(ok_event != nullptr);
  REQUIRE(*wh::core::any_cast<int>(&ok_event->value) == 2);

  auto [writer_error, reader_error] = wh::compose::make_graph_stream(2U);
  REQUIRE(writer_error.try_write(wh::compose::graph_value{7}).has_value());
  REQUIRE(writer_error.close().has_value());

  wh::compose::tools_options error_options{};
  error_options.middleware.push_back({
      .after = [](const wh::compose::tool_call &, wh::compose::graph_value &,
                  const wh::tool::call_scope &) -> wh::core::result<void> {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      },
  });

  std::vector<wh::compose::detail::stream_completion> error_inputs{};
  error_inputs.push_back(
      wh::compose::detail::stream_completion{.index = 0U,
                                             .call = {.call_id = "call", .tool_name = "tool"},
                                             .stream = std::move(reader_error),
                                             .after_context = wh::core::run_context{},
                                             .rerun_extra = {}});
  auto error_reader = materialize_stream_output(error_options, std::move(error_inputs));
  REQUIRE(error_reader.has_value());

  auto error_status = error_reader.value().read();
  REQUIRE(error_status.has_value());
  REQUIRE(error_status.value().error == wh::core::errc::invalid_argument);
}
