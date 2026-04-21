#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/component.hpp"

namespace {

struct callback_event_t {
  int value{0};
};

struct callback_state_t {
  callback_event_t event{};
  wh::callbacks::run_info run_info{};
};

struct options_provider {
  wh::core::component_options options{};

  [[nodiscard]] auto component_options() const noexcept -> const wh::core::component_options & {
    return options;
  }
};

struct observed_dispatch {
  std::vector<int> values{};
  std::vector<std::string> trace_ids{};
  std::vector<std::string> span_ids{};
  std::vector<wh::core::address> node_paths{};
};

auto make_sink_context(observed_dispatch &observed) -> wh::core::run_context {
  wh::core::run_context context{};
  context.callbacks.emplace();
  context.callbacks->manager.register_local_callbacks(
      wh::callbacks::make_callback_config([](const wh::callbacks::stage stage) noexcept {
        return stage == wh::callbacks::stage::start;
      }),
      wh::callbacks::make_typed_stage_callbacks<callback_event_t>(
          [&observed](const callback_event_t &event, const wh::callbacks::run_info &info) {
            observed.values.push_back(event.value);
            observed.trace_ids.push_back(info.trace_id);
            observed.span_ids.push_back(info.span_id);
            observed.node_paths.push_back(info.node_path);
          }));
  return context;
}

auto make_run_info() -> wh::callbacks::run_info {
  wh::callbacks::run_info info{};
  info.name = "callbacks";
  info.type = "callbacks";
  info.component = wh::core::component_kind::custom;
  info.trace_id = "trace-base";
  info.span_id = "span-base";
  info.node_path = wh::core::address{"graph", "node"};
  return info;
}

} // namespace

TEST_CASE(
    "callbacks sink exposes empty borrowed and owned manager states",
    "[UT][wh/callbacks/callbacks.hpp][callback_sink::manager_ptr][condition][branch][boundary]") {
  const auto empty = wh::callbacks::make_callback_sink();
  REQUIRE_FALSE(empty.has_value());
  REQUIRE(empty.manager_ptr() == nullptr);

  observed_dispatch observed{};
  auto borrowed_context = make_sink_context(observed);
  const auto borrowed = wh::callbacks::borrow_callback_sink(borrowed_context);
  REQUIRE(borrowed.has_value());
  REQUIRE(borrowed.borrowed != nullptr);
  REQUIRE(borrowed.owned == std::nullopt);
  REQUIRE(borrowed.manager_ptr() == borrowed.borrowed);

  auto owned = wh::callbacks::make_callback_sink(std::move(borrowed_context));
  REQUIRE(owned.has_value());
  REQUIRE(owned.borrowed == nullptr);
  REQUIRE(owned.owned.has_value());
  REQUIRE(owned.manager_ptr() != nullptr);
}

TEST_CASE("callbacks sink emission overlays metadata and supports bundle-state forwarding",
          "[UT][wh/callbacks/callbacks.hpp][emit][condition][branch][boundary]") {
  observed_dispatch observed{};
  auto context = make_sink_context(observed);
  context.callbacks->metadata.trace_id = "trace-override";
  context.callbacks->metadata.span_id = "span-override";
  context.callbacks->metadata.node_path = wh::core::address{"graph", "override"};

  const auto sink = wh::callbacks::make_callback_sink(context);
  const auto info = make_run_info();

  wh::callbacks::emit(sink, wh::callbacks::stage::start, callback_event_t{1}, info);
  wh::callbacks::emit(sink, wh::callbacks::stage::end, callback_event_t{2}, info);
  wh::callbacks::emit(sink, wh::callbacks::stage::start,
                      callback_state_t{.event = callback_event_t{3}, .run_info = info});

  REQUIRE(observed.values == std::vector<int>{1, 3});
  REQUIRE(observed.trace_ids == std::vector<std::string>{"trace-override", "trace-override"});
  REQUIRE(observed.span_ids == std::vector<std::string>{"span-override", "span-override"});
  REQUIRE(observed.node_paths == std::vector<wh::core::address>{
                                     wh::core::address{"graph", "override"},
                                     wh::core::address{"graph", "override"},
                                 });
}

TEST_CASE("callbacks helpers apply component run info and filter disabled sinks",
          "[UT][wh/callbacks/callbacks.hpp][filter_callback_sink][condition][branch][boundary]") {
  options_provider enabled{};
  enabled.options.set_base(wh::core::component_common_options{
      .callbacks_enabled = true, .trace_id = "trace-base", .span_id = "span-base"});
  enabled.options.set_call_override(
      wh::core::component_override_options{.callbacks_enabled = true,
                                           .trace_id = std::string{"trace-override"},
                                           .span_id = std::string{"span-override"}});

  auto info = wh::callbacks::apply_component_run_info(make_run_info(), enabled);
  REQUIRE(info.trace_id == "trace-override");
  REQUIRE(info.span_id == "span-override");
  REQUIRE(wh::callbacks::callbacks_enabled(enabled));

  observed_dispatch observed{};
  auto context = make_sink_context(observed);
  auto sink = wh::callbacks::borrow_callback_sink(context);
  REQUIRE(wh::callbacks::filter_callback_sink(sink, enabled).has_value());

  options_provider disabled{};
  disabled.options.set_base(wh::core::component_common_options{.callbacks_enabled = true});
  disabled.options.set_call_override(
      wh::core::component_override_options{.callbacks_enabled = false});
  REQUIRE_FALSE(wh::callbacks::callbacks_enabled(disabled));
  REQUIRE_FALSE(wh::callbacks::filter_callback_sink(std::move(sink), disabled).has_value());
}
