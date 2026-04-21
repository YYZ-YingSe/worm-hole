// Defines shared checkpoint runtime helpers for session restore, codecs, and persistence.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/compose/graph/detail/graph_core.hpp"
#include "wh/compose/graph/detail/runtime/input.hpp"
#include "wh/compose/graph/detail/runtime/pending_inputs.hpp"
#include "wh/compose/graph/detail/runtime/state.hpp"
#include "wh/compose/graph/keys.hpp"
#include "wh/compose/graph/restore_check.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/path.hpp"
#include "wh/compose/runtime/checkpoint.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose::detail::checkpoint_runtime {

enum class restore_scope : std::uint8_t {
  full = 0U,
  forwarded_only,
};

struct prepared_restore {
  checkpoint_state checkpoint{};
  std::string checkpoint_id_hint{};
  bool restore_skip_pre_handlers{false};
};

[[nodiscard]] inline auto
resolve_forwarded_restore_key(const std::string_view graph_name,
                              const node_path &runtime_path) -> std::string {
  if (!runtime_path.empty()) {
    return runtime_path.to_string("/");
  }
  return std::string{graph_name};
}

inline auto set_error_detail(runtime_state::invoke_outputs &outputs,
                             const wh::core::error_code code,
                             const std::string_view checkpoint_id,
                             const std::string_view operation) -> void {
  if (outputs.checkpoint_error.has_value()) {
    return;
  }
  outputs.checkpoint_error = checkpoint_error_detail{
      .code = code,
      .checkpoint_id = std::string{checkpoint_id},
      .operation = std::string{operation},
  };
}

[[nodiscard]] inline auto
resolve_id_hint(const checkpoint_load_options &options) -> std::string {
  if (options.checkpoint_id.has_value() && !options.checkpoint_id->empty()) {
    return *options.checkpoint_id;
  }
  return std::string{};
}

[[nodiscard]] inline auto
resolve_id_hint(const checkpoint_save_options &options) -> std::string {
  if (options.checkpoint_id.has_value() && !options.checkpoint_id->empty()) {
    return *options.checkpoint_id;
  }
  return std::string{};
}

[[nodiscard]] inline auto default_serializer() -> const checkpoint_serializer & {
  static const checkpoint_serializer serializer{
      .encode = checkpoint_serializer_encode{[](checkpoint_state &&state,
                                                wh::core::run_context &)
                                                 -> wh::core::result<graph_value> {
        return wh::core::any(std::move(state));
      }},
      .decode = checkpoint_serializer_decode{[](graph_value &&payload,
                                                wh::core::run_context &)
                                                 -> wh::core::result<checkpoint_state> {
        if (auto *typed = wh::core::any_cast<checkpoint_state>(&payload);
            typed != nullptr) {
          return std::move(*typed);
        }
        return wh::core::result<checkpoint_state>::failure(
            wh::core::errc::type_mismatch);
      }},
  };
  return serializer;
}

[[nodiscard]] inline auto
resolve_serializer(const runtime_state::invoke_config &config)
    -> wh::core::result<const checkpoint_serializer *> {
  if (config.checkpoint_serializer_ptr == nullptr) {
    return std::addressof(default_serializer());
  }
  const auto *serializer = config.checkpoint_serializer_ptr;
  if (!serializer->encode || !serializer->decode) {
    return wh::core::result<const checkpoint_serializer *>::failure(
        wh::core::errc::invalid_argument);
  }
  return serializer;
}

[[nodiscard]] inline auto
roundtrip_with_serializer(checkpoint_state &&checkpoint,
                          wh::core::run_context &context,
                          const runtime_state::invoke_config &config)
    -> wh::core::result<checkpoint_state> {
  auto serializer_ref = resolve_serializer(config);
  if (serializer_ref.has_error()) {
    return wh::core::result<checkpoint_state>::failure(serializer_ref.error());
  }
  auto encoded = serializer_ref.value()->encode(std::move(checkpoint), context);
  if (encoded.has_error()) {
    return wh::core::result<checkpoint_state>::failure(encoded.error());
  }
  auto decoded =
      serializer_ref.value()->decode(std::move(encoded).value(), context);
  if (decoded.has_error()) {
    return wh::core::result<checkpoint_state>::failure(decoded.error());
  }
  return decoded;
}

[[nodiscard]] inline auto
roundtrip_with_serializer(const checkpoint_state &checkpoint,
                          wh::core::run_context &context,
                          const runtime_state::invoke_config &config)
    -> wh::core::result<checkpoint_state> {
  auto owned = wh::core::into_owned(checkpoint);
  if (owned.has_error()) {
    return wh::core::result<checkpoint_state>::failure(owned.error());
  }
  return roundtrip_with_serializer(std::move(owned).value(), context, config);
}

struct runtime_backend {
  checkpoint_store *store{nullptr};
  checkpoint_backend *backend{nullptr};
};

