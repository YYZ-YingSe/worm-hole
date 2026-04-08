// Defines checkpoint store and serializer pipeline for compose recovery.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/compose/graph/checkpoint_state.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

/// One staged/committed checkpoint record.
struct checkpoint_record {
  /// Stable in-memory record id used by checkpoint retention bookkeeping.
  std::uint64_t record_id{0U};
  /// Checkpoint id storing this record.
  std::string checkpoint_id{};
  /// Thread-level short-memory key for index lookup.
  std::optional<std::string> thread_key{};
  /// Namespace-level long-memory key for index lookup.
  std::optional<std::string> namespace_key{};
  /// Branch replay key for history filtering.
  std::string branch{"main"};
  /// Optional parent branch key for replay lineage.
  std::optional<std::string> parent_branch{};
  /// Staged timestamp captured before commit.
  std::chrono::system_clock::time_point staged_at{};
  /// Commit timestamp when this record becomes visible.
  std::optional<std::chrono::system_clock::time_point> committed_at{};
  /// Checkpoint payload snapshot.
  checkpoint_state state{};
};

/// Result emitted after one staged checkpoint write is materialized.
struct checkpoint_stage_report {
  /// Stable record id assigned to this staged write.
  std::uint64_t record_id{0U};
  /// Checkpoint id bound to this staged write.
  std::string checkpoint_id{};
  /// Timestamp captured for the staged write.
  std::chrono::system_clock::time_point staged_at{};
  /// Pending record replaced for the same checkpoint id, when present.
  std::optional<std::uint64_t> replaced_pending_record_id{};
};

/// Result emitted after one staged checkpoint write becomes committed.
struct checkpoint_commit_report {
  /// Stable record id assigned to the committed checkpoint record.
  std::uint64_t record_id{0U};
  /// Checkpoint id bound to the committed record.
  std::string checkpoint_id{};
  /// Timestamp captured before commit.
  std::chrono::system_clock::time_point staged_at{};
  /// Commit timestamp when the record became visible.
  std::chrono::system_clock::time_point committed_at{};
  /// Pending record replaced for the same checkpoint id, when present.
  std::optional<std::uint64_t> replaced_pending_record_id{};
};

/// Write options used by staged/committed checkpoint save path.
struct checkpoint_save_options {
  /// Checkpoint id written by this save/stage request.
  std::optional<std::string> checkpoint_id{};
  /// Thread-level short-memory index key.
  std::optional<std::string> thread_key{};
  /// Namespace-level long-memory index key.
  std::optional<std::string> namespace_key{};
  /// Branch replay key.
  std::string branch{"main"};
  /// Optional parent branch key for lineage tracking.
  std::optional<std::string> parent_branch{};
  /// Optional staged timestamp override.
  std::optional<std::chrono::system_clock::time_point> staged_at{};
};

/// Checkpoint load options for restore/time-travel/branch replay.
struct checkpoint_load_options {
  /// Checkpoint id used for direct restore target resolution.
  std::optional<std::string> checkpoint_id{};
  /// Thread-level short-memory key for fallback id resolution.
  std::optional<std::string> thread_key{};
  /// Namespace-level long-memory key for fallback id resolution.
  std::optional<std::string> namespace_key{};
  /// Optional time-travel point; loads latest commit <= this point.
  std::optional<std::chrono::system_clock::time_point> as_of{};
  /// Optional branch filter for replay debugging.
  std::optional<std::string> branch{};
  /// Force new run bypasses store load path entirely.
  bool force_new_run{false};
  /// True skips state pre-handlers for restored rerun nodes in this run.
  bool skip_pre_handlers{false};
  /// True includes pending staged writes in restore target selection.
  bool include_pending{true};
};

/// Lifecycle policy for checkpoint retention pruning.
struct checkpoint_retention_policy {
  /// Time-to-live for committed records.
  std::optional<std::chrono::seconds> ttl{};
  /// Max committed records retained per checkpoint id.
  std::optional<std::size_t> max_records_per_checkpoint_id{};
  /// Max staged writes retained before oldest pending writes are reclaimed.
  std::optional<std::size_t> max_pending_writes{};
  /// Max thread-layer index entries retained (short-memory layer).
  std::optional<std::size_t> max_thread_index_entries{};
  /// Max namespace-layer index entries retained (long-memory layer).
  std::optional<std::size_t> max_namespace_index_entries{};
  /// True removes pending staged writes during prune.
  bool drop_pending_writes{false};
};

