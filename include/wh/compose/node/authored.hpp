// Defines authored node types accepted by compose builders.
#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/node/compiled.hpp"
#include "wh/compose/node/detail/gate.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/compose/types.hpp"

namespace wh::compose {

/// Metadata carried by one authored node before compile.
struct node_descriptor {
  /// Stable node key used in graph topology and diagnostics.
  std::string key{};
  /// Public node family selected by the authoring API.
  node_kind kind{node_kind::component};
  /// Execution mode exposed by this authored node.
  node_exec_mode exec_mode{node_exec_mode::sync};
  /// States whether `exec_mode` came from user choice or built-in node rules.
  node_exec_origin exec_origin{default_exec_origin(node_kind::component)};
  /// Input payload contract accepted by this node.
  node_contract input_contract{node_contract::value};
  /// Output payload contract produced by this node.
  node_contract output_contract{node_contract::value};
  /// Compile-visible input boundary facts used before runtime lowering.
  input_gate input_gate_info{input_gate::open()};
  /// Compile-visible output boundary facts used before runtime lowering.
  output_gate output_gate_info{output_gate::dynamic()};
};

namespace detail {

using authored_node_lower =
    wh::core::callback_function<wh::core::result<compiled_node>(
        std::string, graph_add_node_options)>;

template <typename options_t, typename type_t, typename label_t>
[[nodiscard]] inline auto decorate_node_options(options_t &&options,
                                                type_t &&type, label_t &&label)
    -> graph_add_node_options {
  auto node_options = graph_add_node_options{std::forward<options_t>(options)};
  if (node_options.type.empty()) {
    node_options.type = std::string{std::forward<type_t>(type)};
  }
  if (node_options.label.empty()) {
    node_options.label = std::string{std::forward<label_t>(label)};
  }
  return node_options;
}

template <typename key_t, typename options_t, typename type_t, typename label_t>
[[nodiscard]] inline auto
decorate_named_node_options(key_t &&key, options_t &&options, type_t &&type,
                            label_t &&label) -> graph_add_node_options {
  auto node_options = decorate_node_options(std::forward<options_t>(options),
                                            std::forward<type_t>(type),
                                            std::forward<label_t>(label));
  if (node_options.name.empty()) {
    node_options.name = std::string{std::forward<key_t>(key)};
  }
  return node_options;
}

} // namespace detail

struct component_payload {
  /// Component family used to pick binding rules during compile.
  component_kind kind{component_kind::custom};
  /// Callback that turns authored state into one compiled node.
  detail::authored_node_lower lower{nullptr};
};

struct lambda_payload {
  /// Callback that turns authored state into one compiled node.
  detail::authored_node_lower lower{nullptr};
};

struct subgraph_payload {
  /// Callback that turns authored state into one compiled node.
  detail::authored_node_lower lower{nullptr};
};

struct tools_payload {
  /// Frozen dispatch registry compiled into this tools node.
  tool_registry registry{};
  /// Frozen runtime options shared by all calls inside this tools node.
  tools_options runtime_options{};
};

class graph;
class component_node;
class lambda_node;
class subgraph_node;
class tools_node;
class passthrough_node;

namespace detail {
template <typename node_t>
[[nodiscard]] inline auto authored_input_contract(const node_t &node) noexcept
    -> node_contract {
  return node.descriptor().input_contract;
}

template <typename node_t>
[[nodiscard]] inline auto authored_output_contract(const node_t &node) noexcept
    -> node_contract {
  return node.descriptor().output_contract;
}

template <typename node_t>
[[nodiscard]] inline auto authored_input_gate(const node_t &node) noexcept
    -> input_gate {
  return node.descriptor().input_gate_info;
}

template <typename node_t>
[[nodiscard]] inline auto authored_output_gate(const node_t &node) noexcept
    -> output_gate {
  return node.descriptor().output_gate_info;
}

} // namespace detail

/// Authored node handle for component bindings.
class component_node {
public:
  component_node() = default;
  component_node(node_descriptor descriptor, component_payload payload,
                 graph_add_node_options options = {}) noexcept
      : descriptor_(std::move(descriptor)), payload_(std::move(payload)),
        options_(std::move(options)) {}

