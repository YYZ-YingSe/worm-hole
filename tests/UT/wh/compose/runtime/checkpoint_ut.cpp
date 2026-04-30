#include <chrono>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/compose/runtime/checkpoint.hpp"

namespace {

[[nodiscard]] auto make_checkpoint_state(std::string id, std::string branch = "main")
    -> wh::compose::checkpoint_state {
  wh::compose::checkpoint_state state{};
  state.checkpoint_id = std::move(id);
  state.branch = std::move(branch);
  state.runtime.dag = wh::compose::checkpoint_dag_runtime_state{};
  state.runtime.dag->pending_inputs.entry = wh::compose::graph_value{7};
  return state;
}

} // namespace

TEST_CASE("checkpoint reports codecs and store lifecycle cover staged committed and restore paths",
          "[UT][wh/compose/runtime/"
          "checkpoint.hpp][checkpoint_store::save][condition][branch][boundary]") {
  wh::compose::checkpoint_prune_report empty_report{};
  REQUIRE(empty_report.removed_total() == 0U);
  empty_report.removed_committed_record_ids.push_back(1U);
  empty_report.removed_pending_record_ids.push_back(2U);
  empty_report.removed_thread_index_entries = 3U;
  empty_report.removed_namespace_index_entries = 4U;
  REQUIRE(empty_report.removed_total() == 9U);

  auto codec = wh::compose::make_default_stream_codec();
  wh::core::run_context context{};

  auto waited = stdexec::sync_wait(
      codec.to_value(wh::compose::make_single_value_stream_reader(std::string{"chunk"}).value(),
                     context));
  REQUIRE(waited.has_value());
  auto to_value = std::get<0>(std::move(*waited));
  REQUIRE(to_value.has_value());
  auto *chunks = wh::core::any_cast<std::vector<wh::compose::graph_value>>(&to_value.value());
  REQUIRE(chunks != nullptr);
  REQUIRE(chunks->size() == 1U);
  REQUIRE(*wh::core::any_cast<std::string>(&chunks->front()) == "chunk");

  auto existing_reader = wh::compose::make_single_value_stream_reader(std::string{"ready"});
  REQUIRE(existing_reader.has_value());
  auto restored_reader =
      codec.to_stream(wh::compose::graph_value{std::move(existing_reader).value()}, context);
  REQUIRE(restored_reader.has_value());
  auto restored_chunks =
      wh::compose::collect_graph_stream_reader(std::move(restored_reader).value());
  REQUIRE(restored_chunks.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&restored_chunks.value().front()) == "ready");

  auto from_vector = codec.to_stream(wh::compose::graph_value{std::vector<wh::compose::graph_value>{
                                         wh::compose::graph_value{1}, wh::compose::graph_value{2}}},
                                     context);
  REQUIRE(from_vector.has_value());
  auto vector_chunks = wh::compose::collect_graph_stream_reader(std::move(from_vector).value());
  REQUIRE(vector_chunks.has_value());
  REQUIRE(vector_chunks.value().size() == 2U);

  auto from_single = codec.to_stream(wh::compose::graph_value{9}, context);
  REQUIRE(from_single.has_value());
  auto single_chunks = wh::compose::collect_graph_stream_reader(std::move(from_single).value());
  REQUIRE(single_chunks.has_value());
  REQUIRE(*wh::core::any_cast<int>(&single_chunks.value().front()) == 9);

  wh::compose::checkpoint_store store{};
  auto committed = store.save(make_checkpoint_state("cp-1"), {
                                                                 .checkpoint_id = "cp-1",
                                                                 .thread_key = "thread-a",
                                                                 .namespace_key = "ns-a",
                                                             });
  REQUIRE(committed.has_value());
  REQUIRE(committed.value().checkpoint_id == "cp-1");
  REQUIRE(committed.value().record_id == 1U);

  auto loaded = store.load();
  REQUIRE(loaded.has_value());
  REQUIRE(loaded.value().checkpoint_id == "cp-1");

  auto latest_thread = store.latest_thread_checkpoint_id("thread-a");
  REQUIRE(latest_thread.has_value());
  REQUIRE(latest_thread.value() == "cp-1");
  auto latest_namespace = store.latest_namespace_checkpoint_id("ns-a");
  REQUIRE(latest_namespace.has_value());
  REQUIRE(latest_namespace.value() == "cp-1");

  auto history = store.history("cp-1");
  REQUIRE(history.has_value());
  REQUIRE(history.value().get().size() == 1U);

  auto staged =
      store.stage_write(make_checkpoint_state("cp-pending"), {
                                                                 .checkpoint_id = "cp-pending",
                                                                 .branch = "feature",
                                                             });
  REQUIRE(staged.has_value());
  REQUIRE(staged.value().checkpoint_id == "cp-pending");
  auto restore_pending = store.prepare_restore({
      .checkpoint_id = "cp-pending",
      .include_pending = true,
  });
  REQUIRE(restore_pending.has_value());
  REQUIRE(restore_pending.value().restore_from_checkpoint);
  REQUIRE(restore_pending.value().checkpoint->checkpoint_id == "cp-pending");

  auto committed_pending = store.commit_staged("cp-pending");
  REQUIRE(committed_pending.has_value());
  auto branch_loaded = store.load({
      .checkpoint_id = "cp-pending",
      .branch = "feature",
  });
  REQUIRE(branch_loaded.has_value());
  REQUIRE(branch_loaded.value().checkpoint_id == "cp-pending");

  auto aborted =
      store.stage_write(make_checkpoint_state("cp-abort"), {
                                                               .checkpoint_id = "cp-abort",
                                                           });
  REQUIRE(aborted.has_value());
  auto abort_report = store.abort_staged("cp-abort");
  REQUIRE(abort_report.has_value());
  REQUIRE(abort_report.value().checkpoint_id == "cp-abort");

  auto forced = store.prepare_restore({
      .checkpoint_id = "cp-1",
      .force_new_run = true,
  });
  REQUIRE(forced.has_value());
  REQUIRE_FALSE(forced.value().restore_from_checkpoint);
  REQUIRE_FALSE(forced.value().checkpoint.has_value());
}

