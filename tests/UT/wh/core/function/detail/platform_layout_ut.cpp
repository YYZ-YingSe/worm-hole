#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/agent/agent.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/function/detail/error_policy.hpp"
#include "wh/core/function/detail/ownership_policy.hpp"
#include "wh/core/function/detail/storage_policy.hpp"

namespace {

template <
    typename signature_t, template <typename, template <typename> class> class ownership_policy,
    template <typename> class acceptance_policy, typename error_policy, std::size_t buffer_size_v>
class owning_storage_probe
    : public wh::core::fn::owning_storage<signature_t, ownership_policy, acceptance_policy,
                                          error_policy, buffer_size_v, std::allocator> {
  using base = wh::core::fn::owning_storage<signature_t, ownership_policy, acceptance_policy,
                                            error_policy, buffer_size_v, std::allocator>;

public:
  template <typename fun_t, typename... args_t>
  static constexpr bool can_create_v = base::template can_create_from<fun_t, args_t...>();
};

using lower_graph_hook_probe =
    owning_storage_probe<wh::core::result<wh::compose::graph>(), wh::core::fn::deep_copy,
                         wh::core::fn::non_copyable_accept, wh::core::fn::assert_on_error,
                         sizeof(void *)>;

using graph_selector_probe =
    owning_storage_probe<wh::core::result<std::vector<std::uint32_t>>(
                             const wh::compose::graph_value &, wh::core::run_context &,
                             const wh::compose::graph_call_scope &) const,
                         wh::core::fn::deep_copy, wh::core::fn::standard_accept,
                         wh::core::fn::skip_on_error, wh::core::fn_detail::member_pointer_size>;

using node_sync_factory_probe =
    owning_storage_probe<wh::core::result<wh::compose::graph_value>(
                             wh::compose::graph_value &, wh::core::run_context &,
                             const wh::compose::node_runtime &) const,
                         wh::core::fn::deep_copy, wh::core::fn::standard_accept,
                         wh::core::fn::skip_on_error, wh::core::fn_detail::member_pointer_size>;

using node_async_factory_probe =
    owning_storage_probe<wh::compose::graph_sender(wh::compose::graph_value &,
                                                   wh::core::run_context &,
                                                   const wh::compose::node_runtime &) const,
                         wh::core::fn::deep_copy, wh::core::fn::standard_accept,
                         wh::core::fn::skip_on_error, wh::core::fn_detail::member_pointer_size>;

using graph_state_pre_handler_probe =
    owning_storage_probe<wh::core::result<void>(const wh::compose::graph_state_cause &,
                                                wh::compose::graph_process_state &,
                                                wh::compose::graph_value &, wh::core::run_context &)
                             const,
                         wh::core::fn::deep_copy, wh::core::fn::standard_accept,
                         wh::core::fn::skip_on_error, wh::core::fn_detail::member_pointer_size>;

template <typename run_t> auto make_sync_wrapper(run_t &&run) {
  auto stored = wh::compose::detail::make_mutable_capture(std::forward<run_t>(run));
  return
      [stored = std::move(stored)](
          wh::compose::graph_value &input, wh::core::run_context &context,
          const wh::compose::node_runtime &runtime) -> wh::core::result<wh::compose::graph_value> {
        return stored.value(input, context, runtime);
      };
}

template <typename run_t> auto make_async_wrapper(run_t &&run) {
  auto stored = wh::compose::detail::make_mutable_capture(std::forward<run_t>(run));
  return [stored = std::move(stored)](
             wh::compose::graph_value &input, wh::core::run_context &context,
             const wh::compose::node_runtime &runtime) -> wh::compose::graph_sender {
    auto sender = stored.value(input, context, runtime);
    if (runtime.graph_scheduler() == nullptr) {
      return wh::compose::detail::failure_graph_sender(wh::core::errc::contract_violation);
    }
    if constexpr (std::same_as<std::remove_cvref_t<decltype(sender)>, wh::compose::graph_sender>) {
      return wh::compose::detail::bridge_graph_sender(std::move(sender));
    }
    return wh::compose::detail::bridge_graph_sender(
        wh::core::detail::write_sender_scheduler(std::move(sender), *runtime.graph_scheduler()));
  };
}

} // namespace