/// Structured report emitted after pruning checkpoint records and indexes.
struct checkpoint_prune_report {
  /// Committed checkpoint record ids removed during this prune pass.
  std::vector<std::uint64_t> removed_committed_record_ids{};
  /// Pending checkpoint record ids removed during this prune pass.
  std::vector<std::uint64_t> removed_pending_record_ids{};
  /// Number of thread-index entries reclaimed during this prune pass.
  std::size_t removed_thread_index_entries{0U};
  /// Number of namespace-index entries reclaimed during this prune pass.
  std::size_t removed_namespace_index_entries{0U};

  /// Returns the total number of removed records and index entries.
  [[nodiscard]] auto removed_total() const noexcept -> std::size_t {
    return removed_committed_record_ids.size() +
           removed_pending_record_ids.size() + removed_thread_index_entries +
           removed_namespace_index_entries;
  }
};

/// Restore plan indicating whether checkpoint store was used.
struct checkpoint_restore_plan {
  /// True when restore should load from checkpoint store.
  bool restore_from_checkpoint{false};
  /// Loaded checkpoint when `restore_from_checkpoint == true`.
  std::optional<checkpoint_state> checkpoint{};
};

/// Structured checkpoint failure detail with checkpoint-id context.
struct checkpoint_error_detail {
  /// Structured error code.
  wh::core::error_code code{wh::core::errc::ok};
  /// Checkpoint id resolved for the failing operation.
  std::string checkpoint_id{};
  /// Operation label (`load/save/prepare_restore/...`).
  std::string operation{};
};

/// One-shot forwarded checkpoint map consumed by subgraph restore path.
using forwarded_checkpoint_map =
    std::unordered_map<std::string, checkpoint_state,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>;

/// Checkpoint state modifier called before/after load/save I/O.
using checkpoint_state_modifier =
    wh::core::callback_function<wh::core::result<void>(
        checkpoint_state &, wh::core::run_context &) const>;

/// Node-state modifier callback applied by NodePath-scoped checkpoint hooks.
using checkpoint_node_modifier =
    wh::core::callback_function<wh::core::result<void>(
        graph_node_state &, wh::core::run_context &) const>;

/// NodePath-scoped node-state modifier entry.
struct checkpoint_node_hook {
  /// Target node path for this modifier scope.
  node_path path{};
  /// True also applies this modifier to descendants under `path`.
  bool include_descendants{true};
  /// Modifier callback applied to matching checkpoint node states.
  checkpoint_node_modifier modifier{nullptr};
};

/// NodePath-scoped checkpoint modifier list.
using checkpoint_node_hooks = std::vector<checkpoint_node_hook>;

/// Marker payload persisted for stream->value converted checkpoint entries.
struct checkpoint_stream_value_payload {
  /// Value payload produced by stream->value conversion.
  graph_value value{};
};

/// Converts one stream payload into checkpoint-safe value payload.
using checkpoint_stream_save =
    wh::core::callback_function<wh::core::result<graph_value>(
        graph_stream_reader &&, wh::core::run_context &) const>;
/// Converts one persisted value payload back into stream payload.
using checkpoint_stream_load =
    wh::core::callback_function<wh::core::result<graph_stream_reader>(
        graph_value &&, wh::core::run_context &) const>;

/// One bidirectional stream convert pair registered by node key/path.
struct checkpoint_stream_codec {
  /// Stream -> value conversion callback used before checkpoint save.
  checkpoint_stream_save to_value{nullptr};
  /// Value -> stream conversion callback used during checkpoint restore.
  checkpoint_stream_load to_stream{nullptr};
};

/// Builds the default stream/value codec used by checkpoint bridge helpers.
[[nodiscard]] inline auto make_default_stream_codec()
    -> checkpoint_stream_codec {
  return checkpoint_stream_codec{
      .to_value = [](graph_stream_reader &&reader,
                     wh::core::run_context &) -> wh::core::result<graph_value> {
        auto collected = collect_graph_stream_reader(std::move(reader));
        if (collected.has_error()) {
          return wh::core::result<graph_value>::failure(collected.error());
        }
        return wh::core::any(std::move(collected).value());
      },
      .to_stream = [](graph_value &&value, wh::core::run_context &)
          -> wh::core::result<graph_stream_reader> {
        if (auto *reader = wh::core::any_cast<graph_stream_reader>(&value);
            reader != nullptr) {
          return std::move(*reader);
        }
        if (auto *chunks = wh::core::any_cast<std::vector<graph_value>>(&value);
            chunks != nullptr) {
          return make_values_stream_reader(std::move(*chunks));
        }
        return make_single_value_stream_reader(std::move(value));
      },
  };
}