TEST_CASE("checkpoint store prune enforces ttl record pending and index limits",
          "[UT][wh/compose/runtime/"
          "checkpoint.hpp][checkpoint_store::prune][condition][branch][boundary]") {
  wh::compose::checkpoint_store store{};
  REQUIRE(store
              .save(make_checkpoint_state("cp-a"),
                    {
                        .checkpoint_id = "cp-a",
                        .thread_key = "thread-a",
                        .namespace_key = "ns-a",
                    })
              .has_value());
  REQUIRE(store
              .save(make_checkpoint_state("cp-a"),
                    {
                        .checkpoint_id = "cp-a",
                        .thread_key = "thread-a",
                        .namespace_key = "ns-a",
                    })
              .has_value());
  REQUIRE(store
              .save(make_checkpoint_state("cp-b"),
                    {
                        .checkpoint_id = "cp-b",
                        .thread_key = "thread-b",
                        .namespace_key = "ns-b",
                    })
              .has_value());
  REQUIRE(
      store
          .stage_write(make_checkpoint_state("pending-1"),
                       {
                           .checkpoint_id = "pending-1",
                           .staged_at = std::chrono::system_clock::now() - std::chrono::hours(3),
                       })
          .has_value());
  REQUIRE(
      store
          .stage_write(make_checkpoint_state("pending-2"),
                       {
                           .checkpoint_id = "pending-2",
                           .staged_at = std::chrono::system_clock::now() - std::chrono::hours(2),
                       })
          .has_value());

  auto report = store.prune(
      wh::compose::checkpoint_retention_policy{
          .max_records_per_checkpoint_id = 1U,
          .max_pending_writes = 1U,
          .max_thread_index_entries = 1U,
          .max_namespace_index_entries = 1U,
      },
      std::chrono::system_clock::now() + std::chrono::hours(4));
  REQUIRE(report.removed_total() >= 4U);

  auto thread_a = store.latest_thread_checkpoint_id("thread-a");
  auto thread_b = store.latest_thread_checkpoint_id("thread-b");
  REQUIRE((thread_a.has_value() || thread_b.has_value()));

  auto expired = store.prune(
      wh::compose::checkpoint_retention_policy{
          .ttl = std::chrono::seconds{1},
      },
      std::chrono::system_clock::now() + std::chrono::hours(4));
  REQUIRE(expired.removed_committed_record_ids.size() >= 2U);

  auto clear_pending = store.prune(wh::compose::checkpoint_retention_policy{
      .drop_pending_writes = true,
  });
  REQUIRE(clear_pending.removed_pending_record_ids.size() <= 1U);
}