TEST_CASE("function storage accepts representative callable layouts across "
          "move-only and callback wrappers",
          "[UT][wh/core/function/detail/storage_policy.hpp][platform]["
          "windows][boundary]") {
  auto lower = [payload =
                    std::make_unique<int>(7)]() mutable -> wh::core::result<wh::compose::graph> {
    return wh::core::result<wh::compose::graph>::failure(wh::core::errc::not_supported);
  };
  using lower_t = decltype(lower);
  STATIC_REQUIRE(!std::is_copy_constructible_v<lower_t>);
  STATIC_REQUIRE(lower_graph_hook_probe::template can_create_v<lower_t, lower_t>);
  STATIC_REQUIRE(std::is_constructible_v<wh::agent::agent::lower_graph_hook, lower_t>);

  wh::agent::agent::lower_graph_hook lower_hook{std::move(lower)};
  REQUIRE(static_cast<bool>(lower_hook));

  auto selector =
      [shared = std::make_shared<int>(3)](
          const wh::compose::graph_value &, wh::core::run_context &,
          const wh::compose::graph_call_scope &) -> wh::core::result<std::vector<std::uint32_t>> {
    return std::vector<std::uint32_t>{static_cast<std::uint32_t>(*shared)};
  };
  using selector_t = decltype(selector);
  STATIC_REQUIRE(std::is_copy_constructible_v<selector_t>);
  STATIC_REQUIRE(graph_selector_probe::template can_create_v<selector_t, selector_t>);
  STATIC_REQUIRE(std::is_constructible_v<wh::compose::graph_value_branch_selector_ids, selector_t>);

  wh::compose::graph_value_branch_selector_ids selector_ids{selector};
  REQUIRE(static_cast<bool>(selector_ids));

  auto state_pre = [shared = std::make_shared<int>(1)](
                       const wh::compose::graph_state_cause &, wh::compose::graph_process_state &,
                       wh::compose::graph_value &,
                       wh::core::run_context &) -> wh::core::result<void> {
    if (*shared == 0) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    return {};
  };
  using state_pre_t = decltype(state_pre);
  STATIC_REQUIRE(std::is_copy_constructible_v<state_pre_t>);
  STATIC_REQUIRE(graph_state_pre_handler_probe::template can_create_v<state_pre_t, state_pre_t>);
  STATIC_REQUIRE(std::is_constructible_v<wh::compose::graph_state_pre_handler, state_pre_t>);

  wh::compose::graph_state_pre_handler pre_handler{state_pre};
  REQUIRE(static_cast<bool>(pre_handler));

  auto sync_run_for_probe =
      [counter = std::make_shared<int>(0)](
          wh::compose::graph_value &, wh::core::run_context &,
          const wh::compose::node_runtime &) mutable -> wh::core::result<wh::compose::graph_value> {
    ++*counter;
    return wh::compose::detail::make_graph_unit_value();
  };
  auto sync_wrapper = make_sync_wrapper(sync_run_for_probe);
  using sync_wrapper_t = decltype(sync_wrapper);
  STATIC_REQUIRE(std::is_copy_constructible_v<sync_wrapper_t>);
  STATIC_REQUIRE(node_sync_factory_probe::template can_create_v<sync_wrapper_t, sync_wrapper_t>);
  STATIC_REQUIRE(std::is_constructible_v<wh::compose::node_sync_factory, sync_wrapper_t>);

  auto sync_run_for_bind =
      [counter = std::make_shared<int>(0)](
          wh::compose::graph_value &, wh::core::run_context &,
          const wh::compose::node_runtime &) mutable -> wh::core::result<wh::compose::graph_value> {
    ++*counter;
    return wh::compose::detail::make_graph_unit_value();
  };
  auto sync_factory = wh::compose::detail::bind_node_sync_factory(std::move(sync_run_for_bind));
  REQUIRE(static_cast<bool>(sync_factory));

  auto async_run_for_probe =
      [counter = std::make_shared<int>(0)](
          wh::compose::graph_value &, wh::core::run_context &,
          const wh::compose::node_runtime &) mutable -> wh::compose::graph_sender {
    ++*counter;
    return wh::compose::detail::ready_graph_unit_sender();
  };
  auto async_wrapper = make_async_wrapper(async_run_for_probe);
  using async_wrapper_t = decltype(async_wrapper);
  STATIC_REQUIRE(std::is_copy_constructible_v<async_wrapper_t>);
  STATIC_REQUIRE(node_async_factory_probe::template can_create_v<async_wrapper_t, async_wrapper_t>);
  STATIC_REQUIRE(std::is_constructible_v<wh::compose::node_async_factory, async_wrapper_t>);

  auto async_run_for_bind =
      [counter = std::make_shared<int>(0)](
          wh::compose::graph_value &, wh::core::run_context &,
          const wh::compose::node_runtime &) mutable -> wh::compose::graph_sender {
    ++*counter;
    return wh::compose::detail::ready_graph_unit_sender();
  };
  auto async_factory = wh::compose::detail::bind_node_async_factory(std::move(async_run_for_bind));
  REQUIRE(static_cast<bool>(async_factory));
}