/// Registry of stream convert pairs keyed by node key/path.
using checkpoint_stream_codecs =
    std::unordered_map<std::string, checkpoint_stream_codec,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>;

/// Runtime checkpoint backend load callback for pluggable stores.
using checkpoint_backend_prepare_restore =
    wh::core::callback_function<wh::core::result<checkpoint_restore_plan>(
        const checkpoint_load_options &, wh::core::run_context &) const>;
/// Runtime checkpoint backend save callback for pluggable stores.
using checkpoint_backend_save =
    wh::core::callback_function<wh::core::result<void>(
        checkpoint_state &&, checkpoint_save_options &&,
        wh::core::run_context &) const>;

/// Serializer encode callback used before checkpoint backend/store writes.
using checkpoint_serializer_encode =
    wh::core::callback_function<wh::core::result<graph_value>(
        checkpoint_state &&, wh::core::run_context &) const>;
/// Serializer decode callback used after checkpoint backend/store reads.
using checkpoint_serializer_decode =
    wh::core::callback_function<wh::core::result<checkpoint_state>(
        graph_value &&, wh::core::run_context &) const>;

/// Serializer pair for custom/default checkpoint encode/decode flows.
struct checkpoint_serializer {
  /// Encode callback producing one serialized checkpoint payload.
  checkpoint_serializer_encode encode{nullptr};
  /// Decode callback restoring one checkpoint state from serialized payload.
  checkpoint_serializer_decode decode{nullptr};
};

/// Pluggable checkpoint backend adapter used by runtime session wiring.
struct checkpoint_backend {
  /// Restore callback for runtime checkpoint loads.
  checkpoint_backend_prepare_restore prepare_restore{nullptr};
  /// Save callback for runtime checkpoint persists.
  checkpoint_backend_save save{nullptr};
};

class checkpoint_store;

/// In-memory checkpoint store with restore/time-travel APIs.
class checkpoint_store {
public:
  /// Saves one checkpoint snapshot (copy path).
  auto save(const checkpoint_state &state)
      -> wh::core::result<checkpoint_commit_report> {
    return save_impl(state, checkpoint_save_options{});
  }

  /// Saves one checkpoint snapshot (move path).
  auto save(checkpoint_state &&state)
      -> wh::core::result<checkpoint_commit_report> {
    return save_impl(std::move(state), checkpoint_save_options{});
  }

  /// Saves one checkpoint snapshot with explicit id/write options (copy path).
  auto save(const checkpoint_state &state,
            const checkpoint_save_options &options)
      -> wh::core::result<checkpoint_commit_report> {
    return save_impl(state, options);
  }

  /// Saves one checkpoint snapshot with explicit id/write options (move path).
  auto save(checkpoint_state &&state, checkpoint_save_options &&options)
      -> wh::core::result<checkpoint_commit_report> {
    return save_impl(std::move(state), std::move(options));
  }

  /// Stages one pending checkpoint write without making it visible.
  auto stage_write(const checkpoint_state &state,
                   const checkpoint_save_options &options = {})
      -> wh::core::result<checkpoint_stage_report> {
    return stage_write_impl(state, options);
  }

  /// Stages one pending checkpoint write without making it visible.
  auto stage_write(checkpoint_state &&state,
                   checkpoint_save_options &&options = {})
      -> wh::core::result<checkpoint_stage_report> {
    return stage_write_impl(std::move(state), std::move(options));
  }

