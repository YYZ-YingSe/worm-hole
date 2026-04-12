#include <catch2/catch_test_macros.hpp>

#include "wh/compose/graph/detail/runtime/handlers.hpp"

TEST_CASE("runtime handler resolution prefers authored handlers and validates external contracts",
          "[UT][wh/compose/graph/detail/runtime/handlers.hpp][resolve_node_state_handlers][condition][branch][boundary]") {
  wh::compose::graph_add_node_options authored_options{};
  authored_options.state.bind_pre<int>(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &,
         int &, wh::core::run_context &) -> wh::core::result<void> { return {}; });

  auto authored = wh::compose::detail::state_runtime::resolve_node_state_handlers(
      nullptr, "node", authored_options);
  REQUIRE(authored.has_value());
  REQUIRE(authored.value() == authored_options.state.authored_handlers());

  wh::compose::graph_state_handler_registry registry{};
  registry.emplace("node", wh::compose::graph_node_state_handlers{
                               .pre = authored_options.state.authored_handlers()->pre,
                           });

  auto conflict = wh::compose::detail::state_runtime::resolve_node_state_handlers(
      &registry, "node", authored_options);
  REQUIRE(conflict.has_error());
  REQUIRE(conflict.error() == wh::core::errc::contract_violation);

  wh::compose::graph_add_node_options required_external{};
  required_external.state.require_post();
  auto missing = wh::compose::detail::state_runtime::resolve_node_state_handlers(
      nullptr, "node", required_external);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  auto incomplete =
      wh::compose::detail::state_runtime::resolve_node_state_handlers(
          &registry, "node", required_external);
  REQUIRE(incomplete.has_error());
  REQUIRE(incomplete.error() == wh::core::errc::contract_violation);
}

TEST_CASE("runtime handlers select phase handlers and apply value and stream payloads",
          "[UT][wh/compose/graph/detail/runtime/handlers.hpp][apply_pre_handlers][condition][branch][boundary]") {
  wh::compose::graph_node_state_handlers handlers{};
  handlers.pre = [](const wh::compose::graph_state_cause &,
                    wh::compose::graph_process_state &, wh::compose::graph_value &value,
                    wh::core::run_context &) -> wh::core::result<void> {
    auto *typed = wh::core::any_cast<int>(&value);
    REQUIRE(typed != nullptr);
    *typed += 2;
    return {};
  };
  handlers.stream_post =
      [](const wh::compose::graph_state_cause &,
         wh::compose::graph_process_state &, wh::compose::graph_value &value,
         wh::core::run_context &) -> wh::core::result<void> {
    auto *typed = wh::core::any_cast<std::string>(&value);
    REQUIRE(typed != nullptr);
    typed->append("!");
    return {};
  };

  REQUIRE(static_cast<bool>(wh::compose::detail::state_runtime::value_handler_for(
      &handlers, wh::compose::detail::state_runtime::state_phase::pre)));
  REQUIRE(static_cast<bool>(wh::compose::detail::state_runtime::stream_handler_for(
      &handlers, wh::compose::detail::state_runtime::state_phase::post)));
  REQUIRE(wh::compose::detail::state_runtime::has_stream_phase(
      &handlers, wh::compose::detail::state_runtime::state_phase::post));

  wh::core::run_context context{};
  wh::compose::graph_process_state process_state{};
  wh::compose::graph_state_cause cause{
      .run_id = 1U,
      .step = 3U,
      .node_key = "worker",
  };

  wh::compose::graph_value value_payload{5};
  auto pre = wh::compose::detail::state_runtime::apply_pre_handlers(
      context, &handlers, "worker", cause, process_state, value_payload,
      [](wh::core::run_context &, std::string_view, wh::core::error_code,
         std::string_view) {});
  REQUIRE(pre.has_value());
  REQUIRE(*wh::core::any_cast<int>(&value_payload) == 7);

  auto reader_status =
      wh::compose::make_single_value_stream_reader(std::string{"x"});
  REQUIRE(reader_status.has_value());
  wh::compose::graph_value stream_payload{std::move(reader_status).value()};
  REQUIRE(wh::compose::detail::state_runtime::needs_async_phase(
      &handlers, stream_payload, wh::compose::detail::state_runtime::state_phase::post));
  REQUIRE_FALSE(wh::compose::detail::state_runtime::needs_async_phase(
      &handlers, value_payload, wh::compose::detail::state_runtime::state_phase::post));

  wh::compose::graph_value chunk_payload{std::string{"done"}};
  auto post = wh::compose::detail::state_runtime::apply_post_handlers(
      context, &handlers, "worker", cause, process_state, chunk_payload,
      [](wh::core::run_context &, std::string_view, wh::core::error_code,
         std::string_view) {});
  REQUIRE(post.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&chunk_payload) == "done!");
}