[[nodiscard]] inline auto
resolve_runtime_backend(const runtime_state::invoke_config &config)
    -> wh::core::result<runtime_backend> {
  runtime_backend resolved{};
  resolved.store = config.checkpoint_store_ptr;
  resolved.backend = config.checkpoint_backend_ptr;
  if (resolved.backend != nullptr &&
      (!resolved.backend->prepare_restore || !resolved.backend->save)) {
    return wh::core::result<runtime_backend>::failure(
        wh::core::errc::invalid_argument);
  }
  if (resolved.store != nullptr && resolved.backend != nullptr) {
    return wh::core::result<runtime_backend>::failure(
        wh::core::errc::invalid_argument);
  }
  return resolved;
}

[[nodiscard]] inline auto
make_node_path_from_state_key(const std::string_view node_key) -> node_path {
  auto parsed = parse_node_path(node_key);
  if (parsed.has_value()) {
    return std::move(parsed).value();
  }
  return make_node_path({node_key});
}

inline auto apply_node_hooks(wh::core::run_context &context,
                             const checkpoint_node_hooks *modifiers,
                             checkpoint_state &state) -> wh::core::result<void> {
  if (modifiers == nullptr || modifiers->empty()) {
    return {};
  }
  for (auto &lifecycle : state.runtime.lifecycle) {
    const auto node_state_path = make_node_path_from_state_key(lifecycle.key);
    for (const auto &path_modifier : *modifiers) {
      if (!path_modifier.modifier || path_modifier.path.empty()) {
        return wh::core::result<void>::failure(
            wh::core::errc::invalid_argument);
      }
      const auto matched = path_modifier.include_descendants
                               ? node_state_path.starts_with(path_modifier.path)
                               : node_state_path == path_modifier.path;
      if (!matched) {
        continue;
      }
      auto status = path_modifier.modifier(lifecycle, context);
      if (status.has_error()) {
        return wh::core::result<void>::failure(status.error());
      }
    }
  }
  return {};
}

struct captured_runtime_io {
  std::vector<checkpoint_runtime_slot> node_outputs{};
  std::vector<checkpoint_runtime_slot> edge_values{};
  std::vector<checkpoint_runtime_slot> edge_readers{};
  std::vector<checkpoint_runtime_slot> merged_readers{};
  std::vector<checkpoint_reader_lane> merged_reader_lanes{};
  std::optional<graph_value> final_output_reader{};
};

[[nodiscard]] inline auto capture_runtime_value_slot(const std::uint32_t slot_id,
                                                     graph_value &value)
    -> wh::core::result<checkpoint_runtime_slot> {
  auto forked = fork_graph_value(value);
  if (forked.has_error()) {
    return wh::core::result<checkpoint_runtime_slot>::failure(forked.error());
  }
  return checkpoint_runtime_slot{
      .slot_id = slot_id,
      .value = std::move(forked).value(),
  };
}

[[nodiscard]] inline auto capture_runtime_reader_slot(
    const std::uint32_t slot_id, graph_stream_reader &reader)
    -> wh::core::result<checkpoint_runtime_slot> {
  auto forked = detail::fork_graph_reader(reader);
  if (forked.has_error()) {
    return wh::core::result<checkpoint_runtime_slot>::failure(forked.error());
  }
  return checkpoint_runtime_slot{
      .slot_id = slot_id,
      .value = graph_value{std::move(forked).value()},
  };
}

[[nodiscard]] inline auto capture_runtime_io(input_runtime::io_storage &storage)
    -> wh::core::result<captured_runtime_io> {
  captured_runtime_io captured{};
  captured.node_outputs.reserve(storage.node_values.size());
  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(storage.node_values.size());
       ++node_id) {
    auto &value = storage.node_values[node_id];
    if (!value.has_value()) {
      continue;
    }
    auto slot = capture_runtime_value_slot(node_id, value);
    if (slot.has_error()) {
      return wh::core::result<captured_runtime_io>::failure(slot.error());
    }
    captured.node_outputs.push_back(std::move(slot).value());
  }

  captured.edge_values.reserve(storage.edge_values.size());
  for (std::uint32_t edge_id = 0U;
       edge_id < static_cast<std::uint32_t>(storage.edge_values.size());
       ++edge_id) {
    if (!storage.edge_value_valid.test(edge_id)) {
      continue;
    }
    auto slot = capture_runtime_value_slot(edge_id, storage.edge_values[edge_id]);
    if (slot.has_error()) {
      return wh::core::result<captured_runtime_io>::failure(slot.error());
    }
    captured.edge_values.push_back(std::move(slot).value());
  }

  captured.edge_readers.reserve(storage.edge_readers.size());
  for (std::uint32_t edge_id = 0U;
       edge_id < static_cast<std::uint32_t>(storage.edge_readers.size());
       ++edge_id) {
    if (!storage.edge_reader_valid.test(edge_id)) {
      continue;
    }
    auto slot = capture_runtime_reader_slot(edge_id, storage.edge_readers[edge_id]);
    if (slot.has_error()) {
      return wh::core::result<captured_runtime_io>::failure(slot.error());
    }
    captured.edge_readers.push_back(std::move(slot).value());
  }

  captured.merged_readers.reserve(storage.merged_readers.size());
  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(storage.merged_readers.size());
       ++node_id) {
    if (!storage.merged_reader_valid.test(node_id)) {
      continue;
    }
    auto slot = capture_runtime_reader_slot(node_id, storage.merged_readers[node_id]);
    if (slot.has_error()) {
      return wh::core::result<captured_runtime_io>::failure(slot.error());
    }
    captured.merged_readers.push_back(std::move(slot).value());
  }

  captured.merged_reader_lanes.reserve(storage.merged_reader_lane_states.size());
  for (std::uint32_t edge_id = 0U;
       edge_id < static_cast<std::uint32_t>(storage.merged_reader_lane_states.size());
       ++edge_id) {
    const auto lane_state = storage.merged_reader_lane_states[edge_id];
    if (lane_state == input_runtime::reader_lane_state::unseen) {
      continue;
    }
    captured.merged_reader_lanes.push_back(checkpoint_reader_lane{
        .edge_id = edge_id,
        .state = lane_state == input_runtime::reader_lane_state::attached
                     ? checkpoint_reader_lane_state::attached
                     : checkpoint_reader_lane_state::disabled,
    });
  }

  if (storage.final_output_reader.has_value()) {
    auto forked = detail::fork_graph_reader(*storage.final_output_reader);
    if (forked.has_error()) {
      return wh::core::result<captured_runtime_io>::failure(forked.error());
    }
    captured.final_output_reader = graph_value{std::move(forked).value()};
  }

  return captured;
}