  /// Commits one pending checkpoint write atomically.
  auto commit_staged(const std::string_view checkpoint_id)
      -> wh::core::result<checkpoint_commit_report> {
    const auto staged_iter = pending_writes_.find(checkpoint_id);
    if (staged_iter == pending_writes_.end()) {
      return wh::core::result<checkpoint_commit_report>::failure(
          wh::core::errc::not_found);
    }
    auto record = std::move(staged_iter->second);
    pending_writes_.erase(staged_iter);

    record.committed_at = std::chrono::system_clock::now();
    auto &history = committed_history_[record.checkpoint_id];
    history.push_back(std::move(record));
    auto &committed = history.back();
    latest_checkpoint_id_ = committed.checkpoint_id;
    if (committed.thread_key.has_value()) {
      latest_id_by_thread_.insert_or_assign(*committed.thread_key,
                                            committed.checkpoint_id);
    }
    if (committed.namespace_key.has_value()) {
      latest_id_by_namespace_.insert_or_assign(*committed.namespace_key,
                                               committed.checkpoint_id);
    }
    return to_commit_report(committed, std::nullopt);
  }

  /// Aborts one pending checkpoint write.
  auto abort_staged(const std::string_view checkpoint_id)
      -> wh::core::result<checkpoint_stage_report> {
    const auto iter = pending_writes_.find(checkpoint_id);
    if (iter == pending_writes_.end()) {
      return wh::core::result<checkpoint_stage_report>::failure(
          wh::core::errc::not_found);
    }
    auto record = std::move(iter->second);
    pending_writes_.erase(iter);
    return to_stage_report(record, std::nullopt);
  }

  /// Loads latest checkpoint snapshot.
  [[nodiscard]] auto load() const -> wh::core::result<checkpoint_state> {
    if (!latest_checkpoint_id_.has_value()) {
      return wh::core::result<checkpoint_state>::failure(
          wh::core::errc::not_found);
    }
    checkpoint_load_options options{};
    options.checkpoint_id = latest_checkpoint_id_;
    return load(options);
  }

  /// Loads checkpoint snapshot using id/layer/time/branch options.
  [[nodiscard]] auto load(const checkpoint_load_options &options) const
      -> wh::core::result<checkpoint_state> {
    if (options.force_new_run) {
      return wh::core::result<checkpoint_state>::failure(
          wh::core::errc::not_found);
    }
    auto resolved_id = resolve_read_checkpoint_id(options);
    if (resolved_id.has_error()) {
      return wh::core::result<checkpoint_state>::failure(resolved_id.error());
    }
    const auto history_iter = committed_history_.find(resolved_id.value());
    if (history_iter == committed_history_.end() ||
        history_iter->second.empty()) {
      return wh::core::result<checkpoint_state>::failure(
          wh::core::errc::not_found);
    }
    const auto *record = select_record(history_iter->second, options);
    if (record == nullptr) {
      return wh::core::result<checkpoint_state>::failure(
          wh::core::errc::not_found);
    }
    return record->state;
  }

  /// Builds one restore plan (supports `force_new_run` bypass semantics).
  [[nodiscard]] auto
  prepare_restore(const checkpoint_load_options &options = {}) const
      -> wh::core::result<checkpoint_restore_plan> {
    if (options.force_new_run) {
      return checkpoint_restore_plan{
          .restore_from_checkpoint = false,
          .checkpoint = std::nullopt,
      };
    }
    auto resolved_id = resolve_read_checkpoint_id(options);
    if (resolved_id.has_error()) {
      return wh::core::result<checkpoint_restore_plan>::failure(
          resolved_id.error());
    }
    if (options.include_pending) {
      const auto pending_iter = pending_writes_.find(resolved_id.value());
      if (pending_iter != pending_writes_.end() &&
          pending_matches(pending_iter->second, options)) {
        return checkpoint_restore_plan{
            .restore_from_checkpoint = true,
            .checkpoint = pending_iter->second.state,
        };
      }
    }
    auto loaded = load(options);
    if (loaded.has_error()) {
      return wh::core::result<checkpoint_restore_plan>::failure(loaded.error());
    }
    return checkpoint_restore_plan{
        .restore_from_checkpoint = true,
        .checkpoint = std::move(loaded).value(),
    };
  }

  /// Returns committed history list for one checkpoint id.
  [[nodiscard]] auto history(const std::string_view checkpoint_id) const
      -> wh::core::result<
          std::reference_wrapper<const std::vector<checkpoint_record>>> {
    const auto iter = committed_history_.find(checkpoint_id);
    if (iter == committed_history_.end()) {
      return wh::core::result<std::reference_wrapper<const std::vector<
          checkpoint_record>>>::failure(wh::core::errc::not_found);
    }
    return std::cref(iter->second);
  }