TEST_CASE("checkpoint store surfaces not-found and replacement branches for direct public apis",
          "[UT][wh/compose/runtime/"
          "checkpoint.hpp][checkpoint_store::load][condition][branch][boundary]") {
  wh::compose::checkpoint_store empty{};
  REQUIRE(empty.load().has_error());
  REQUIRE(empty.load().error() == wh::core::errc::not_found);
  REQUIRE(empty.load({.force_new_run = true}).has_error());
  REQUIRE(empty.load({.force_new_run = true}).error() == wh::core::errc::not_found);
  REQUIRE(empty.prepare_restore().has_error());
  REQUIRE(empty.history("missing").has_error());
  REQUIRE(empty.latest_thread_checkpoint_id("missing").has_error());
  REQUIRE(empty.latest_namespace_checkpoint_id("missing").has_error());
  REQUIRE(empty.commit_staged("missing").has_error());
  REQUIRE(empty.abort_staged("missing").has_error());

  wh::compose::checkpoint_store store{};
  auto default_saved = store.save(make_checkpoint_state("state-default"), {});
  REQUIRE(default_saved.has_value());
  REQUIRE(default_saved->checkpoint_id == "default");

  auto first_pending =
      store.stage_write(make_checkpoint_state("state-1"), {
                                                              .checkpoint_id = "pending",
                                                              .branch = "main",
                                                          });
  REQUIRE(first_pending.has_value());
  auto replaced_pending =
      store.stage_write(make_checkpoint_state("state-2"), {
                                                              .checkpoint_id = "pending",
                                                              .branch = "main",
                                                          });
  REQUIRE(replaced_pending.has_value());
  REQUIRE(replaced_pending->replaced_pending_record_id ==
          std::optional<std::uint64_t>{first_pending->record_id});

  auto committed = store.commit_staged("pending");
  REQUIRE(committed.has_value());
  REQUIRE_FALSE(committed->replaced_pending_record_id.has_value());

  auto no_pending_restore = store.prepare_restore({
      .checkpoint_id = "pending",
      .include_pending = false,
  });
  REQUIRE(no_pending_restore.has_value());
  REQUIRE(no_pending_restore->restore_from_checkpoint);
  REQUIRE(no_pending_restore->checkpoint->checkpoint_id == "state-2");
}

TEST_CASE("checkpoint store ownerizes borrowed node payloads before persistence",
          "[UT][wh/compose/runtime/checkpoint.hpp][checkpoint_store][ownership][boundary]") {
  std::string request_text = "original-request";

  wh::compose::checkpoint_state state{};
  state.checkpoint_id = "cp-owned";
  state.runtime.dag = wh::compose::checkpoint_dag_runtime_state{};
  state.runtime.dag->pending_inputs.nodes.push_back(wh::compose::checkpoint_node_input{
      .node_id = 1U,
      .key = "tool-node",
      .input = wh::compose::graph_value{wh::compose::tool_batch{
          .calls = std::vector<wh::compose::tool_call>{wh::compose::tool_call{
              .call_id = "call-1",
              .tool_name = "delegate",
              .payload = wh::core::any::ref(request_text),
          }}}},
  });

  wh::compose::checkpoint_store store{};
  REQUIRE(store.save(std::move(state), {.checkpoint_id = "cp-owned"}).has_value());

  request_text = "mutated-after-save";

  auto loaded = store.load({.checkpoint_id = "cp-owned"});
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->runtime.dag.has_value());
  const auto *tool_node = [&]() -> const wh::compose::graph_value * {
    for (const auto &node_input : loaded->runtime.dag->pending_inputs.nodes) {
      if (node_input.key == "tool-node") {
        return std::addressof(node_input.input);
      }
    }
    return nullptr;
  }();
  REQUIRE(tool_node != nullptr);

  const auto *batch = wh::core::any_cast<wh::compose::tool_batch>(tool_node);
  REQUIRE(batch != nullptr);
  REQUIRE(batch->calls.size() == 1U);

  const auto *payload = wh::core::any_cast<std::string>(&batch->calls.front().payload);
  REQUIRE(payload != nullptr);
  REQUIRE(*payload == "original-request");
}
