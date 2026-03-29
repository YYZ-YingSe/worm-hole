// Defines checkpoint runtime helpers extracted from graph execution core.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include "wh/compose/graph/restore_check.hpp"
#include "wh/compose/graph/detail/keys.hpp"
#include "wh/compose/graph/detail/runtime/rerun.hpp"
#include "wh/compose/graph/detail/runtime/state.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/runtime/checkpoint.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/node/path.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose::detail::checkpoint_runtime {

enum class restore_scope : std::uint8_t {
  full = 0U,
  forwarded_only,
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

[[nodiscard]] inline auto
default_serializer() -> const checkpoint_serializer & {
  static const checkpoint_serializer serializer{
      .encode =
          checkpoint_serializer_encode{
              [](checkpoint_state &&state, wh::core::run_context &)
                  -> wh::core::result<graph_value> {
                return wh::core::any(std::move(state));
              }},
      .decode =
          checkpoint_serializer_decode{
              [](graph_value &&payload, wh::core::run_context &)
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
  if (config.checkpoint_serializer == nullptr) {
    return std::addressof(default_serializer());
  }
  const auto *serializer = config.checkpoint_serializer;
  if (!serializer->encode || !serializer->decode) {
    return wh::core::result<const checkpoint_serializer *>::failure(
        wh::core::errc::invalid_argument);
  }
  return serializer;
}

[[nodiscard]] inline auto roundtrip_with_serializer(
    checkpoint_state &&checkpoint, wh::core::run_context &context,
    const runtime_state::invoke_config &config)
    -> wh::core::result<checkpoint_state> {
  auto serializer_ref = resolve_serializer(config);
  if (serializer_ref.has_error()) {
    return wh::core::result<checkpoint_state>::failure(
        serializer_ref.error());
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

[[nodiscard]] inline auto roundtrip_with_serializer(
    const checkpoint_state &checkpoint, wh::core::run_context &context,
    const runtime_state::invoke_config &config)
    -> wh::core::result<checkpoint_state> {
  return roundtrip_with_serializer(checkpoint_state{checkpoint}, context,
                                   config);
}

struct runtime_backend {
  checkpoint_store *store{nullptr};
  checkpoint_backend *backend{nullptr};
};

[[nodiscard]] inline auto
resolve_runtime_backend(const runtime_state::invoke_config &config)
    -> wh::core::result<runtime_backend> {
  runtime_backend resolved{};
  resolved.store = config.checkpoint_store;
  resolved.backend = config.checkpoint_backend;
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
                                      checkpoint_state &state)
    -> wh::core::result<void> {
  if (modifiers == nullptr || modifiers->empty()) {
    return {};
  }
  for (auto &node_state : state.node_states) {
    const auto node_state_path = make_node_path_from_state_key(node_state.key);
    for (const auto &path_modifier : *modifiers) {
      if (!path_modifier.modifier || path_modifier.path.empty()) {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
      const auto matched =
          path_modifier.include_descendants
              ? node_state_path.starts_with(path_modifier.path)
              : node_state_path == path_modifier.path;
      if (!matched) {
        continue;
      }
      auto status = path_modifier.modifier(node_state, context);
      if (status.has_error()) {
        return wh::core::result<void>::failure(status.error());
      }
    }
  }
  return {};
}

inline auto apply_stream_codecs_for_save(
    checkpoint_state &checkpoint, wh::core::run_context &context,
    const checkpoint_stream_codecs *registry)
    -> wh::core::result<void> {
  const bool has_registry = registry != nullptr;

  const auto convert_one = [&](const std::string_view node_key, graph_value &payload,
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
    if (converter_iter == registry->end() ||
        !converter_iter->second.to_value) {
      return wh::core::result<void>::failure(wh::core::errc::not_supported);
    }
    auto converted = converter_iter->second.to_value(std::move(*reader), context);
    if (converted.has_error()) {
      if (tolerate_channel_closed &&
          converted.error() == wh::core::errc::channel_closed) {
        return {};
      }
      return wh::core::result<void>::failure(converted.error());
    }
    payload = wh::core::any(checkpoint_stream_value_payload{
        .value = std::move(converted).value(),
    });
    return {};
  };

  auto start_iter = checkpoint.rerun_inputs.find(graph_start_node_key);
  if (start_iter != checkpoint.rerun_inputs.end()) {
    auto converted_start =
        convert_one(graph_start_node_key, start_iter->second, false);
    if (converted_start.has_error()) {
      return converted_start;
    }
  }

  for (auto &[node_key, payload] : checkpoint.rerun_inputs) {
    if (node_key == graph_start_node_key) {
      continue;
    }
    auto converted = convert_one(node_key, payload, true);
    if (converted.has_error()) {
      return converted;
    }
  }
  return {};
}

inline auto apply_stream_codecs_for_load(
    checkpoint_state &checkpoint, wh::core::run_context &context,
    const checkpoint_stream_codecs *registry)
    -> wh::core::result<void> {
  const bool has_registry = registry != nullptr;

  for (auto &[node_key, payload] : checkpoint.rerun_inputs) {
    auto *stored =
        wh::core::any_cast<checkpoint_stream_value_payload>(&payload);
    if (stored == nullptr) {
      continue;
    }
    if (!has_registry) {
      return wh::core::result<void>::failure(wh::core::errc::not_supported);
    }
    const auto converter_iter = registry->find(node_key);
    if (converter_iter == registry->end() ||
        !converter_iter->second.to_stream) {
      return wh::core::result<void>::failure(wh::core::errc::not_supported);
    }
    auto restored =
        converter_iter->second.to_stream(std::move(stored->value), context);
    if (restored.has_error()) {
      return wh::core::result<void>::failure(restored.error());
    }
    payload = wh::core::any(std::move(restored).value());
  }
  return {};
}

template <typename resolve_node_id_fn_t>
[[nodiscard]] inline auto load_rerun_inputs(
    checkpoint_state &checkpoint, runtime_state::rerun_state &rerun_state,
    resolve_node_id_fn_t &&resolve_node_id, const std::uint32_t start_id)
    -> wh::core::result<void> {
  const auto &resolver = resolve_node_id;
  for (auto &[node_key, payload] : checkpoint.rerun_inputs) {
    if (node_key == graph_start_node_key) {
      continue;
    }
    auto node_id = resolver(node_key);
    if (!node_id.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (*node_id == start_id) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    rerun_state.store(*node_id, std::move(payload));
    rerun_state.mark_restored(*node_id);
  }
  return {};
}

[[nodiscard]] inline auto save_rerun_inputs(
    runtime_state::rerun_state &rerun_state,
    const std::span<const std::string> node_keys) -> wh::core::result<graph_value_map> {
  graph_value_map cloned{};
  cloned.reserve(rerun_state.active_count());
  for (std::uint32_t node_id = 0U;
       node_id < static_cast<std::uint32_t>(node_keys.size()); ++node_id) {
    auto *payload = rerun_state.find(node_id);
    if (payload == nullptr) {
      continue;
    }
    auto forked = fork_graph_value(*payload);
    if (forked.has_error()) {
      return wh::core::result<graph_value_map>::failure(forked.error());
    }
    cloned.insert_or_assign(node_keys[node_id], std::move(forked).value());
  }
  return cloned;
}

inline auto restore_node_states(const checkpoint_state &checkpoint,
                                graph_state_table &state_table)
    -> wh::core::result<void> {
  for (const auto &node_state : checkpoint.node_states) {
    auto updated = state_table.update(node_state.node_id, node_state.lifecycle,
                                      node_state.attempts, node_state.last_error);
    if (updated.has_error()) {
      continue;
    }
  }
  return {};
}

[[nodiscard]] inline auto validate_runtime_configuration(
    const runtime_state::invoke_config &config,
    runtime_state::invoke_outputs &outputs) -> wh::core::result<void> {
  const bool has_runtime_backend =
      config.checkpoint_store != nullptr || config.checkpoint_backend != nullptr;
  std::string explicit_checkpoint_id{};
  if (config.checkpoint_store != nullptr && config.checkpoint_backend != nullptr) {
    set_error_detail(outputs, wh::core::errc::invalid_argument, "",
                     "validate_runtime_config");
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }
  if (config.checkpoint_backend != nullptr &&
      (!config.checkpoint_backend->prepare_restore ||
       !config.checkpoint_backend->save)) {
    set_error_detail(outputs, wh::core::errc::invalid_argument, "",
                     "validate_runtime_config");
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }
  if (config.checkpoint_serializer != nullptr &&
      (!config.checkpoint_serializer->encode ||
       !config.checkpoint_serializer->decode)) {
    set_error_detail(outputs, wh::core::errc::invalid_argument, "",
                     "validate_runtime_config");
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }

  bool has_explicit_checkpoint_id = false;
  if (config.checkpoint_load.has_value()) {
    has_explicit_checkpoint_id =
        has_explicit_checkpoint_id ||
        config.checkpoint_load->checkpoint_id.has_value();
    if (explicit_checkpoint_id.empty()) {
      explicit_checkpoint_id = resolve_id_hint(*config.checkpoint_load);
    }
  }
  if (config.checkpoint_save.has_value()) {
    has_explicit_checkpoint_id =
        has_explicit_checkpoint_id ||
        config.checkpoint_save->checkpoint_id.has_value();
    if (explicit_checkpoint_id.empty()) {
      explicit_checkpoint_id = resolve_id_hint(*config.checkpoint_save);
    }
  }

  if (has_explicit_checkpoint_id && !has_runtime_backend) {
    set_error_detail(outputs, wh::core::errc::contract_violation,
                     explicit_checkpoint_id, "validate_runtime_config");
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  return {};
}

inline auto apply_modifier(wh::core::run_context &context,
                           const checkpoint_state_modifier &modifier,
                           const checkpoint_node_hooks *node_hooks,
                           checkpoint_state &state)
    -> wh::core::result<void> {
  if (modifier) {
    auto status = modifier(state, context);
    if (status.has_error()) {
      return wh::core::result<void>::failure(status.error());
    }
  }
  return apply_node_hooks(context, node_hooks, state);
}

template <typename resolve_node_id_fn_t, typename missing_input_fn_t>
inline auto maybe_restore(graph_value &input, wh::core::run_context &context,
                          graph_state_table &state_table,
                          runtime_state::rerun_state &rerun_state,
                          bool &skip_state_pre_handlers,
                          const restore_scope scope,
                          const std::string_view graph_name,
                          const node_path &runtime_path,
                          const graph_restore_shape &current_restore_shape,
                          const std::uint32_t start_id,
                          const runtime_state::invoke_config &config,
                          runtime_state::invoke_outputs &outputs,
                          forwarded_checkpoint_map &forwarded_checkpoints,
                          resolve_node_id_fn_t &&resolve_node_id,
                          missing_input_fn_t &&resolve_missing_rerun_input)
    -> wh::core::result<void> {
  skip_state_pre_handlers = false;
  const auto resolver = std::forward<resolve_node_id_fn_t>(resolve_node_id);
  checkpoint_load_options load_options =
      config.checkpoint_load.value_or(checkpoint_load_options{});
  if (load_options.force_new_run) {
    return {};
  }
  auto checkpoint_id_hint = resolve_id_hint(load_options);
  if (checkpoint_id_hint.empty()) {
    checkpoint_id_hint = std::string{graph_name};
  }
  const auto forwarded_restore_key =
      resolve_forwarded_restore_key(graph_name, runtime_path);

  std::optional<checkpoint_state> forwarded_checkpoint{};
  const auto forwarded_iter = forwarded_checkpoints.find(forwarded_restore_key);
  if (forwarded_iter != forwarded_checkpoints.end()) {
    forwarded_checkpoint.emplace(std::move(forwarded_iter->second));
    forwarded_checkpoints.erase(forwarded_iter);
  }

  std::optional<checkpoint_state> checkpoint_value{};
  if (forwarded_checkpoint.has_value()) {
    checkpoint_value.emplace(std::move(forwarded_checkpoint).value());
  } else {
    if (scope == restore_scope::forwarded_only) {
      return {};
    }

    auto runtime_backend = resolve_runtime_backend(config);
    if (runtime_backend.has_error()) {
      set_error_detail(outputs, runtime_backend.error(), checkpoint_id_hint,
                       "restore_store_lookup");
      return wh::core::result<void>::failure(runtime_backend.error());
    }
    const bool has_runtime_backend =
        runtime_backend.value().store != nullptr ||
        runtime_backend.value().backend != nullptr;
    if (!has_runtime_backend) {
      return {};
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
        return {};
      }
      set_error_detail(outputs, restore.error(), checkpoint_id_hint,
                       "prepare_restore");
      return wh::core::result<void>::failure(restore.error());
    }
    auto plan = std::move(restore).value();
    if (!plan.restore_from_checkpoint || !plan.checkpoint.has_value()) {
      return {};
    }
    checkpoint_value.emplace(std::move(plan.checkpoint).value());
  }

  auto serializer_roundtrip =
      roundtrip_with_serializer(std::move(checkpoint_value).value(), context,
                                config);
  if (serializer_roundtrip.has_error()) {
    set_error_detail(outputs, serializer_roundtrip.error(), checkpoint_id_hint,
                     "restore_serializer_roundtrip");
    return wh::core::result<void>::failure(serializer_roundtrip.error());
  }
  auto checkpoint = std::move(serializer_roundtrip).value();
  auto pre_load = apply_modifier(
      context, config.checkpoint_before_load,
      std::addressof(config.checkpoint_before_load_nodes), checkpoint);
  if (pre_load.has_error()) {
    set_error_detail(outputs, pre_load.error(), checkpoint_id_hint,
                     "pre_load_modifier");
    return pre_load;
  }
  auto validation = restore_check::validate(current_restore_shape, checkpoint);
  if (!validation.restorable) {
    set_error_detail(outputs, wh::core::errc::contract_violation,
                     checkpoint_id_hint, "validate_restore");
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  skip_state_pre_handlers = load_options.skip_pre_handlers;

  auto stream_restored = apply_stream_codecs_for_load(
      checkpoint, context, config.checkpoint_stream_codecs);
  if (stream_restored.has_error()) {
    set_error_detail(outputs, stream_restored.error(), checkpoint_id_hint,
                     "restore_stream_convert");
    return stream_restored;
  }

  auto rerun_restored =
      load_rerun_inputs(checkpoint, rerun_state, resolver, start_id);
  if (rerun_restored.has_error()) {
    set_error_detail(outputs, rerun_restored.error(), checkpoint_id_hint,
                     "restore_rerun_inputs");
    return rerun_restored;
  }

  if (!context.resume_info.has_value()) {
    context.resume_info = checkpoint.resume_snapshot;
  } else {
    auto merged = context.resume_info->merge(checkpoint.resume_snapshot);
    if (merged.has_error()) {
      set_error_detail(outputs, merged.error(), checkpoint_id_hint,
                       "merge_resume_snapshot");
      return wh::core::result<void>::failure(merged.error());
    }
  }

  auto restored_states = restore_node_states(checkpoint, state_table);
  if (restored_states.has_error()) {
    set_error_detail(outputs, restored_states.error(), checkpoint_id_hint,
                     "restore_node_states");
    return restored_states;
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

  const auto *empty_marker = wh::core::any_cast<std::monostate>(&input);
  if (empty_marker != nullptr) {
    const auto rerun_iter = checkpoint.rerun_inputs.find(graph_start_node_key);
    if (rerun_iter != checkpoint.rerun_inputs.end()) {
      input = std::move(rerun_iter->second);
    } else {
      auto fallback = resolve_missing_rerun_input();
      if (fallback.has_error()) {
        set_error_detail(outputs, fallback.error(), checkpoint_id_hint,
                         "resolve_missing_rerun_input");
        return wh::core::result<void>::failure(fallback.error());
      }
      input = std::move(fallback).value();
    }
  }

  auto post_load = apply_modifier(
      context, config.checkpoint_after_load,
      std::addressof(config.checkpoint_after_load_nodes), checkpoint);
  if (post_load.has_error()) {
    set_error_detail(outputs, post_load.error(), checkpoint_id_hint,
                     "post_load_modifier");
    return post_load;
  }
  return {};
}

inline auto maybe_persist(wh::core::run_context &context,
                          const graph_state_table &state_table,
                          runtime_state::rerun_state &rerun_state,
                          const std::span<const std::string> node_keys,
                          const std::string_view graph_name,
                          const graph_restore_shape &current_restore_shape,
                          const runtime_state::invoke_config &config,
                          runtime_state::invoke_outputs &outputs)
    -> wh::core::result<void> {
  auto runtime_backend = resolve_runtime_backend(config);
  if (runtime_backend.has_error()) {
    set_error_detail(outputs, runtime_backend.error(), graph_name,
                     "persist_store_lookup");
    return wh::core::result<void>::failure(runtime_backend.error());
  }
  if (runtime_backend.value().store == nullptr &&
      runtime_backend.value().backend == nullptr) {
    return {};
  }

  checkpoint_state checkpoint{};
  checkpoint.checkpoint_id = std::string{graph_name};
  checkpoint.restore_shape = current_restore_shape;
  checkpoint.node_states = state_table.states();
  checkpoint.resume_snapshot =
      context.resume_info.value_or(wh::core::resume_state{});
  auto cloned_rerun_inputs = save_rerun_inputs(rerun_state, node_keys);
  if (cloned_rerun_inputs.has_error()) {
    set_error_detail(outputs, cloned_rerun_inputs.error(), graph_name,
                     "persist_rerun_clone");
    return wh::core::result<void>::failure(cloned_rerun_inputs.error());
  }
  checkpoint.rerun_inputs = std::move(cloned_rerun_inputs).value();
  if (context.interrupt_info.has_value()) {
    checkpoint.interrupt_snapshot = wh::core::flatten_interrupt_signals(
        std::vector<wh::core::interrupt_signal>{
            wh::compose::to_reinterrupt_signal(*context.interrupt_info)});
  }
  auto stream_persisted = apply_stream_codecs_for_save(
      checkpoint, context, config.checkpoint_stream_codecs);
  if (stream_persisted.has_error()) {
    set_error_detail(outputs, stream_persisted.error(), graph_name,
                     "persist_stream_convert");
    return stream_persisted;
  }

  auto pre_save =
      apply_modifier(context, config.checkpoint_before_save,
                     std::addressof(config.checkpoint_before_save_nodes),
                     checkpoint);
  if (pre_save.has_error()) {
    set_error_detail(outputs, pre_save.error(), graph_name, "pre_save_modifier");
    return pre_save;
  }
  auto serializer_roundtrip =
      roundtrip_with_serializer(std::move(checkpoint), context, config);
  if (serializer_roundtrip.has_error()) {
    set_error_detail(outputs, serializer_roundtrip.error(), graph_name,
                     "persist_serializer_roundtrip");
    return wh::core::result<void>::failure(serializer_roundtrip.error());
  }
  checkpoint = std::move(serializer_roundtrip).value();

  checkpoint_save_options write_options =
      config.checkpoint_save.value_or(checkpoint_save_options{});
  auto checkpoint_id_hint = resolve_id_hint(write_options);
  if (checkpoint_id_hint.empty()) {
    checkpoint_id_hint = std::string{graph_name};
  }
  if (!write_options.checkpoint_id.has_value()) {
    write_options.checkpoint_id = std::string{graph_name};
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

  auto post_save = apply_modifier(
      context, config.checkpoint_after_save,
      std::addressof(config.checkpoint_after_save_nodes), post_save_state);
  if (post_save.has_error()) {
    set_error_detail(outputs, post_save.error(), checkpoint_id_hint,
                     "post_save_modifier");
    return post_save;
  }
  return {};
}

} // namespace wh::compose::detail::checkpoint_runtime