  [[nodiscard]] auto key() const noexcept -> std::string_view {
    return descriptor_.key;
  }
  [[nodiscard]] auto input_contract() const noexcept -> node_contract {
    return descriptor_.input_contract;
  }
  [[nodiscard]] auto output_contract() const noexcept -> node_contract {
    return descriptor_.output_contract;
  }
  [[nodiscard]] auto input_gate() const noexcept -> wh::compose::input_gate {
    return descriptor_.input_gate_info;
  }
  [[nodiscard]] auto output_gate() const noexcept -> wh::compose::output_gate {
    return descriptor_.output_gate_info;
  }
  [[nodiscard]] auto exec_mode() const noexcept -> node_exec_mode {
    return descriptor_.exec_mode;
  }
  [[nodiscard]] auto exec_origin() const noexcept -> node_exec_origin {
    return descriptor_.exec_origin;
  }
  [[nodiscard]] auto descriptor() const noexcept -> const node_descriptor & {
    return descriptor_;
  }
  [[nodiscard]] auto options() const noexcept
      -> const graph_add_node_options & {
    return options_;
  }
  auto mutable_options() noexcept -> graph_add_node_options & {
    return options_;
  }

private:
  [[nodiscard]] auto compile() const & -> wh::core::result<compiled_node>;
  [[nodiscard]] auto compile() && -> wh::core::result<compiled_node>;

  node_descriptor descriptor_{};
  component_payload payload_{};
  graph_add_node_options options_{};

  friend class graph;
};

/// Authored node handle for lambda-based graph steps.
class lambda_node {
public:
  lambda_node() = default;
  lambda_node(node_descriptor descriptor, lambda_payload payload,
              graph_add_node_options options = {}) noexcept
      : descriptor_(std::move(descriptor)), payload_(std::move(payload)),
        options_(std::move(options)) {}

  [[nodiscard]] auto key() const noexcept -> std::string_view {
    return descriptor_.key;
  }
  [[nodiscard]] auto input_contract() const noexcept -> node_contract {
    return descriptor_.input_contract;
  }
  [[nodiscard]] auto output_contract() const noexcept -> node_contract {
    return descriptor_.output_contract;
  }
  [[nodiscard]] auto input_gate() const noexcept -> wh::compose::input_gate {
    return descriptor_.input_gate_info;
  }
  [[nodiscard]] auto output_gate() const noexcept -> wh::compose::output_gate {
    return descriptor_.output_gate_info;
  }
  [[nodiscard]] auto exec_mode() const noexcept -> node_exec_mode {
    return descriptor_.exec_mode;
  }
  [[nodiscard]] auto exec_origin() const noexcept -> node_exec_origin {
    return descriptor_.exec_origin;
  }
  [[nodiscard]] auto descriptor() const noexcept -> const node_descriptor & {
    return descriptor_;
  }
  [[nodiscard]] auto options() const noexcept
      -> const graph_add_node_options & {
    return options_;
  }
  auto mutable_options() noexcept -> graph_add_node_options & {
    return options_;
  }

private:
  [[nodiscard]] auto compile() const & -> wh::core::result<compiled_node>;
  [[nodiscard]] auto compile() && -> wh::core::result<compiled_node>;

  node_descriptor descriptor_{};
  lambda_payload payload_{};
  graph_add_node_options options_{};

  friend class graph;
};

/// Authored node handle for nested graph invocation.
class subgraph_node {
public:
  subgraph_node() = default;
  subgraph_node(node_descriptor descriptor, subgraph_payload payload,
                graph_add_node_options options = {}) noexcept
      : descriptor_(std::move(descriptor)), payload_(std::move(payload)),
        options_(std::move(options)) {}

