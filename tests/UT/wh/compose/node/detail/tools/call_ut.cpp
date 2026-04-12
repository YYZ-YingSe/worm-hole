#include <catch2/catch_test_macros.hpp>

#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/node/detail/tools/call.hpp"

namespace {

template <typename sender_t>
[[nodiscard]] auto await_sender(sender_t &&sender) {
  auto waited = stdexec::sync_wait(std::forward<sender_t>(sender));
  REQUIRE(waited.has_value());
  return std::get<0>(std::move(*waited));
}

[[nodiscard]] auto make_string_reader(const std::string &value)
    -> wh::compose::graph_stream_reader {
  auto reader = wh::compose::make_values_stream_reader(
      std::vector<wh::compose::graph_value>{wh::compose::graph_value{value}});
  REQUIRE(reader.has_value());
  return std::move(reader).value();
}

static_assert(stdexec::sender<wh::compose::detail::call_completion_sender>);
static_assert(stdexec::sender<wh::compose::detail::stream_completion_sender>);

} // namespace

TEST_CASE("tools call helpers erase ready failure replay and direct dispatch paths",
          "[UT][wh/compose/node/detail/tools/call.hpp][call_value][condition][branch][boundary]") {
  auto erased_invoke = wh::compose::detail::erase_tools_invoke(
      stdexec::just(wh::core::result<wh::compose::graph_value>{
          wh::compose::graph_value{1}}));
  auto erased_invoke_status = await_sender(std::move(erased_invoke));
  REQUIRE(erased_invoke_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&erased_invoke_status.value()) == 1);

  auto erased_stream = wh::compose::detail::erase_tools_stream(
      stdexec::just(wh::compose::make_single_value_stream_reader(2)));
  auto erased_stream_status = await_sender(std::move(erased_stream));
  REQUIRE(erased_stream_status.has_value());
  auto collected = wh::compose::collect_graph_stream_reader(
      std::move(erased_stream_status).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 1U);
  REQUIRE(*wh::core::any_cast<int>(&collected.value().front()) == 2);

  wh::compose::detail::call_completion completion{
      .index = 0U,
      .call = {.call_id = "c", .tool_name = "echo"},
      .value = wh::compose::graph_value{3},
      .rerun_extra = wh::compose::graph_value{"extra"},
  };
  auto ready_completion = wh::compose::detail::ready_sender<
      wh::compose::detail::call_completion_sender>(
      wh::core::result<wh::compose::detail::call_completion>{completion});
  auto ready_completion_status = await_sender(std::move(ready_completion));
  REQUIRE(ready_completion_status.has_value());
  REQUIRE(ready_completion_status.value().call.call_id == "c");

  auto failed_completion = wh::compose::detail::failure_sender<
      wh::compose::detail::call_completion_sender,
      wh::core::result<wh::compose::detail::call_completion>>(
      wh::core::errc::invalid_argument);
  auto failed_completion_status = await_sender(std::move(failed_completion));
  REQUIRE(failed_completion_status.has_error());
  REQUIRE(failed_completion_status.error() == wh::core::errc::invalid_argument);

  auto ready_stream_reader = make_string_reader("ready");
  wh::compose::detail::stream_completion stream_completion{
      .index = 1U,
      .call = {.call_id = "s", .tool_name = "stream"},
      .stream = std::move(ready_stream_reader),
      .rerun_extra = wh::compose::graph_value{"extra-stream"},
      .context = {},
  };
  auto ready_stream = wh::compose::detail::ready_sender<
      wh::compose::detail::stream_completion_sender>(
      wh::core::result<wh::compose::detail::stream_completion>{
          std::move(stream_completion)});
  auto ready_stream_status = await_sender(std::move(ready_stream));
  REQUIRE(ready_stream_status.has_value());
  REQUIRE(ready_stream_status.value().call.call_id == "s");

  auto erased_call_completion = wh::compose::detail::erase_call_completion(
      stdexec::just(wh::core::result<wh::compose::detail::call_completion>{
          std::move(completion)}));
  REQUIRE(await_sender(std::move(erased_call_completion)).has_value());

  auto erased_stream_completion = wh::compose::detail::erase_stream_completion(
      stdexec::just(wh::core::result<wh::compose::detail::stream_completion>{
          std::move(ready_stream_status).value()}));
  REQUIRE(await_sender(std::move(erased_stream_completion)).has_value());

  auto replay_mismatch =
      wh::compose::detail::replay_stream(wh::compose::graph_value{7});
  REQUIRE(replay_mismatch.has_error());
  REQUIRE(replay_mismatch.error() == wh::core::errc::type_mismatch);

  auto replayed = wh::compose::detail::replay_stream(wh::compose::graph_value{
      std::vector<wh::compose::graph_value>{wh::compose::graph_value{9}}});
  REQUIRE(replayed.has_value());
  auto replayed_values =
      wh::compose::collect_graph_stream_reader(std::move(replayed).value());
  REQUIRE(replayed_values.has_value());
  REQUIRE(replayed_values.value().size() == 1U);
  REQUIRE(*wh::core::any_cast<int>(&replayed_values.value().front()) == 9);

  wh::core::run_context context{};
  context.interrupt_info.emplace();
  context.interrupt_info->interrupt_id = "interrupt";
  auto cloned = wh::compose::detail::clone_call_context(context);
  REQUIRE(cloned.has_value());
  REQUIRE(cloned->interrupt_info.has_value());
  REQUIRE(cloned->interrupt_info->interrupt_id == "interrupt");

  wh::compose::tools_options options{};
  std::vector<std::string> before_trace{};
  std::vector<std::string> after_trace{};
  options.middleware.push_back({
      .before =
          [&before_trace](wh::compose::tool_call &call,
                          const wh::tool::call_scope &scope)
              -> wh::core::result<void> {
        before_trace.push_back(call.tool_name);
        scope.run.interrupt_info.emplace();
        scope.run.interrupt_info->interrupt_id = "middleware";
        call.arguments += "-before";
        return {};
      },
      .after =
          [&after_trace](const wh::compose::tool_call &call,
                         wh::compose::graph_value &value,
                         const wh::tool::call_scope &)
              -> wh::core::result<void> {
        after_trace.push_back(call.tool_name);
        auto *typed = wh::core::any_cast<std::string>(&value);
        REQUIRE(typed != nullptr);
        value = wh::compose::graph_value{*typed + "-after"};
        return {};
      },
  });

  wh::compose::tool_registry registry{};
  registry.emplace(
      "echo",
      wh::compose::tool_entry{
          .invoke =
              [](const wh::compose::tool_call &call, wh::tool::call_scope)
                  -> wh::core::result<wh::compose::graph_value> {
                return wh::compose::graph_value{call.arguments};
              },
          .stream =
              [](const wh::compose::tool_call &call, wh::tool::call_scope)
                  -> wh::core::result<wh::compose::graph_stream_reader> {
                return wh::compose::make_values_stream_reader(
                    std::vector<wh::compose::graph_value>{
                        wh::compose::graph_value{call.arguments}});
              },
          .async_invoke =
              [](wh::compose::tool_call call,
                 wh::tool::call_scope) -> wh::compose::tools_invoke_sender {
                return wh::compose::detail::erase_tools_invoke(stdexec::just(
                    wh::core::result<wh::compose::graph_value>{
                        wh::compose::graph_value{call.arguments}}));
              },
          .async_stream =
              [](wh::compose::tool_call call,
                 wh::tool::call_scope) -> wh::compose::tools_stream_sender {
                return wh::compose::detail::erase_tools_stream(stdexec::just(
                    wh::compose::make_values_stream_reader(
                        std::vector<wh::compose::graph_value>{
                            wh::compose::graph_value{call.arguments}})));
              },
      });
  registry.emplace("stream-only", wh::compose::tool_entry{
                                      .stream = registry.at("echo").stream,
                                      .async_stream =
                                          registry.at("echo").async_stream,
                                  });

  wh::compose::detail::tools_state state{};
  state.options = &options;
  state.default_tools = &registry;
  state.parent_context = &context;

  auto missing_value =
      wh::compose::detail::call_value(state, {.tool_name = "missing"},
                                      {.run = context,
                                       .component = "tools",
                                       .implementation = "tool",
                                       .tool_name = "missing",
                                       .call_id = "m"});
  REQUIRE(missing_value.has_error());
  REQUIRE(missing_value.error() == wh::core::errc::not_found);

  auto not_supported_value =
      wh::compose::detail::call_value(state, {.tool_name = "stream-only"},
                                      {.run = context,
                                       .component = "tools",
                                       .implementation = "tool",
                                       .tool_name = "stream-only",
                                       .call_id = "s"});
  REQUIRE(not_supported_value.has_error());
  REQUIRE(not_supported_value.error() == wh::core::errc::not_supported);

  auto called_value = wh::compose::detail::call_value(
      state, {.tool_name = "echo", .arguments = "payload"},
      {.run = context,
       .component = "tools",
       .implementation = "tool",
       .tool_name = "echo",
       .call_id = "e"});
  REQUIRE(called_value.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&called_value.value()) == "payload");

  auto called_stream = wh::compose::detail::call_stream(
      state, {.tool_name = "echo", .arguments = "lane"},
      {.run = context,
       .component = "tools",
       .implementation = "tool",
       .tool_name = "echo",
       .call_id = "e"});
  REQUIRE(called_stream.has_value());
  auto stream_values =
      wh::compose::collect_graph_stream_reader(std::move(called_stream).value());
  REQUIRE(stream_values.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&stream_values.value().front()) ==
          "lane");

  auto started_value = await_sender(wh::compose::detail::start_value(
      state, {.tool_name = "echo", .arguments = "async"},
      {.run = context,
       .component = "tools",
       .implementation = "tool",
       .tool_name = "echo",
       .call_id = "e"}));
  REQUIRE(started_value.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&started_value.value()) == "async");

  auto started_stream = await_sender(wh::compose::detail::start_stream(
      state, {.tool_name = "echo", .arguments = "async-stream"},
      {.run = context,
       .component = "tools",
       .implementation = "tool",
       .tool_name = "echo",
       .call_id = "e"}));
  REQUIRE(started_stream.has_value());
  auto started_stream_values =
      wh::compose::collect_graph_stream_reader(std::move(started_stream).value());
  REQUIRE(started_stream_values.has_value());
  REQUIRE(
      *wh::core::any_cast<std::string>(&started_stream_values.value().front()) ==
      "async-stream");

  wh::compose::tool_call middleware_call{
      .call_id = "mw",
      .tool_name = "echo",
      .arguments = "payload",
  };
  auto middleware_scope = wh::compose::detail::make_scope(middleware_call, context);
  REQUIRE(wh::compose::detail::run_before(options, middleware_call, middleware_scope)
              .has_value());
  REQUIRE(middleware_call.arguments == "payload-before");
  wh::compose::graph_value after_value{std::string{"payload-before"}};
  REQUIRE(wh::compose::detail::run_after(options, middleware_call, after_value,
                                         middleware_scope)
              .has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&after_value) ==
          "payload-before-after");
  REQUIRE(before_trace == std::vector<std::string>{"echo"});
  REQUIRE(after_trace == std::vector<std::string>{"echo"});
}

