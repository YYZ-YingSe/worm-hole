#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/agent/toolset.hpp"

namespace {

struct sync_tool {
  wh::schema::tool_schema_definition schema_value{
      .name = "sync",
      .description = "sync tool",
  };

  [[nodiscard]] auto schema() const -> const wh::schema::tool_schema_definition & {
    return schema_value;
  }

  [[nodiscard]] auto invoke(wh::tool::tool_request request, wh::core::run_context &) const
      -> wh::tool::tool_invoke_result {
    return request.input_json;
  }
};

struct async_stream_tool {
  wh::schema::tool_schema_definition schema_value{
      .name = "stream",
      .description = "stream tool",
  };

  [[nodiscard]] auto schema() const -> const wh::schema::tool_schema_definition & {
    return schema_value;
  }

  [[nodiscard]] auto async_stream(wh::tool::tool_request request, wh::core::run_context &) const {
    auto reader =
        wh::schema::stream::make_single_value_stream_reader<std::string>(request.input_json);
    return stdexec::just(wh::tool::tool_output_stream_result{
        wh::tool::tool_output_stream_reader{std::move(reader)}});
  }
};

static_assert(wh::agent::detail::registered_tool_component<sync_tool>);
static_assert(wh::agent::detail::registered_tool_component<async_stream_tool>);

} // namespace

TEST_CASE("agent toolset validates raw entry schemas and handler presence",
          "[UT][wh/agent/toolset.hpp][toolset::add_entry][condition][branch][boundary]") {
  wh::agent::toolset tools{};
  wh::schema::tool_schema_definition bad_schema{};
  wh::compose::tool_entry bad_entry{};

  REQUIRE(tools.empty());
  REQUIRE(tools.size() == 0U);

  auto invalid_schema = tools.add_entry(bad_schema, bad_entry);
  REQUIRE(invalid_schema.has_error());
  REQUIRE(invalid_schema.error() == wh::core::errc::invalid_argument);

  auto invalid_entry =
      tools.add_entry({.name = "noimpl", .description = "missing handlers"}, bad_entry);
  REQUIRE(invalid_entry.has_error());
  REQUIRE(invalid_entry.error() == wh::core::errc::invalid_argument);
}

TEST_CASE("agent toolset add_tool registers schemas return-direct flags and duplicate rejection",
          "[UT][wh/agent/toolset.hpp][toolset::add_tool][condition][branch][boundary]") {
  wh::agent::toolset tools{};
  sync_tool sync{};
  async_stream_tool streaming{};

  REQUIRE(tools.add_tool(sync, {.return_direct = true}).has_value());
  REQUIRE_FALSE(tools.empty());
  REQUIRE(tools.size() == 1U);
  REQUIRE(tools.is_return_direct_tool("sync"));
  REQUIRE_FALSE(tools.is_return_direct_tool("missing"));
  REQUIRE(tools.registry().at("sync").return_direct);

  auto duplicate = tools.add_tool(sync);
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);

  REQUIRE(tools.add_tool(streaming).has_value());
  REQUIRE(tools.size() == 2U);
  REQUIRE(tools.schemas().size() == 2U);
}

TEST_CASE("agent toolset make_tool_entry bridges sync invoke tools into compose values",
          "[UT][wh/agent/toolset.hpp][make_tool_entry][condition][branch][boundary]") {
  sync_tool sync{};
  auto entry = wh::agent::detail::make_tool_entry(sync, true);
  wh::core::run_context context{};

  auto invoked =
      entry.invoke({.tool_name = "sync", .arguments = "payload"}, {.run = context,
                                                                   .component = "tools",
                                                                   .implementation = "tool",
                                                                   .tool_name = "sync",
                                                                   .call_id = "call-1"});

  REQUIRE(invoked.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&invoked.value()) == "payload");
  REQUIRE(entry.return_direct);
}

TEST_CASE("agent toolset make_tool_entry bridges async stream tools into graph stream readers",
          "[UT][wh/agent/toolset.hpp][make_tool_entry][condition][branch][boundary]") {
  async_stream_tool streaming{};
  auto entry = wh::agent::detail::make_tool_entry(streaming, false);
  wh::core::run_context context{};

  auto stream_status = stdexec::sync_wait(
      entry.async_stream({.tool_name = "stream", .arguments = "chunk"}, {.run = context,
                                                                         .component = "tools",
                                                                         .implementation = "tool",
                                                                         .tool_name = "stream",
                                                                         .call_id = "call-2"}));

  REQUIRE(stream_status.has_value());
  REQUIRE(std::get<0>(*stream_status).has_value());
  REQUIRE_FALSE(entry.return_direct);
}

TEST_CASE("agent toolset preserves middleware and node authoring options",
          "[UT][wh/agent/toolset.hpp][toolset::set_node_options][condition][branch][boundary]") {
  wh::agent::toolset tools{};

  REQUIRE(tools.add_middleware({}).has_value());
  REQUIRE(tools.runtime_options().middleware.size() == 1U);
  REQUIRE(
      tools.set_node_options({.exec_mode = wh::compose::node_exec_mode::async, .sequential = false})
          .has_value());
  REQUIRE(tools.node_options().has_value());
  REQUIRE(tools.node_options()->exec_mode == wh::compose::node_exec_mode::async);
  REQUIRE_FALSE(tools.node_options()->sequential);
  REQUIRE_FALSE(tools.runtime_options().sequential);
}
