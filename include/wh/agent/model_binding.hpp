// Defines one semantic authored model-binding wrapper used by agent-family
// shells to defer compose model-node materialization until lower time.
#pragma once

#include <concepts>
#include <string>
#include <type_traits>
#include <utility>

#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/node/component.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"

namespace wh::agent {

/// Semantic authored binding for one model-role slot.
class model_binding {
public:
  using lower_fn =
      wh::core::callback_function<wh::core::result<wh::compose::component_node>(std::string) const>;

  model_binding() noexcept = default;

  model_binding(const wh::compose::node_contract input_contract,
                const wh::compose::node_contract output_contract,
                const wh::compose::node_exec_mode exec_mode, lower_fn lower) noexcept
      : input_contract_(input_contract), output_contract_(output_contract), exec_mode_(exec_mode),
        lower_(std::move(lower)) {}

  [[nodiscard]] auto input_contract() const noexcept -> wh::compose::node_contract {
    return input_contract_;
  }

  [[nodiscard]] auto output_contract() const noexcept -> wh::compose::node_contract {
    return output_contract_;
  }

  [[nodiscard]] auto exec_mode() const noexcept -> wh::compose::node_exec_mode {
    return exec_mode_;
  }

  /// Materializes one compose model node using the caller-owned stable key.
  [[nodiscard]] auto materialize(std::string key) const
      -> wh::core::result<wh::compose::component_node> {
    if (key.empty()) {
      return wh::core::result<wh::compose::component_node>::failure(
          wh::core::errc::invalid_argument);
    }
    if (!static_cast<bool>(lower_)) {
      return wh::core::result<wh::compose::component_node>::failure(
          wh::core::errc::not_supported);
    }
    return lower_(std::move(key));
  }

private:
  wh::compose::node_contract input_contract_{wh::compose::node_contract::value};
  wh::compose::node_contract output_contract_{wh::compose::node_contract::value};
  wh::compose::node_exec_mode exec_mode_{wh::compose::node_exec_mode::sync};
  lower_fn lower_{nullptr};
};

template <wh::compose::node_contract From, wh::compose::node_contract To,
          wh::compose::node_exec_mode Exec = wh::compose::node_exec_mode::sync, typename model_t,
          typename options_t = wh::compose::graph_add_node_options>
  requires std::constructible_from<wh::compose::graph_add_node_options, options_t &&> &&
           requires(std::string key, std::remove_cvref_t<model_t> model, options_t options) {
             wh::compose::make_component_node<wh::compose::component_kind::model, From, To, Exec>(
                 std::move(key), std::move(model), std::forward<options_t>(options));
           }
/// Captures one authored model implementation and defers node materialization
/// until the surrounding shell lowerer provides a stable internal key.
[[nodiscard]] inline auto make_model_binding(model_t &&model, options_t &&options = {})
    -> model_binding {
  using stored_model_t = std::remove_cvref_t<model_t>;
  auto stored_model = stored_model_t{std::forward<model_t>(model)};
  auto stored_options = wh::compose::graph_add_node_options{std::forward<options_t>(options)};
  return model_binding{
      From,
      To,
      Exec,
      typename model_binding::lower_fn{
          [model = std::move(stored_model), options = std::move(stored_options)](
              std::string key) -> wh::core::result<wh::compose::component_node> {
            return wh::compose::make_component_node<wh::compose::component_kind::model, From, To,
                                                    Exec>(std::move(key), model, options);
          }},
  };
}

/// Verifies that one authored model binding matches the shell-native
/// input/output boundary expected by the public surface.
[[nodiscard]] inline auto validate_model_binding(const model_binding &binding,
                                                 const wh::compose::node_contract expected_input,
                                                 const wh::compose::node_contract expected_output)
    -> wh::core::result<void> {
  if (binding.input_contract() != expected_input || binding.output_contract() != expected_output) {
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  return {};
}

} // namespace wh::agent