TEST_CASE("tools call runners cover sync async rerun cache and middleware error branches",
          "[UT][wh/compose/node/detail/tools/call.hpp][start_call][condition][branch]") {
  wh::core::run_context context{};

  wh::compose::tool_call sync_call{
      .tool_name = "echo",
      .arguments = "payload",
  };
  wh::compose::tool_registry sync_registry{};
  sync_registry.emplace(
      "echo",
      wh::compose::tool_entry{
          .invoke =
              [](const wh::compose::tool_call &call, wh::tool::call_scope)
                  -> wh::core::result<wh::compose::graph_value> {
                return wh::compose::graph_value{call.arguments};
              },
          .stream =
              [](const wh::compose::tool_call &call, wh::tool::call_scope)
                  -> wh::core::result<wh::compose::graph_stream_reader> {
                return wh::compose::make_values_stream_reader(
                    std::vector<wh::compose::graph_value>{
                        wh::compose::graph_value{call.arguments}});
              },
          .async_invoke =
              [](wh::compose::tool_call call,
                 wh::tool::call_scope) -> wh::compose::tools_invoke_sender {
                return wh::compose::detail::erase_tools_invoke(stdexec::just(
                    wh::core::result<wh::compose::graph_value>{
                        wh::compose::graph_value{call.arguments}}));
              },
          .async_stream =
              [](wh::compose::tool_call call,
                 wh::tool::call_scope) -> wh::compose::tools_stream_sender {
                return wh::compose::detail::erase_tools_stream(stdexec::just(
                    wh::compose::make_values_stream_reader(
                        std::vector<wh::compose::graph_value>{
                            wh::compose::graph_value{call.arguments}})));
              },
      });

  wh::compose::tools_options sync_options{};
  sync_options.middleware.push_back({
      .before =
          [](wh::compose::tool_call &call, const wh::tool::call_scope &scope)
              -> wh::core::result<void> {
        call.arguments += "-before";
        scope.run.interrupt_info.emplace();
        scope.run.interrupt_info->interrupt_id = "sync";
        return {};
      },
      .after =
          [](const wh::compose::tool_call &, wh::compose::graph_value &value,
             const wh::tool::call_scope &)
              -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<std::string>(&value);
        REQUIRE(typed != nullptr);
        value = wh::compose::graph_value{*typed + "-after"};
        return {};
      },
  });

  wh::compose::detail::tools_state sync_state{};
  sync_state.options = &sync_options;
  sync_state.default_tools = &sync_registry;
  sync_state.parent_context = &context;
  sync_state.plans.push_back(
      {.index = 0U, .call = &sync_call, .call_id = "call-0"});

  auto sync_completion = wh::compose::detail::run_call(sync_state, 0U, context);
  REQUIRE(sync_completion.has_value());
  REQUIRE(sync_completion.value().call.call_id == "call-0");
  REQUIRE(*wh::core::any_cast<std::string>(&sync_completion.value().value) ==
          "payload-before-after");
  REQUIRE(*wh::core::any_cast<std::string>(&sync_completion.value().rerun_extra) ==
          "payload");
  REQUIRE(context.interrupt_info.has_value());
  REQUIRE(context.interrupt_info->interrupt_id == "sync");

  sync_state.rerun().ids.insert("other");
  sync_state.rerun().outputs.insert_or_assign("call-0", wh::compose::graph_value{9});
  auto cached_completion = wh::compose::detail::run_call(sync_state, 0U, context);
  REQUIRE(cached_completion.has_value());
  REQUIRE(*wh::core::any_cast<int>(&cached_completion.value().value) == 9);

  wh::compose::detail::tools_state missing_cache_state = sync_state;
  missing_cache_state.local_rerun.outputs.clear();
  auto missing_cache = wh::compose::detail::run_call(missing_cache_state, 0U, context);
  REQUIRE(missing_cache.has_error());
  REQUIRE(missing_cache.error() == wh::core::errc::not_found);

  wh::compose::tools_options before_error_options{};
  before_error_options.middleware.push_back({
      .before =
          [](wh::compose::tool_call &, const wh::tool::call_scope &)
              -> wh::core::result<void> {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      },
  });
  wh::compose::detail::tools_state before_error_state{};
  before_error_state.options = &before_error_options;
  before_error_state.default_tools = &sync_registry;
  before_error_state.parent_context = &context;
  before_error_state.plans.push_back(
      {.index = 0U, .call = &sync_call, .call_id = "call-0"});
  auto before_error =
      wh::compose::detail::run_call(before_error_state, 0U, context);
  REQUIRE(before_error.has_error());
  REQUIRE(before_error.error() == wh::core::errc::invalid_argument);

  wh::compose::tools_options after_error_options{};
  after_error_options.middleware.push_back({
      .after =
          [](const wh::compose::tool_call &, wh::compose::graph_value &,
             const wh::tool::call_scope &) -> wh::core::result<void> {
        return wh::core::result<void>::failure(wh::core::errc::not_supported);
      },
  });
  wh::compose::detail::tools_state after_error_state{};
  after_error_state.options = &after_error_options;
  after_error_state.default_tools = &sync_registry;
  after_error_state.parent_context = &context;
  after_error_state.plans.push_back(
      {.index = 0U, .call = &sync_call, .call_id = "call-0"});
  auto after_error =
      wh::compose::detail::run_call(after_error_state, 0U, context);
  REQUIRE(after_error.has_error());
  REQUIRE(after_error.error() == wh::core::errc::not_supported);

  sync_state.rerun().ids.clear();
  sync_state.rerun().outputs.clear();
  auto stream_completion =
      wh::compose::detail::run_stream_call(sync_state, 0U, context);
  REQUIRE(stream_completion.has_value());
  auto stream_values = wh::compose::collect_graph_stream_reader(
      std::move(stream_completion).value().stream);
  REQUIRE(stream_values.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&stream_values.value().front()) ==
          "payload-before");

  auto cached_stream_state = sync_state;
  cached_stream_state.local_rerun.ids.insert("other");
  cached_stream_state.local_rerun.outputs.insert_or_assign(
      "call-0",
      wh::compose::graph_value{std::vector<wh::compose::graph_value>{
          wh::compose::graph_value{1}}});
  auto replayed_stream =
      wh::compose::detail::run_stream_call(cached_stream_state, 0U, context);
  REQUIRE(replayed_stream.has_value());
  auto replayed_values = wh::compose::collect_graph_stream_reader(
      std::move(replayed_stream).value().stream);
  REQUIRE(replayed_values.has_value());
  REQUIRE(*wh::core::any_cast<int>(&replayed_values.value().front()) == 1);

  auto async_completion = await_sender(
      wh::compose::detail::start_call(sync_state, 0U, context));
  REQUIRE(async_completion.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&async_completion.value().value) ==
          "payload-before-after");

  auto async_stream_completion = await_sender(
      wh::compose::detail::start_stream_call(sync_state, 0U, context));
  REQUIRE(async_stream_completion.has_value());
  auto async_stream_values = wh::compose::collect_graph_stream_reader(
      std::move(async_stream_completion).value().stream);
  REQUIRE(async_stream_values.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&async_stream_values.value().front()) ==
          "payload-before");
}