  [[nodiscard]] auto key() const noexcept -> std::string_view {
    return descriptor_.key;
  }
  [[nodiscard]] auto input_contract() const noexcept -> node_contract {
    return descriptor_.input_contract;
  }
  [[nodiscard]] auto output_contract() const noexcept -> node_contract {
    return descriptor_.output_contract;
  }
  [[nodiscard]] auto input_gate() const noexcept -> wh::compose::input_gate {
    return descriptor_.input_gate_info;
  }
  [[nodiscard]] auto output_gate() const noexcept -> wh::compose::output_gate {
    return descriptor_.output_gate_info;
  }
  [[nodiscard]] auto exec_mode() const noexcept -> node_exec_mode {
    return descriptor_.exec_mode;
  }
  [[nodiscard]] auto exec_origin() const noexcept -> node_exec_origin {
    return descriptor_.exec_origin;
  }
  [[nodiscard]] auto descriptor() const noexcept -> const node_descriptor & {
    return descriptor_;
  }
  [[nodiscard]] auto options() const noexcept
      -> const graph_add_node_options & {
    return options_;
  }
  auto mutable_options() noexcept -> graph_add_node_options & {
    return options_;
  }

private:
  [[nodiscard]] auto compile() const & -> wh::core::result<compiled_node>;
  [[nodiscard]] auto compile() && -> wh::core::result<compiled_node>;

  node_descriptor descriptor_{};
  subgraph_payload payload_{};
  graph_add_node_options options_{};

  friend class graph;
};

/// Authored node handle for tool-dispatch groups.
class tools_node {
public:
  tools_node() = default;
  tools_node(node_descriptor descriptor, tools_payload payload,
             graph_add_node_options options = {}) noexcept
      : descriptor_(std::move(descriptor)), payload_(std::move(payload)),
        options_(std::move(options)) {}

  [[nodiscard]] auto key() const noexcept -> std::string_view {
    return descriptor_.key;
  }
  [[nodiscard]] auto input_contract() const noexcept -> node_contract {
    return descriptor_.input_contract;
  }
  [[nodiscard]] auto output_contract() const noexcept -> node_contract {
    return descriptor_.output_contract;
  }
  [[nodiscard]] auto input_gate() const noexcept -> wh::compose::input_gate {
    return descriptor_.input_gate_info;
  }
  [[nodiscard]] auto output_gate() const noexcept -> wh::compose::output_gate {
    return descriptor_.output_gate_info;
  }
  [[nodiscard]] auto exec_mode() const noexcept -> node_exec_mode {
    return descriptor_.exec_mode;
  }
  [[nodiscard]] auto exec_origin() const noexcept -> node_exec_origin {
    return descriptor_.exec_origin;
  }
  [[nodiscard]] auto descriptor() const noexcept -> const node_descriptor & {
    return descriptor_;
  }
  [[nodiscard]] auto options() const noexcept
      -> const graph_add_node_options & {
    return options_;
  }
  auto mutable_options() noexcept -> graph_add_node_options & {
    return options_;
  }

private:
  [[nodiscard]] auto compile() const & -> wh::core::result<compiled_node>;
  [[nodiscard]] auto compile() && -> wh::core::result<compiled_node>;

  node_descriptor descriptor_{};
  tools_payload payload_{};
  graph_add_node_options options_{};

  friend class graph;
};

/// Authored node handle that forwards payload unchanged.
class passthrough_node {
public:
  passthrough_node() = default;
  passthrough_node(node_descriptor descriptor,
                   graph_add_node_options options = {}) noexcept
      : descriptor_(std::move(descriptor)), options_(std::move(options)) {}

