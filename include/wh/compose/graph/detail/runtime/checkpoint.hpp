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
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/runtime/checkpoint.hpp"
#include "wh/compose/runtime/interrupt.hpp"
#include "wh/compose/runtime/state.hpp"
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

inline auto set_error_detail(wh::core::run_context &context,
                             const wh::core::error_code code,
                             const std::string_view checkpoint_id,
                             const std::string_view operation) -> void {
  if (context.session_values.contains(
          std::string{checkpoint_last_error_session_key})) {
    return;
  }
  wh::core::set_session_value(
      context, std::string{checkpoint_last_error_session_key},
      checkpoint_error_detail{
          .code = code,
          .checkpoint_id = std::string{checkpoint_id},
          .operation = std::string{operation},
      });
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
resolve_serializer(const wh::core::run_context &context)
    -> wh::core::result<const checkpoint_serializer *> {
  const auto iter =
      context.session_values.find(checkpoint_serializer_session_key);
  if (iter == context.session_values.end()) {
    return std::addressof(default_serializer());
  }
  const auto *serializer =
      wh::core::any_cast<checkpoint_serializer>(&iter->second);
  if (serializer == nullptr) {
    return wh::core::result<const checkpoint_serializer *>::failure(
        wh::core::errc::type_mismatch);
  }
  if (!serializer->encode || !serializer->decode) {
    return wh::core::result<const checkpoint_serializer *>::failure(
        wh::core::errc::invalid_argument);
  }
  return serializer;
}

[[nodiscard]] inline auto roundtrip_with_serializer(
    checkpoint_state &&checkpoint, wh::core::run_context &context)
    -> wh::core::result<checkpoint_state> {
  auto serializer_ref = resolve_serializer(context);
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
    const checkpoint_state &checkpoint, wh::core::run_context &context)
    -> wh::core::result<checkpoint_state> {
  return roundtrip_with_serializer(checkpoint_state{checkpoint}, context);
}

struct runtime_backend {
  checkpoint_store *store{nullptr};
  checkpoint_backend *backend{nullptr};
};

[[nodiscard]] inline auto
resolve_runtime_backend(wh::core::run_context &context)
    -> wh::core::result<runtime_backend> {
  runtime_backend resolved{};
  auto store_ref = wh::core::session_value_ref<checkpoint_store *>(
      context, checkpoint_store_session_key);
  if (store_ref.has_error()) {
    if (store_ref.error() != wh::core::errc::not_found) {
      return wh::core::result<runtime_backend>::failure(store_ref.error());
    }
  } else {
    resolved.store = store_ref.value().get();
    if (resolved.store == nullptr) {
      return wh::core::result<runtime_backend>::failure(
          wh::core::errc::invalid_argument);
    }
  }

  auto backend_ref = wh::core::session_value_ref<checkpoint_backend *>(
      context, checkpoint_backend_session_key);
  if (backend_ref.has_error()) {
    if (backend_ref.error() != wh::core::errc::not_found) {
      return wh::core::result<runtime_backend>::failure(backend_ref.error());
    }
  } else {
    resolved.backend = backend_ref.value().get();
    if (resolved.backend == nullptr || !resolved.backend->prepare_restore ||
        !resolved.backend->save) {
      return wh::core::result<runtime_backend>::failure(
          wh::core::errc::invalid_argument);
    }
  }
  if (resolved.store != nullptr && resolved.backend != nullptr) {
    return wh::core::result<runtime_backend>::failure(
        wh::core::errc::invalid_argument);
  }
  return resolved;
}

[[nodiscard]] inline auto
resolve_node_hook_key(const std::string_view modifier_key)
    -> std::optional<std::string_view> {
  if (modifier_key == checkpoint_before_load_session_key) {
    return checkpoint_before_load_nodes_session_key;
  }
  if (modifier_key == checkpoint_after_load_session_key) {
    return checkpoint_after_load_nodes_session_key;
  }
  if (modifier_key == checkpoint_before_save_session_key) {
    return checkpoint_before_save_nodes_session_key;
  }
  if (modifier_key == checkpoint_after_save_session_key) {
    return checkpoint_after_save_nodes_session_key;
  }
  return std::nullopt;
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
                                      const std::string_view modifier_key,
                                      checkpoint_state &state)
    -> wh::core::result<void> {
  const auto path_modifier_key = resolve_node_hook_key(modifier_key);
  if (!path_modifier_key.has_value()) {
    return {};
  }
  const auto iter = context.session_values.find(*path_modifier_key);
  if (iter == context.session_values.end()) {
    return {};
  }
  const auto *modifiers =
      wh::core::any_cast<checkpoint_node_hooks>(&iter->second);
  if (modifiers == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
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
    checkpoint_state &checkpoint, wh::core::run_context &context)
    -> wh::core::result<void> {
  auto registry_ref = wh::core::session_value_ref<checkpoint_stream_codecs>(
      context, checkpoint_stream_codecs_session_key);
  const bool has_registry = registry_ref.has_value();
  if (registry_ref.has_error() &&
      registry_ref.error() != wh::core::errc::not_found) {
    return wh::core::result<void>::failure(registry_ref.error());
  }

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
    const auto converter_iter = registry_ref.value().get().find(node_key);
    if (converter_iter == registry_ref.value().get().end() ||
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
    checkpoint_state &checkpoint, wh::core::run_context &context)
    -> wh::core::result<void> {
  auto registry_ref = wh::core::session_value_ref<checkpoint_stream_codecs>(
      context, checkpoint_stream_codecs_session_key);
  const bool has_registry = registry_ref.has_value();
  if (registry_ref.has_error() &&
      registry_ref.error() != wh::core::errc::not_found) {
    return wh::core::result<void>::failure(registry_ref.error());
  }

  for (auto &[node_key, payload] : checkpoint.rerun_inputs) {
    auto *stored =
        wh::core::any_cast<checkpoint_stream_value_payload>(&payload);
    if (stored == nullptr) {
      continue;
    }
    if (!has_registry) {
      return wh::core::result<void>::failure(wh::core::errc::not_supported);
    }
    const auto converter_iter = registry_ref.value().get().find(node_key);
    if (converter_iter == registry_ref.value().get().end() ||
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
    wh::core::run_context &context) -> wh::core::result<void> {
  bool has_runtime_backend = false;
  std::string explicit_checkpoint_id{};
  const auto store_iter =
      context.session_values.find(checkpoint_store_session_key);
  if (store_iter != context.session_values.end()) {
    const auto *store =
        wh::core::any_cast<checkpoint_store *>(&store_iter->second);
    if (store == nullptr) {
      set_error_detail(context, wh::core::errc::type_mismatch, "",
                       "validate_runtime_config");
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    if (*store == nullptr) {
      set_error_detail(context, wh::core::errc::invalid_argument, "",
                       "validate_runtime_config");
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    has_runtime_backend = true;
  }
  const auto backend_iter =
      context.session_values.find(checkpoint_backend_session_key);
  if (backend_iter != context.session_values.end()) {
    const auto *backend =
        wh::core::any_cast<checkpoint_backend *>(&backend_iter->second);
    if (backend == nullptr) {
      set_error_detail(context, wh::core::errc::type_mismatch, "",
                       "validate_runtime_config");
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    if (*backend == nullptr || !(*backend)->prepare_restore || !(*backend)->save) {
      set_error_detail(context, wh::core::errc::invalid_argument, "",
                       "validate_runtime_config");
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    has_runtime_backend = true;
  }
  if (store_iter != context.session_values.end() &&
      backend_iter != context.session_values.end()) {
    set_error_detail(context, wh::core::errc::invalid_argument, "",
                     "validate_runtime_config");
    return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
  }

  bool has_explicit_checkpoint_id = false;
  const auto load_iter =
      context.session_values.find(checkpoint_load_session_key);
  if (load_iter != context.session_values.end()) {
    const auto *load_options =
        wh::core::any_cast<checkpoint_load_options>(&load_iter->second);
    if (load_options == nullptr) {
      set_error_detail(context, wh::core::errc::type_mismatch, "",
                       "validate_runtime_config");
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    has_explicit_checkpoint_id =
        has_explicit_checkpoint_id || load_options->checkpoint_id.has_value();
    if (explicit_checkpoint_id.empty()) {
      explicit_checkpoint_id = resolve_id_hint(*load_options);
    }
  }

  const auto write_iter =
      context.session_values.find(checkpoint_save_session_key);
  if (write_iter != context.session_values.end()) {
    const auto *write_options =
        wh::core::any_cast<checkpoint_save_options>(&write_iter->second);
    if (write_options == nullptr) {
      set_error_detail(context, wh::core::errc::type_mismatch, "",
                       "validate_runtime_config");
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    has_explicit_checkpoint_id =
        has_explicit_checkpoint_id || write_options->checkpoint_id.has_value();
    if (explicit_checkpoint_id.empty()) {
      explicit_checkpoint_id = resolve_id_hint(*write_options);
    }
  }

  if (has_explicit_checkpoint_id && !has_runtime_backend) {
    set_error_detail(context, wh::core::errc::contract_violation,
                     explicit_checkpoint_id, "validate_runtime_config");
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  return {};
}

inline auto apply_modifier(wh::core::run_context &context,
                           const std::string_view modifier_key,
                           checkpoint_state &state)
    -> wh::core::result<void> {
  const auto iter = context.session_values.find(modifier_key);
  if (iter != context.session_values.end()) {
    const auto *modifier =
        wh::core::any_cast<checkpoint_state_modifier>(&iter->second);
    if (modifier == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
    }
    auto status = (*modifier)(state, context);
    if (status.has_error()) {
      return wh::core::result<void>::failure(status.error());
    }
  }
  return apply_node_hooks(context, modifier_key, state);
}

[[nodiscard]] inline auto
resolve_target_version(const wh::core::run_context &context) -> std::uint32_t {
  auto target_ref = wh::core::session_value_ref<std::uint32_t>(
      context, checkpoint_version_session_key);
  if (target_ref.has_value() && target_ref.value().get() > 0U) {
    return target_ref.value().get();
  }
  return 1U;
}

[[nodiscard]] inline auto migrate_if_needed(checkpoint_state &&checkpoint,
                                            wh::core::run_context &context)
    -> wh::core::result<checkpoint_state> {
  const auto target_version = resolve_target_version(context);
  if (checkpoint.version == target_version) {
    return checkpoint;
  }
  auto registry_ref = wh::core::session_value_ref<checkpoint_migrator_registry *>(
      context, checkpoint_migrators_session_key);
  if (registry_ref.has_error()) {
    return wh::core::result<checkpoint_state>::failure(
        wh::core::errc::not_supported);
  }
  auto *registry = registry_ref.value().get();
  if (registry == nullptr) {
    return wh::core::result<checkpoint_state>::failure(
        wh::core::errc::invalid_argument);
  }
  return registry->migrate(std::move(checkpoint), target_version);
}

[[nodiscard]] inline auto migrate_if_needed(
    const checkpoint_state &checkpoint, wh::core::run_context &context)
    -> wh::core::result<checkpoint_state> {
  return migrate_if_needed(checkpoint_state{checkpoint}, context);
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
                          resolve_node_id_fn_t &&resolve_node_id,
                          missing_input_fn_t &&resolve_missing_rerun_input)
    -> wh::core::result<void> {
  skip_state_pre_handlers = false;
  const auto resolver = std::forward<resolve_node_id_fn_t>(resolve_node_id);
  checkpoint_load_options load_options{};
  auto load_options_ref = wh::core::session_value_ref<checkpoint_load_options>(
      context, checkpoint_load_session_key);
  if (load_options_ref.has_value()) {
    load_options = load_options_ref.value().get();
  }
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
  auto forwarded_ref = wh::core::session_value_ref<forwarded_checkpoint_map>(
      context, forwarded_checkpoints_session_key);
  if (!forwarded_ref.has_error()) {
    auto &forwarded = forwarded_ref.value().get();
    const auto iter = forwarded.find(forwarded_restore_key);
    if (iter != forwarded.end()) {
      forwarded_checkpoint.emplace(std::move(iter->second));
      forwarded.erase(iter);
    }
  }

  std::optional<checkpoint_state> checkpoint_value{};
  if (forwarded_checkpoint.has_value()) {
    checkpoint_value.emplace(std::move(forwarded_checkpoint).value());
  } else {
    if (scope == restore_scope::forwarded_only) {
      return {};
    }

    auto runtime_backend = resolve_runtime_backend(context);
    if (runtime_backend.has_error()) {
      set_error_detail(context, runtime_backend.error(), checkpoint_id_hint,
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
      set_error_detail(context, restore.error(), checkpoint_id_hint,
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
      roundtrip_with_serializer(std::move(checkpoint_value).value(), context);
  if (serializer_roundtrip.has_error()) {
    set_error_detail(context, serializer_roundtrip.error(), checkpoint_id_hint,
                     "restore_serializer_roundtrip");
    return wh::core::result<void>::failure(serializer_roundtrip.error());
  }
  auto migrated_checkpoint =
      migrate_if_needed(std::move(serializer_roundtrip).value(), context);
  if (migrated_checkpoint.has_error()) {
    set_error_detail(context, migrated_checkpoint.error(), checkpoint_id_hint,
                     "migrate");
    return wh::core::result<void>::failure(migrated_checkpoint.error());
  }
  auto checkpoint = std::move(migrated_checkpoint).value();
  auto pre_load = apply_modifier(
      context, checkpoint_before_load_session_key, checkpoint);
  if (pre_load.has_error()) {
    set_error_detail(context, pre_load.error(), checkpoint_id_hint,
                     "pre_load_modifier");
    return pre_load;
  }
  auto validation = restore_check::validate(current_restore_shape, checkpoint);
  if (!validation.restorable) {
    set_error_detail(context, wh::core::errc::contract_violation,
                     checkpoint_id_hint, "validate_restore");
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  skip_state_pre_handlers = load_options.skip_pre_handlers;

  auto stream_restored = apply_stream_codecs_for_load(checkpoint, context);
  if (stream_restored.has_error()) {
    set_error_detail(context, stream_restored.error(), checkpoint_id_hint,
                     "restore_stream_convert");
    return stream_restored;
  }

  auto rerun_restored =
      load_rerun_inputs(checkpoint, rerun_state, resolver, start_id);
  if (rerun_restored.has_error()) {
    set_error_detail(context, rerun_restored.error(), checkpoint_id_hint,
                     "restore_rerun_inputs");
    return rerun_restored;
  }

  if (!context.resume_info.has_value()) {
    context.resume_info = checkpoint.resume_snapshot;
  } else {
    auto merged = context.resume_info->merge(checkpoint.resume_snapshot);
    if (merged.has_error()) {
      set_error_detail(context, merged.error(), checkpoint_id_hint,
                       "merge_resume_snapshot");
      return wh::core::result<void>::failure(merged.error());
    }
  }

  auto restored_states = restore_node_states(checkpoint, state_table);
  if (restored_states.has_error()) {
    set_error_detail(context, restored_states.error(), checkpoint_id_hint,
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
        set_error_detail(context, fallback.error(), checkpoint_id_hint,
                         "resolve_missing_rerun_input");
        return wh::core::result<void>::failure(fallback.error());
      }
      input = std::move(fallback).value();
    }
  }

  auto post_load = apply_modifier(
      context, checkpoint_after_load_session_key, checkpoint);
  if (post_load.has_error()) {
    set_error_detail(context, post_load.error(), checkpoint_id_hint,
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
                          const graph_restore_shape &current_restore_shape)
    -> wh::core::result<void> {
  auto runtime_backend = resolve_runtime_backend(context);
  if (runtime_backend.has_error()) {
    if (runtime_backend.error() == wh::core::errc::not_found) {
      return {};
    }
    set_error_detail(context, runtime_backend.error(), graph_name,
                     "persist_store_lookup");
    return wh::core::result<void>::failure(runtime_backend.error());
  }
  if (runtime_backend.value().store == nullptr &&
      runtime_backend.value().backend == nullptr) {
    return {};
  }

  checkpoint_state checkpoint{};
  checkpoint.version = 1U;
  checkpoint.checkpoint_id = std::string{graph_name};
  checkpoint.restore_shape = current_restore_shape;
  checkpoint.node_states = state_table.states();
  checkpoint.resume_snapshot =
      context.resume_info.value_or(wh::core::resume_state{});
  auto cloned_rerun_inputs = save_rerun_inputs(rerun_state, node_keys);
  if (cloned_rerun_inputs.has_error()) {
    set_error_detail(context, cloned_rerun_inputs.error(), graph_name,
                     "persist_rerun_clone");
    return wh::core::result<void>::failure(cloned_rerun_inputs.error());
  }
  checkpoint.rerun_inputs = std::move(cloned_rerun_inputs).value();
  if (context.interrupt_info.has_value()) {
    checkpoint.interrupt_snapshot = wh::core::flatten_interrupt_signals(
        std::vector<wh::core::interrupt_signal>{
            wh::compose::to_reinterrupt_signal(*context.interrupt_info)});
  }
  auto stream_persisted = apply_stream_codecs_for_save(checkpoint, context);
  if (stream_persisted.has_error()) {
    set_error_detail(context, stream_persisted.error(), graph_name,
                     "persist_stream_convert");
    return stream_persisted;
  }

  auto pre_save =
      apply_modifier(context, checkpoint_before_save_session_key,
                     checkpoint);
  if (pre_save.has_error()) {
    set_error_detail(context, pre_save.error(), graph_name, "pre_save_modifier");
    return pre_save;
  }
  auto serializer_roundtrip =
      roundtrip_with_serializer(std::move(checkpoint), context);
  if (serializer_roundtrip.has_error()) {
    set_error_detail(context, serializer_roundtrip.error(), graph_name,
                     "persist_serializer_roundtrip");
    return wh::core::result<void>::failure(serializer_roundtrip.error());
  }
  checkpoint = std::move(serializer_roundtrip).value();

  checkpoint_save_options write_options{};
  auto write_options_ref = wh::core::session_value_ref<checkpoint_save_options>(
      context, checkpoint_save_session_key);
  if (write_options_ref.has_error() &&
      write_options_ref.error() != wh::core::errc::not_found) {
    set_error_detail(context, write_options_ref.error(), graph_name,
                     "persist_write_options_lookup");
    return wh::core::result<void>::failure(write_options_ref.error());
  }
  if (write_options_ref.has_value()) {
    write_options = write_options_ref.value().get();
  }
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
      set_error_detail(context, saved.error(), checkpoint_id_hint, "save");
      return wh::core::result<void>::failure(saved.error());
    }
  } else {
    auto saved = runtime_backend.value().store->save(std::move(checkpoint),
                                                     std::move(write_options));
    if (saved.has_error()) {
      set_error_detail(context, saved.error(), checkpoint_id_hint, "save");
      return wh::core::result<void>::failure(saved.error());
    }
  }

  auto post_save = apply_modifier(
      context, checkpoint_after_save_session_key, post_save_state);
  if (post_save.has_error()) {
    set_error_detail(context, post_save.error(), checkpoint_id_hint,
                     "post_save_modifier");
    return post_save;
  }
  return {};
}

} // namespace wh::compose::detail::checkpoint_runtime