  /// Returns latest checkpoint id bound to one thread key.
  [[nodiscard]] auto
  latest_thread_checkpoint_id(const std::string_view thread_key) const
      -> wh::core::result<std::string> {
    const auto iter = latest_id_by_thread_.find(thread_key);
    if (iter == latest_id_by_thread_.end()) {
      return wh::core::result<std::string>::failure(wh::core::errc::not_found);
    }
    return iter->second;
  }

  /// Returns latest checkpoint id bound to one namespace key.
  [[nodiscard]] auto
  latest_namespace_checkpoint_id(const std::string_view namespace_key) const
      -> wh::core::result<std::string> {
    const auto iter = latest_id_by_namespace_.find(namespace_key);
    if (iter == latest_id_by_namespace_.end()) {
      return wh::core::result<std::string>::failure(wh::core::errc::not_found);
    }
    return iter->second;
  }

  /// Prunes committed/pending records by lifecycle policy.
  auto prune(const checkpoint_retention_policy &policy,
             const std::chrono::system_clock::time_point now =
                 std::chrono::system_clock::now()) -> checkpoint_prune_report {
    checkpoint_prune_report report{};
    std::vector<std::string> empty_history_ids{};
    for (auto &[checkpoint_id, history] : committed_history_) {
      if (policy.ttl.has_value()) {
        const auto ttl = *policy.ttl;
        auto kept = std::vector<checkpoint_record>{};
        kept.reserve(history.size());
        for (auto &record : history) {
          if (!record.committed_at.has_value() ||
              (now - *record.committed_at) <= ttl) {
            kept.push_back(std::move(record));
            continue;
          }
          report.removed_committed_record_ids.push_back(record.record_id);
        }
        history = std::move(kept);
      }

      if (policy.max_records_per_checkpoint_id.has_value() &&
          history.size() > *policy.max_records_per_checkpoint_id) {
        const auto remove_count =
            history.size() - *policy.max_records_per_checkpoint_id;
        for (std::size_t index = 0U; index < remove_count; ++index) {
          report.removed_committed_record_ids.push_back(
              history[index].record_id);
        }
        history.erase(history.begin(),
                      history.begin() +
                          static_cast<std::ptrdiff_t>(remove_count));
      }

      if (history.empty()) {
        empty_history_ids.push_back(checkpoint_id);
      }
    }

    for (const auto &checkpoint_id : empty_history_ids) {
      committed_history_.erase(checkpoint_id);
      erase_indexes(checkpoint_id);
    }

    if (policy.drop_pending_writes) {
      for (const auto &[checkpoint_id, record] : pending_writes_) {
        static_cast<void>(checkpoint_id);
        report.removed_pending_record_ids.push_back(record.record_id);
      }
      pending_writes_.clear();
    } else if (policy.max_pending_writes.has_value() &&
               pending_writes_.size() > *policy.max_pending_writes) {
      std::vector<std::pair<std::string, std::chrono::system_clock::time_point>>
          pending_keys{};
      pending_keys.reserve(pending_writes_.size());
      for (const auto &[checkpoint_id, record] : pending_writes_) {
        pending_keys.emplace_back(checkpoint_id, record.staged_at);
      }
      std::sort(pending_keys.begin(), pending_keys.end(),
                [](const auto &lhs, const auto &rhs) noexcept {
                  if (lhs.second == rhs.second) {
                    return lhs.first < rhs.first;
                  }
                  return lhs.second < rhs.second;
                });
      const auto remove_count =
          pending_keys.size() - *policy.max_pending_writes;
      for (std::size_t index = 0U; index < remove_count; ++index) {
        const auto pending_iter =
            pending_writes_.find(pending_keys[index].first);
        if (pending_iter == pending_writes_.end()) {
          continue;
        }
        report.removed_pending_record_ids.push_back(
            pending_iter->second.record_id);
        pending_writes_.erase(pending_iter);
      }
    }

    if (policy.max_thread_index_entries.has_value()) {
      report.removed_thread_index_entries =
          prune_index(latest_id_by_thread_, *policy.max_thread_index_entries);
    }
    if (policy.max_namespace_index_entries.has_value()) {
      report.removed_namespace_index_entries = prune_index(
          latest_id_by_namespace_, *policy.max_namespace_index_entries);
    }

    if (latest_checkpoint_id_.has_value() &&
        !committed_history_.contains(*latest_checkpoint_id_)) {
      latest_checkpoint_id_.reset();
    }
    return report;
  }

private:
  [[nodiscard]] static auto
  to_stage_report(const checkpoint_record &record,
                  std::optional<std::uint64_t> replaced_pending_record_id)
      -> checkpoint_stage_report {
    return checkpoint_stage_report{
        .record_id = record.record_id,
        .checkpoint_id = record.checkpoint_id,
        .staged_at = record.staged_at,
        .replaced_pending_record_id = replaced_pending_record_id,
    };
  }

