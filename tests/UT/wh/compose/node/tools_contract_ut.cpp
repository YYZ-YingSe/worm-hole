#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/node/tools_contract.hpp"

TEST_CASE("tools contract types store dispatch payloads middleware and rerun metadata",
          "[UT][wh/compose/node/tools_contract.hpp][tool_entry][condition][branch][boundary]") {
  wh::compose::tool_call call{};
  call.call_id = "call-1";
  call.tool_name = "echo";
  call.arguments = R"({"x":1})";
  call.payload = wh::compose::graph_value{7};

  wh::compose::tool_entry entry{};
  entry.return_direct = true;
  entry.invoke = [](const wh::compose::tool_call &request,
                    wh::tool::call_scope scope) -> wh::core::result<wh::compose::graph_value> {
    REQUIRE(scope.tool_name == request.tool_name);
    return wh::compose::graph_value{request.arguments};
  };
  entry.stream =
      [](const wh::compose::tool_call &request,
         wh::tool::call_scope scope) -> wh::core::result<wh::compose::graph_stream_reader> {
    REQUIRE(scope.call_id == request.call_id);
    return wh::compose::make_single_value_stream_reader(request.arguments);
  };
  entry.async_invoke = [](wh::compose::tool_call request, wh::tool::call_scope scope) {
    REQUIRE(scope.tool_name == request.tool_name);
    return [](wh::compose::tool_call owned_request)
        -> exec::task<wh::core::result<wh::compose::graph_value>> {
      co_return wh::core::result<wh::compose::graph_value>{
          wh::compose::graph_value{std::move(owned_request.call_id)}};
    }(std::move(request));
  };
  entry.async_stream = [](wh::compose::tool_call request, wh::tool::call_scope scope) {
    REQUIRE(scope.call_id == request.call_id);
    return [](wh::compose::tool_call owned_request)
        -> exec::task<wh::core::result<wh::compose::graph_stream_reader>> {
      auto reader =
          wh::compose::make_single_value_stream_reader(std::move(owned_request.tool_name));
      REQUIRE(reader.has_value());
      co_return wh::core::result<wh::compose::graph_stream_reader>{std::move(reader).value()};
    }(std::move(request));
  };

  wh::core::run_context context{};
  wh::tool::call_scope scope{
      .run = context,
      .component = "tools",
      .implementation = "invoke",
      .tool_name = call.tool_name,
      .call_id = call.call_id,
  };
  auto invoked = entry.invoke(call, scope);
  REQUIRE(invoked.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&invoked.value()) == R"({"x":1})");

  auto streamed = entry.stream(call, scope);
  REQUIRE(streamed.has_value());
  auto stream_values = wh::compose::collect_graph_stream_reader(std::move(streamed).value());
  REQUIRE(stream_values.has_value());
  REQUIRE(stream_values.value().size() == 1U);
  REQUIRE(*wh::core::any_cast<std::string>(&stream_values.value()[0]) == R"({"x":1})");

  auto async_invoked = stdexec::sync_wait(entry.async_invoke(call, scope));
  REQUIRE(async_invoked.has_value());
  REQUIRE(std::get<0>(*async_invoked).has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&std::get<0>(*async_invoked).value()) == "call-1");

  auto async_streamed = stdexec::sync_wait(entry.async_stream(call, scope));
  REQUIRE(async_streamed.has_value());
  REQUIRE(std::get<0>(*async_streamed).has_value());
  auto async_stream_values =
      wh::compose::collect_graph_stream_reader(std::get<0>(std::move(*async_streamed)).value());
  REQUIRE(async_stream_values.has_value());
  REQUIRE(async_stream_values.value().size() == 1U);
  REQUIRE(*wh::core::any_cast<std::string>(&async_stream_values.value()[0]) == "echo");

  REQUIRE(entry.return_direct);

  wh::compose::tool_registry registry{};
  registry.emplace("echo", entry);
  REQUIRE(registry.find(std::string_view{"echo"}) != registry.end());

  wh::compose::tool_middleware middleware{};
  middleware.before = [](wh::compose::tool_call &request,
                         const wh::tool::call_scope &) -> wh::core::result<void> {
    request.call_id = "rewritten";
    return {};
  };
  middleware.after = [](const wh::compose::tool_call &request, wh::compose::graph_value &value,
                        const wh::tool::call_scope &) -> wh::core::result<void> {
    value = wh::compose::graph_value{request.call_id};
    return {};
  };
  REQUIRE(middleware.before(call, scope).has_value());
  REQUIRE(call.call_id == "rewritten");
  auto payload = wh::compose::graph_value{1};
  REQUIRE(middleware.after(call, payload, scope).has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&payload) == "rewritten");

  wh::compose::tools_rerun rerun{};
  rerun.ids.insert("rewritten");
  rerun.extra.emplace("rewritten", wh::compose::graph_value{9});
  rerun.outputs.emplace("rewritten", wh::compose::graph_value{10});

  wh::compose::tools_options options{};
  options.missing = entry;
  options.middleware.push_back(middleware);
  options.sequential = false;

  wh::compose::tools_call_options call_options{};
  call_options.registry = std::cref(registry);
  call_options.sequential = true;
  call_options.rerun = &rerun;

  REQUIRE(options.missing.has_value());
  REQUIRE(options.middleware.size() == 1U);
  REQUIRE_FALSE(options.sequential);
  REQUIRE(call_options.registry->get().find("echo") != registry.end());
  REQUIRE(call_options.sequential == std::optional<bool>{true});
  REQUIRE(call_options.rerun == &rerun);
}

TEST_CASE("tools contract defaults expose null callbacks and empty rerun state",
          "[UT][wh/compose/node/tools_contract.hpp][tools_options][condition][branch][boundary]") {
  wh::compose::tool_entry entry{};
  REQUIRE_FALSE(static_cast<bool>(entry.invoke));
  REQUIRE_FALSE(static_cast<bool>(entry.stream));
  REQUIRE_FALSE(static_cast<bool>(entry.async_invoke));
  REQUIRE_FALSE(static_cast<bool>(entry.async_stream));
  REQUIRE_FALSE(entry.return_direct);

  wh::compose::tool_batch batch{};
  REQUIRE(batch.calls.empty());

  wh::compose::tool_result result{};
  REQUIRE(result.call_id.empty());
  REQUIRE(result.tool_name.empty());

  wh::compose::tool_event event{};
  REQUIRE(event.call_id.empty());
  REQUIRE(event.tool_name.empty());

  wh::compose::tool_middleware middleware{};
  REQUIRE_FALSE(static_cast<bool>(middleware.before));
  REQUIRE_FALSE(static_cast<bool>(middleware.after));

  wh::compose::tools_options options{};
  REQUIRE_FALSE(options.missing.has_value());
  REQUIRE(options.middleware.empty());
  REQUIRE(options.sequential);

  wh::compose::tools_rerun rerun{};
  REQUIRE(rerun.ids.empty());
  REQUIRE(rerun.extra.empty());
  REQUIRE(rerun.outputs.empty());

  wh::compose::tools_call_options call_options{};
  REQUIRE_FALSE(call_options.registry.has_value());
  REQUIRE_FALSE(call_options.sequential.has_value());
  REQUIRE(call_options.rerun == nullptr);
}