  [[nodiscard]] auto key() const noexcept -> std::string_view {
    return descriptor_.key;
  }
  [[nodiscard]] auto input_contract() const noexcept -> node_contract {
    return descriptor_.input_contract;
  }
  [[nodiscard]] auto output_contract() const noexcept -> node_contract {
    return descriptor_.output_contract;
  }
  [[nodiscard]] auto input_gate() const noexcept -> wh::compose::input_gate {
    return descriptor_.input_gate_info;
  }
  [[nodiscard]] auto output_gate() const noexcept -> wh::compose::output_gate {
    return descriptor_.output_gate_info;
  }
  [[nodiscard]] auto exec_mode() const noexcept -> node_exec_mode {
    return descriptor_.exec_mode;
  }
  [[nodiscard]] auto exec_origin() const noexcept -> node_exec_origin {
    return descriptor_.exec_origin;
  }
  [[nodiscard]] auto descriptor() const noexcept -> const node_descriptor & {
    return descriptor_;
  }
  [[nodiscard]] auto options() const noexcept
      -> const graph_add_node_options & {
    return options_;
  }
  auto mutable_options() noexcept -> graph_add_node_options & {
    return options_;
  }

private:
  [[nodiscard]] auto compile() const & -> wh::core::result<compiled_node>;
  [[nodiscard]] auto compile() && -> wh::core::result<compiled_node>;

  node_descriptor descriptor_{};
  graph_add_node_options options_{};

  friend class graph;
};

template <typename node_t>
concept authored_node_like =
    std::same_as<std::remove_cvref_t<node_t>, component_node> ||
    std::same_as<std::remove_cvref_t<node_t>, lambda_node> ||
    std::same_as<std::remove_cvref_t<node_t>, subgraph_node> ||
    std::same_as<std::remove_cvref_t<node_t>, tools_node> ||
    std::same_as<std::remove_cvref_t<node_t>, passthrough_node>;

/// Sum type covering all authored node handles accepted by compose.
using authored_node = std::variant<component_node, lambda_node, subgraph_node,
                                   tools_node, passthrough_node>;

template <authored_node_like node_t>
[[nodiscard]] inline auto authored_key(const node_t &node) noexcept
    -> std::string_view {
  return node.descriptor().key;
}

template <authored_node_like node_t>
[[nodiscard]] inline auto authored_options(const node_t &node) noexcept
    -> const graph_add_node_options & {
  return node.options();
}

template <authored_node_like node_t>
[[nodiscard]] inline auto authored_options(node_t &node) noexcept
    -> graph_add_node_options & {
  return node.mutable_options();
}

[[nodiscard]] inline auto authored_key(const authored_node &node) noexcept
    -> std::string_view {
  return std::visit([](const auto &value) { return value.key(); }, node);
}

namespace detail {

[[nodiscard]] inline auto
authored_input_gate(const authored_node &node) noexcept -> input_gate {
  return std::visit([](const auto &value) { return value.input_gate(); }, node);
}

[[nodiscard]] inline auto
authored_output_gate(const authored_node &node) noexcept -> output_gate {
  return std::visit([](const auto &value) { return value.output_gate(); },
                    node);
}

} // namespace detail

[[nodiscard]] inline auto
authored_input_contract(const authored_node &node) noexcept -> node_contract {
  return std::visit(
      [](const auto &value) -> node_contract { return value.input_contract(); },
      node);
}

[[nodiscard]] inline auto
authored_output_contract(const authored_node &node) noexcept -> node_contract {
  return std::visit(
      [](const auto &value) -> node_contract {
        return value.output_contract();
      },
      node);
}

template <typename node_t>
[[nodiscard]] inline auto authored_kind(const node_t &node) noexcept
    -> node_kind {
  return node.descriptor().kind;
}

[[nodiscard]] inline auto authored_kind(const authored_node &node) noexcept
    -> node_kind {
  return std::visit(
      [](const auto &value) -> node_kind { return value.descriptor().kind; },
      node);
}

[[nodiscard]] inline auto authored_options(const authored_node &node) noexcept
    -> const graph_add_node_options & {
  return std::visit(
      [](const auto &value) -> const graph_add_node_options & {
        return value.options();
      },
      node);
}

[[nodiscard]] inline auto authored_options(authored_node &node) noexcept
    -> graph_add_node_options & {
  return std::visit(
      [](auto &value) -> graph_add_node_options & {
        return value.mutable_options();
      },
      node);
}

} // namespace wh::compose