  [[nodiscard]] static auto
  to_commit_report(const checkpoint_record &record,
                   std::optional<std::uint64_t> replaced_pending_record_id)
      -> checkpoint_commit_report {
    return checkpoint_commit_report{
        .record_id = record.record_id,
        .checkpoint_id = record.checkpoint_id,
        .staged_at = record.staged_at,
        .committed_at = record.committed_at.value_or(record.staged_at),
        .replaced_pending_record_id = replaced_pending_record_id,
    };
  }

  template <typename state_t, typename options_t>
    requires std::same_as<wh::core::remove_cvref_t<state_t>,
                          checkpoint_state> &&
             std::same_as<wh::core::remove_cvref_t<options_t>,
                          checkpoint_save_options>
  auto save_impl(state_t &&state, options_t &&options)
      -> wh::core::result<checkpoint_commit_report> {
    auto staged = stage_write_impl(std::forward<state_t>(state),
                                   std::forward<options_t>(options));
    if (staged.has_error()) {
      return wh::core::result<checkpoint_commit_report>::failure(
          staged.error());
    }
    auto committed = commit_staged(staged.value().checkpoint_id);
    if (committed.has_error()) {
      return committed;
    }
    auto report = std::move(committed).value();
    report.replaced_pending_record_id =
        staged.value().replaced_pending_record_id;
    return report;
  }

  template <typename state_t, typename options_t>
    requires std::same_as<wh::core::remove_cvref_t<state_t>,
                          checkpoint_state> &&
             std::same_as<wh::core::remove_cvref_t<options_t>,
                          checkpoint_save_options>
  auto stage_write_impl(state_t &&state, options_t &&options)
      -> wh::core::result<checkpoint_stage_report> {
    auto checkpoint_id = resolve_write_checkpoint_id(options);
    if (checkpoint_id.has_error()) {
      return wh::core::result<checkpoint_stage_report>::failure(
          checkpoint_id.error());
    }
    checkpoint_record record{};
    record.record_id = next_record_id_++;
    record.checkpoint_id = std::move(checkpoint_id).value();
    if constexpr (std::is_lvalue_reference_v<options_t>) {
      record.thread_key = options.thread_key;
      record.namespace_key = options.namespace_key;
      record.branch = options.branch;
      record.parent_branch = options.parent_branch;
      record.staged_at =
          options.staged_at.value_or(std::chrono::system_clock::now());
    } else {
      record.thread_key = std::move(options.thread_key);
      record.namespace_key = std::move(options.namespace_key);
      record.branch = std::move(options.branch);
      record.parent_branch = std::move(options.parent_branch);
      record.staged_at = options.staged_at.has_value()
                             ? std::move(options.staged_at).value()
                             : std::chrono::system_clock::now();
    }
    if constexpr (std::is_lvalue_reference_v<state_t>) {
      record.state = state;
    } else {
      record.state = std::move(state);
    }
    std::optional<std::uint64_t> replaced_pending_record_id{};
    const auto pending_iter = pending_writes_.find(record.checkpoint_id);
    if (pending_iter != pending_writes_.end()) {
      replaced_pending_record_id = pending_iter->second.record_id;
    }
    auto report = to_stage_report(record, replaced_pending_record_id);
    pending_writes_.insert_or_assign(record.checkpoint_id, std::move(record));
    return report;
  }

  [[nodiscard]] static auto
  resolve_write_checkpoint_id(const checkpoint_save_options &options)
      -> wh::core::result<std::string> {
    if (options.checkpoint_id.has_value() && !options.checkpoint_id->empty()) {
      return *options.checkpoint_id;
    }
    return std::string{"default"};
  }