template <typename slot_list_t>
inline auto restore_value_slots(slot_list_t &slots,
                                std::vector<graph_value> &storage,
                                wh::compose::detail::dynamic_bitset &valid)
    -> wh::core::result<void> {
  for (auto &slot : slots) {
    if (slot.slot_id >= storage.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    storage[slot.slot_id] = std::move(slot.value);
    valid.set(slot.slot_id);
  }
  return {};
}

template <typename slot_list_t>
inline auto restore_reader_slots(slot_list_t &slots,
                                 std::vector<graph_stream_reader> &storage,
                                 wh::compose::detail::dynamic_bitset &valid)
    -> wh::core::result<void> {
  for (auto &slot : slots) {
    if (slot.slot_id >= storage.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    auto *reader = wh::core::any_cast<graph_stream_reader>(&slot.value);
    if (reader == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    storage[slot.slot_id] = std::move(*reader);
    valid.set(slot.slot_id);
  }
  return {};
}

inline auto restore_runtime_slots(
    input_runtime::io_storage &storage,
    std::vector<checkpoint_runtime_slot> &node_outputs,
    std::vector<checkpoint_runtime_slot> &edge_values,
    std::vector<checkpoint_runtime_slot> &edge_readers,
    std::vector<checkpoint_runtime_slot> &merged_readers,
    const std::vector<checkpoint_reader_lane> &merged_reader_lanes,
    std::optional<graph_value> &final_output_reader) -> wh::core::result<void> {
  auto restored_values =
      restore_value_slots(node_outputs, storage.node_values, storage.output_valid);
  if (restored_values.has_error()) {
    return restored_values;
  }
  restored_values =
      restore_value_slots(edge_values, storage.edge_values, storage.edge_value_valid);
  if (restored_values.has_error()) {
    return restored_values;
  }

  auto restored_readers =
      restore_reader_slots(edge_readers, storage.edge_readers, storage.edge_reader_valid);
  if (restored_readers.has_error()) {
    return restored_readers;
  }
  restored_readers = restore_reader_slots(merged_readers, storage.merged_readers,
                                          storage.merged_reader_valid);
  if (restored_readers.has_error()) {
    return restored_readers;
  }

  std::fill(storage.merged_reader_lane_states.begin(),
            storage.merged_reader_lane_states.end(),
            input_runtime::reader_lane_state::unseen);
  for (const auto &lane : merged_reader_lanes) {
    if (lane.edge_id >= storage.merged_reader_lane_states.size()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    storage.merged_reader_lane_states[lane.edge_id] =
        lane.state == checkpoint_reader_lane_state::attached
            ? input_runtime::reader_lane_state::attached
            : input_runtime::reader_lane_state::disabled;
  }

  if (final_output_reader.has_value()) {
    auto *reader = wh::core::any_cast<graph_stream_reader>(&*final_output_reader);
    if (reader == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    storage.final_output_reader = std::move(*reader);
  }
  return {};
}

[[nodiscard]] inline auto node_key_for_slot(
    const std::span<const std::string> node_keys, const std::uint32_t slot_id)
    -> wh::core::result<std::string_view> {
  if (slot_id >= node_keys.size()) {
    return wh::core::result<std::string_view>::failure(
        wh::core::errc::not_found);
  }
  return node_keys[slot_id];
}

[[nodiscard]] inline auto edge_target_key_for_slot(
    const std::span<const std::string> node_keys,
    const std::span<const detail::graph_core::indexed_edge> indexed_edges,
    const std::uint32_t slot_id) -> wh::core::result<std::string_view> {
  if (slot_id >= indexed_edges.size()) {
    return wh::core::result<std::string_view>::failure(
        wh::core::errc::not_found);
  }
  return node_key_for_slot(node_keys, indexed_edges[slot_id].to);
}

inline auto apply_stream_codecs_for_save(
    checkpoint_state &checkpoint, wh::core::run_context &context,
    const checkpoint_stream_codecs *registry,
    const std::span<const std::string> node_keys,
    const std::span<const detail::graph_core::indexed_edge> indexed_edges,
    const std::uint32_t end_node_id) -> wh::core::result<void> {
  const bool has_registry = registry != nullptr;

  const auto convert_one = [&](const std::string_view node_key,
                               graph_value &payload,
                               const bool tolerate_channel_closed)
      -> wh::core::result<void> {
    auto *reader = wh::core::any_cast<graph_stream_reader>(&payload);
    if (reader == nullptr) {
      return {};
    }
    if (!has_registry) {
      return wh::core::result<void>::failure(wh::core::errc::not_supported);
    }
    const auto converter_iter = registry->find(node_key);
    if (converter_iter == registry->end() || !converter_iter->second.to_value) {
      return wh::core::result<void>::failure(wh::core::errc::not_supported);
    }
    auto converted = converter_iter->second.to_value(std::move(*reader), context);
    if (converted.has_error()) {
      if (tolerate_channel_closed &&
          converted.error() == wh::core::errc::channel_closed) {
        payload = wh::core::any(checkpoint_stream_value_payload{
            .value = wh::core::any(std::vector<graph_value>{}),
        });
        return {};
      }
      return wh::core::result<void>::failure(converted.error());
    }
    payload = wh::core::any(checkpoint_stream_value_payload{
        .value = std::move(converted).value(),
    });
    return {};
  };
  const auto convert_pending_inputs = [&](checkpoint_pending_inputs &pending)
      -> wh::core::result<void> {
    if (pending.entry.has_value()) {
      auto converted_start =
          convert_one(graph_start_node_key, *pending.entry, false);
      if (converted_start.has_error()) {
        return converted_start;
      }
    }

    for (auto &node_input : pending.nodes) {
      auto converted = convert_one(node_input.key, node_input.input, true);
      if (converted.has_error()) {
        return converted;
      }
    }
    return {};
  };

  const auto convert_slots = [&](auto resolve_key, auto &slots)
      -> wh::core::result<void> {
    for (auto &slot : slots) {
      auto node_key = resolve_key(slot.slot_id);
      if (node_key.has_error()) {
        return wh::core::result<void>::failure(node_key.error());
      }
      auto converted = convert_one(node_key.value(), slot.value, true);
      if (converted.has_error()) {
        return converted;
      }
    }
    return {};
  };

  const auto node_key_resolver = [&](const std::uint32_t slot_id)
      -> wh::core::result<std::string_view> {
    return node_key_for_slot(node_keys, slot_id);
  };
  const auto edge_key_resolver = [&](const std::uint32_t slot_id)
      -> wh::core::result<std::string_view> {
    return edge_target_key_for_slot(node_keys, indexed_edges, slot_id);
  };
  const auto convert_final_output = [&](std::optional<graph_value> &payload)
      -> wh::core::result<void> {
    if (!payload.has_value()) {
      return {};
    }
    auto node_key = node_key_for_slot(node_keys, end_node_id);
    if (node_key.has_error()) {
      return wh::core::result<void>::failure(node_key.error());
    }
    return convert_one(node_key.value(), *payload, true);
  };

  if (checkpoint.runtime.dag.has_value()) {
    auto &dag = *checkpoint.runtime.dag;
    auto converted = convert_pending_inputs(dag.pending_inputs);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_slots(node_key_resolver, dag.node_outputs);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_slots(edge_key_resolver, dag.edge_values);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_slots(edge_key_resolver, dag.edge_readers);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_slots(node_key_resolver, dag.merged_readers);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_final_output(dag.final_output_reader);
    if (converted.has_error()) {
      return converted;
    }
  }

  if (checkpoint.runtime.pregel.has_value()) {
    auto &pregel = *checkpoint.runtime.pregel;
    auto converted = convert_pending_inputs(pregel.pending_inputs);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_slots(node_key_resolver, pregel.node_outputs);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_slots(edge_key_resolver, pregel.edge_values);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_slots(edge_key_resolver, pregel.edge_readers);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_slots(node_key_resolver, pregel.merged_readers);
    if (converted.has_error()) {
      return converted;
    }
    converted = convert_final_output(pregel.final_output_reader);
    if (converted.has_error()) {
      return converted;
    }
  }

  return {};
}

inline auto apply_stream_codecs_for_load(
    checkpoint_state &checkpoint, wh::core::run_context &context,
    const checkpoint_stream_codecs *registry,
    const std::span<const std::string> node_keys,
    const std::span<const detail::graph_core::indexed_edge> indexed_edges,
    const std::uint32_t end_node_id) -> wh::core::result<void> {
  const bool has_registry = registry != nullptr;

  const auto restore_one = [&](const std::string_view node_key,
                               graph_value &payload)
      -> wh::core::result<void> {
    auto *stored = wh::core::any_cast<checkpoint_stream_value_payload>(&payload);
    if (stored == nullptr) {
      return {};
    }
    if (!has_registry) {
      return wh::core::result<void>::failure(wh::core::errc::not_supported);
    }
    const auto converter_iter = registry->find(node_key);
    if (converter_iter == registry->end() || !converter_iter->second.to_stream) {
      return wh::core::result<void>::failure(wh::core::errc::not_supported);
    }
    auto restored = converter_iter->second.to_stream(std::move(stored->value), context);
    if (restored.has_error()) {
      return wh::core::result<void>::failure(restored.error());
    }
    payload = wh::core::any(std::move(restored).value());
    return {};
  };
  const auto restore_pending_inputs = [&](checkpoint_pending_inputs &pending)
      -> wh::core::result<void> {
    if (pending.entry.has_value()) {
      auto restored = restore_one(graph_start_node_key, *pending.entry);
      if (restored.has_error()) {
        return restored;
      }
    }

    for (auto &node_input : pending.nodes) {
      auto restored = restore_one(node_input.key, node_input.input);
      if (restored.has_error()) {
        return restored;
      }
    }
    return {};
  };

  const auto restore_slots = [&](auto resolve_key, auto &slots)
      -> wh::core::result<void> {
    for (auto &slot : slots) {
      auto node_key = resolve_key(slot.slot_id);
      if (node_key.has_error()) {
        return wh::core::result<void>::failure(node_key.error());
      }
      auto restored = restore_one(node_key.value(), slot.value);
      if (restored.has_error()) {
        return restored;
      }
    }
    return {};
  };

  const auto node_key_resolver = [&](const std::uint32_t slot_id)
      -> wh::core::result<std::string_view> {
    return node_key_for_slot(node_keys, slot_id);
  };
  const auto edge_key_resolver = [&](const std::uint32_t slot_id)
      -> wh::core::result<std::string_view> {
    return edge_target_key_for_slot(node_keys, indexed_edges, slot_id);
  };
  const auto restore_final_output = [&](std::optional<graph_value> &payload)
      -> wh::core::result<void> {
    if (!payload.has_value()) {
      return {};
    }
    auto node_key = node_key_for_slot(node_keys, end_node_id);
    if (node_key.has_error()) {
      return wh::core::result<void>::failure(node_key.error());
    }
    return restore_one(node_key.value(), *payload);
  };

  if (checkpoint.runtime.dag.has_value()) {
    auto &dag = *checkpoint.runtime.dag;
    auto restored = restore_pending_inputs(dag.pending_inputs);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_slots(node_key_resolver, dag.node_outputs);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_slots(edge_key_resolver, dag.edge_values);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_slots(edge_key_resolver, dag.edge_readers);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_slots(node_key_resolver, dag.merged_readers);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_final_output(dag.final_output_reader);
    if (restored.has_error()) {
      return restored;
    }
  }

  if (checkpoint.runtime.pregel.has_value()) {
    auto &pregel = *checkpoint.runtime.pregel;
    auto restored = restore_pending_inputs(pregel.pending_inputs);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_slots(node_key_resolver, pregel.node_outputs);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_slots(edge_key_resolver, pregel.edge_values);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_slots(edge_key_resolver, pregel.edge_readers);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_slots(node_key_resolver, pregel.merged_readers);
    if (restored.has_error()) {
      return restored;
    }
    restored = restore_final_output(pregel.final_output_reader);
    if (restored.has_error()) {
      return restored;
    }
  }

  return {};
}

[[nodiscard]] inline auto capture_pending_inputs(
    runtime_state::pending_inputs &pending_inputs,
    const std::span<const std::string> node_keys,
    const std::span<const compiled_node *const> nodes_by_id)
    -> wh::core::result<checkpoint_pending_inputs> {
  checkpoint_pending_inputs captured{};
  captured.nodes.reserve(pending_inputs.active_input_count());
  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_keys.size()); ++node_id) {
    auto *payload = pending_inputs.find_input(node_id);
    if (payload == nullptr) {
      continue;
    }
    const auto *node = node_id < nodes_by_id.size() ? nodes_by_id[node_id] : nullptr;
    if (node == nullptr) {
      return wh::core::result<checkpoint_pending_inputs>::failure(
          wh::core::errc::not_found);
    }
    auto forked = node->meta.input_contract == node_contract::stream
                      ? detail::fork_graph_reader_payload(*payload)
                      : fork_graph_value(*payload);
    if (forked.has_error()) {
      return wh::core::result<checkpoint_pending_inputs>::failure(
          forked.error());
    }
    if (node_keys[node_id] == graph_start_node_key) {
      captured.entry = std::move(forked).value();
      continue;
    }
    captured.nodes.push_back(checkpoint_node_input{
        .node_id = node_id,
        .key = node_keys[node_id],
        .input = std::move(forked).value(),
    });
  }
  return captured;
}

inline auto restore_node_states(const checkpoint_state &checkpoint,
                                graph_state_table &state_table)
    -> wh::core::result<void> {
  for (const auto &lifecycle : checkpoint.runtime.lifecycle) {
    auto updated = state_table.update(lifecycle.node_id, lifecycle.lifecycle,
                                      lifecycle.attempts,
                                      lifecycle.last_error);
    if (updated.has_error()) {
      continue;
    }
  }
  return {};
}

inline auto restore_pending_inputs(checkpoint_pending_inputs &&snapshot,
                                   runtime_state::pending_inputs &pending_inputs,
                                   const std::uint32_t start_node_id,
                                   const std::size_t node_count)
    -> wh::core::result<void> {
  if (snapshot.entry.has_value()) {
    if (start_node_id >= node_count) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    pending_inputs.store_input(start_node_id, std::move(*snapshot.entry));
    pending_inputs.mark_restored_input(start_node_id);
    pending_inputs.mark_restored_node(start_node_id);
  }
  for (auto &node_input : snapshot.nodes) {
    if (node_input.node_id >= node_count) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    pending_inputs.store_input(node_input.node_id, std::move(node_input.input));
    pending_inputs.mark_restored_input(node_input.node_id);
    pending_inputs.mark_restored_node(node_input.node_id);
  }
  return {};
}

[[nodiscard]] inline auto
validate_runtime_configuration(const runtime_state::invoke_config &config,
                               runtime_state::invoke_outputs &outputs)
    -> wh::core::result<void> {
  const bool has_runtime_backend =
      config.checkpoint_store_ptr != nullptr ||
      config.checkpoint_backend_ptr != nullptr;
  std::string explicit_checkpoint_id{};
  if (config.checkpoint_store_ptr != nullptr &&
      config.checkpoint_backend_ptr != nullptr) {
    set_error_detail(outputs, wh::core::errc::invalid_argument, "",
                     "validate_runtime_config");
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }
  if (config.checkpoint_backend_ptr != nullptr &&
      (!config.checkpoint_backend_ptr->prepare_restore ||
       !config.checkpoint_backend_ptr->save)) {
    set_error_detail(outputs, wh::core::errc::invalid_argument, "",
                     "validate_runtime_config");
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }
  if (config.checkpoint_serializer_ptr != nullptr &&
      (!config.checkpoint_serializer_ptr->encode ||
       !config.checkpoint_serializer_ptr->decode)) {
    set_error_detail(outputs, wh::core::errc::invalid_argument, "",
                     "validate_runtime_config");
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }

  bool has_explicit_checkpoint_id = false;
  if (config.checkpoint_load.has_value()) {
    has_explicit_checkpoint_id =
        has_explicit_checkpoint_id || config.checkpoint_load->checkpoint_id.has_value();
    if (explicit_checkpoint_id.empty()) {
      explicit_checkpoint_id = resolve_id_hint(*config.checkpoint_load);
    }
  }
  if (config.checkpoint_save.has_value()) {
    has_explicit_checkpoint_id =
        has_explicit_checkpoint_id || config.checkpoint_save->checkpoint_id.has_value();
    if (explicit_checkpoint_id.empty()) {
      explicit_checkpoint_id = resolve_id_hint(*config.checkpoint_save);
    }
  }

  if (has_explicit_checkpoint_id && !has_runtime_backend) {
    set_error_detail(outputs, wh::core::errc::contract_violation,
                     explicit_checkpoint_id, "validate_runtime_config");
    return wh::core::result<void>::failure(
        wh::core::errc::contract_violation);
  }
  return {};
}

inline auto apply_modifier(wh::core::run_context &context,
                           const checkpoint_state_modifier &modifier,
                           const checkpoint_node_hooks *node_hooks,
                           checkpoint_state &state) -> wh::core::result<void> {
  if (modifier) {
    auto status = modifier(state, context);
    if (status.has_error()) {
      return wh::core::result<void>::failure(status.error());
    }
  }
  return apply_node_hooks(context, node_hooks, state);
}

inline auto prepare_restore(
    wh::core::run_context &context, const restore_scope scope,
    const std::string_view graph_name, const node_path &runtime_path,
    const graph_restore_shape &current_restore_shape,
    const std::span<const std::string> node_keys,
    const std::span<const detail::graph_core::indexed_edge> indexed_edges,
    const std::uint32_t end_id, const runtime_state::invoke_config &config,
    runtime_state::invoke_outputs &outputs,
    forwarded_checkpoint_map &forwarded_checkpoints)
    -> wh::core::result<std::optional<prepared_restore>> {
  checkpoint_load_options load_options =
      config.checkpoint_load.value_or(checkpoint_load_options{});
  if (load_options.force_new_run) {
    return std::optional<prepared_restore>{};
  }

  auto checkpoint_id_hint = resolve_id_hint(load_options);
  if (checkpoint_id_hint.empty()) {
    checkpoint_id_hint = std::string{graph_name};
  }
  const auto forwarded_restore_key =
      resolve_forwarded_restore_key(graph_name, runtime_path);

  std::optional<checkpoint_state> checkpoint_value{};
  const auto forwarded_iter =
      forwarded_checkpoints.find(forwarded_restore_key);
  if (forwarded_iter != forwarded_checkpoints.end()) {
    checkpoint_value.emplace(std::move(forwarded_iter->second));
    forwarded_checkpoints.erase(forwarded_iter);
  } else {
    if (scope == restore_scope::forwarded_only) {
      return std::optional<prepared_restore>{};
    }

    auto runtime_backend = resolve_runtime_backend(config);
    if (runtime_backend.has_error()) {
      set_error_detail(outputs, runtime_backend.error(), checkpoint_id_hint,
                       "restore_store_lookup");
      return wh::core::result<std::optional<prepared_restore>>::failure(
          runtime_backend.error());
    }
    const bool has_runtime_backend = runtime_backend.value().store != nullptr ||
                                     runtime_backend.value().backend != nullptr;
    if (!has_runtime_backend) {
      return std::optional<prepared_restore>{};
    }

    wh::core::result<checkpoint_restore_plan> restore =
        wh::core::result<checkpoint_restore_plan>::failure(
            wh::core::errc::invalid_argument);
    if (runtime_backend.value().backend != nullptr) {
      restore =
          runtime_backend.value().backend->prepare_restore(load_options, context);
    } else {
      restore = runtime_backend.value().store->prepare_restore(load_options);
    }
    if (restore.has_error()) {
      if (restore.error() == wh::core::errc::not_found) {
        return std::optional<prepared_restore>{};
      }
      set_error_detail(outputs, restore.error(), checkpoint_id_hint,
                       "prepare_restore");
      return wh::core::result<std::optional<prepared_restore>>::failure(
          restore.error());
    }

    auto plan = std::move(restore).value();
    if (!plan.restore_from_checkpoint || !plan.checkpoint.has_value()) {
      return std::optional<prepared_restore>{};
    }
    checkpoint_value.emplace(std::move(plan.checkpoint).value());
  }

  auto serializer_roundtrip =
      roundtrip_with_serializer(std::move(checkpoint_value).value(), context,
                                config);
  if (serializer_roundtrip.has_error()) {
    set_error_detail(outputs, serializer_roundtrip.error(), checkpoint_id_hint,
                     "restore_serializer_roundtrip");
    return wh::core::result<std::optional<prepared_restore>>::failure(
        serializer_roundtrip.error());
  }
  auto checkpoint = std::move(serializer_roundtrip).value();

  auto pre_load = apply_modifier(context, config.checkpoint_before_load,
                                 std::addressof(config.checkpoint_before_load_nodes),
                                 checkpoint);
  if (pre_load.has_error()) {
    set_error_detail(outputs, pre_load.error(), checkpoint_id_hint,
                     "pre_load_modifier");
    return wh::core::result<std::optional<prepared_restore>>::failure(
        pre_load.error());
  }

  auto validation = restore_check::validate(current_restore_shape, checkpoint);
  if (!validation.restorable) {
    set_error_detail(outputs, wh::core::errc::contract_violation,
                     checkpoint_id_hint, "validate_restore");
    return wh::core::result<std::optional<prepared_restore>>::failure(
        wh::core::errc::contract_violation);
  }

  auto stream_restored = apply_stream_codecs_for_load(
      checkpoint, context, config.checkpoint_stream_codecs_ptr, node_keys,
      indexed_edges, end_id);
  if (stream_restored.has_error()) {
    set_error_detail(outputs, stream_restored.error(), checkpoint_id_hint,
                     "restore_stream_convert");
    return wh::core::result<std::optional<prepared_restore>>::failure(
        stream_restored.error());
  }

  auto post_load = apply_modifier(context, config.checkpoint_after_load,
                                  std::addressof(config.checkpoint_after_load_nodes),
                                  checkpoint);
  if (post_load.has_error()) {
    set_error_detail(outputs, post_load.error(), checkpoint_id_hint,
                     "post_load_modifier");
    return wh::core::result<std::optional<prepared_restore>>::failure(
        post_load.error());
  }

  if (!context.resume_info.has_value()) {
    context.resume_info = checkpoint.resume_snapshot;
  } else {
    auto merged = context.resume_info->merge(checkpoint.resume_snapshot);
    if (merged.has_error()) {
      set_error_detail(outputs, merged.error(), checkpoint_id_hint,
                       "merge_resume_snapshot");
      return wh::core::result<std::optional<prepared_restore>>::failure(
          merged.error());
    }
  }

  if (context.interrupt_info == std::nullopt &&
      !checkpoint.interrupt_snapshot.interrupt_id_to_address.empty()) {
    const auto first =
        checkpoint.interrupt_snapshot.interrupt_id_to_address.begin();
    wh::core::interrupt_context restored_interrupt{};
    restored_interrupt.interrupt_id = first->first;
    restored_interrupt.location = first->second;
    const auto state_iter =
        checkpoint.interrupt_snapshot.interrupt_id_to_state.find(first->first);
    if (state_iter != checkpoint.interrupt_snapshot.interrupt_id_to_state.end()) {
      restored_interrupt.state = state_iter->second;
    }
    context.interrupt_info = std::move(restored_interrupt);
  }

  return std::optional<prepared_restore>{prepared_restore{
      .checkpoint = std::move(checkpoint),
      .checkpoint_id_hint = std::move(checkpoint_id_hint),
      .restore_skip_pre_handlers = load_options.skip_pre_handlers,
  }};
}

inline auto maybe_persist(
    wh::core::run_context &context, checkpoint_state checkpoint,
    const std::span<const std::string> node_keys,
    const std::span<const detail::graph_core::indexed_edge> indexed_edges,
    const std::uint32_t end_id, const runtime_state::invoke_config &config,
    runtime_state::invoke_outputs &outputs) -> wh::core::result<void> {
  auto checkpoint_id_hint = checkpoint.checkpoint_id;
  if (checkpoint_id_hint.empty()) {
    checkpoint_id_hint = "checkpoint";
  }

  auto runtime_backend = resolve_runtime_backend(config);
  if (runtime_backend.has_error()) {
    set_error_detail(outputs, runtime_backend.error(), checkpoint_id_hint,
                     "persist_store_lookup");
    return wh::core::result<void>::failure(runtime_backend.error());
  }
  if (runtime_backend.value().store == nullptr &&
      runtime_backend.value().backend == nullptr) {
    return {};
  }

  auto stream_persisted = apply_stream_codecs_for_save(
      checkpoint, context, config.checkpoint_stream_codecs_ptr, node_keys,
      indexed_edges, end_id);
  if (stream_persisted.has_error()) {
    set_error_detail(outputs, stream_persisted.error(), checkpoint_id_hint,
                     "persist_stream_convert");
    return stream_persisted;
  }

  auto pre_save = apply_modifier(context, config.checkpoint_before_save,
                                 std::addressof(config.checkpoint_before_save_nodes),
                                 checkpoint);
  if (pre_save.has_error()) {
    set_error_detail(outputs, pre_save.error(), checkpoint_id_hint,
                     "pre_save_modifier");
    return pre_save;
  }

  auto owned_checkpoint = wh::core::into_owned(std::move(checkpoint));
  if (owned_checkpoint.has_error()) {
    set_error_detail(outputs, owned_checkpoint.error(), checkpoint_id_hint,
                     "persist_snapshot_own");
    return wh::core::result<void>::failure(owned_checkpoint.error());
  }
  auto serializer_roundtrip =
      roundtrip_with_serializer(std::move(owned_checkpoint).value(), context,
                                config);
  if (serializer_roundtrip.has_error()) {
    set_error_detail(outputs, serializer_roundtrip.error(), checkpoint_id_hint,
                     "persist_serializer_roundtrip");
    return wh::core::result<void>::failure(serializer_roundtrip.error());
  }
  checkpoint = std::move(serializer_roundtrip).value();

  checkpoint_save_options write_options =
      config.checkpoint_save.value_or(checkpoint_save_options{});
  if (!write_options.checkpoint_id.has_value()) {
    write_options.checkpoint_id = checkpoint.checkpoint_id;
  }
  checkpoint.checkpoint_id = *write_options.checkpoint_id;
  checkpoint.branch = write_options.branch;
  checkpoint.parent_branch = write_options.parent_branch;

  auto post_save_state = checkpoint;

  if (runtime_backend.value().backend != nullptr) {
    auto saved = runtime_backend.value().backend->save(
        std::move(checkpoint), std::move(write_options), context);
    if (saved.has_error()) {
      set_error_detail(outputs, saved.error(), checkpoint_id_hint, "save");
      return wh::core::result<void>::failure(saved.error());
    }
  } else {
    auto saved = runtime_backend.value().store->save(std::move(checkpoint),
                                                     std::move(write_options));
    if (saved.has_error()) {
      set_error_detail(outputs, saved.error(), checkpoint_id_hint, "save");
      return wh::core::result<void>::failure(saved.error());
    }
  }

  auto post_save = apply_modifier(context, config.checkpoint_after_save,
                                  std::addressof(config.checkpoint_after_save_nodes),
                                  post_save_state);
  if (post_save.has_error()) {
    set_error_detail(outputs, post_save.error(), checkpoint_id_hint,
                     "post_save_modifier");
    return post_save;
  }
  return {};
}

} // namespace wh::compose::detail::checkpoint_runtime