  [[nodiscard]] auto
  resolve_read_checkpoint_id(const checkpoint_load_options &options) const
      -> wh::core::result<std::string> {
    if (options.checkpoint_id.has_value() && !options.checkpoint_id->empty()) {
      return *options.checkpoint_id;
    }
    if (options.thread_key.has_value()) {
      const auto thread_iter = latest_id_by_thread_.find(*options.thread_key);
      if (thread_iter != latest_id_by_thread_.end()) {
        return thread_iter->second;
      }
    }
    if (options.namespace_key.has_value()) {
      const auto namespace_iter =
          latest_id_by_namespace_.find(*options.namespace_key);
      if (namespace_iter != latest_id_by_namespace_.end()) {
        return namespace_iter->second;
      }
    }
    if (latest_checkpoint_id_.has_value()) {
      return *latest_checkpoint_id_;
    }
    return wh::core::result<std::string>::failure(wh::core::errc::not_found);
  }

  [[nodiscard]] static auto
  select_record(const std::vector<checkpoint_record> &history,
                const checkpoint_load_options &options)
      -> const checkpoint_record * {
    for (auto iter = history.rbegin(); iter != history.rend(); ++iter) {
      if (options.branch.has_value() && iter->branch != *options.branch) {
        continue;
      }
      if (options.as_of.has_value()) {
        if (!iter->committed_at.has_value() ||
            iter->committed_at.value() > *options.as_of) {
          continue;
        }
      }
      return &(*iter);
    }
    return nullptr;
  }

  [[nodiscard]] static auto
  pending_matches(const checkpoint_record &record,
                  const checkpoint_load_options &options) -> bool {
    if (options.branch.has_value() && record.branch != *options.branch) {
      return false;
    }
    if (options.as_of.has_value() && record.staged_at > *options.as_of) {
      return false;
    }
    return true;
  }

  auto erase_indexes(const std::string_view checkpoint_id) -> void {
    std::erase_if(latest_id_by_thread_, [&](const auto &entry) {
      return entry.second == checkpoint_id;
    });
    std::erase_if(latest_id_by_namespace_, [&](const auto &entry) {
      return entry.second == checkpoint_id;
    });
    if (latest_checkpoint_id_.has_value() &&
        *latest_checkpoint_id_ == checkpoint_id) {
      latest_checkpoint_id_.reset();
    }
  }

  template <typename index_map_t>
  auto prune_index(index_map_t &index_map, const std::size_t limit) const
      -> std::size_t {
    if (index_map.size() <= limit) {
      return 0U;
    }
    struct entry_meta {
      std::string key{};
      std::chrono::system_clock::time_point recency{};
    };
    std::vector<entry_meta> entries{};
    entries.reserve(index_map.size());
    for (const auto &[layer_key, checkpoint_id] : index_map) {
      auto recency = std::chrono::system_clock::time_point{};
      const auto history_iter = committed_history_.find(checkpoint_id);
      if (history_iter != committed_history_.end() &&
          !history_iter->second.empty()) {
        const auto &latest_record = history_iter->second.back();
        recency = latest_record.committed_at.value_or(latest_record.staged_at);
      } else {
        const auto pending_iter = pending_writes_.find(checkpoint_id);
        if (pending_iter != pending_writes_.end()) {
          recency = pending_iter->second.staged_at;
        }
      }
      entries.push_back(entry_meta{
          .key = layer_key,
          .recency = recency,
      });
    }

    std::sort(entries.begin(), entries.end(),
              [](const entry_meta &lhs, const entry_meta &rhs) noexcept {
                if (lhs.recency == rhs.recency) {
                  return lhs.key < rhs.key;
                }
                return lhs.recency > rhs.recency;
              });

    std::size_t removed = 0U;
    for (std::size_t index = limit; index < entries.size(); ++index) {
      removed += index_map.erase(entries[index].key);
    }
    return removed;
  }

  std::unordered_map<std::string, std::vector<checkpoint_record>,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      committed_history_{};
  std::unordered_map<std::string, checkpoint_record,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      pending_writes_{};
  std::unordered_map<std::string, std::string,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      latest_id_by_thread_{};
  std::unordered_map<std::string, std::string,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      latest_id_by_namespace_{};
  std::optional<std::string> latest_checkpoint_id_{};
  std::uint64_t next_record_id_{1U};
};

} // namespace wh::compose
