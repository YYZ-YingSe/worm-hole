#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "helper/compose_graph_runtime_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/runtime.hpp"
#include "wh/core/any.hpp"

namespace {

template <typename value_t>
[[nodiscard]] auto read_any(const wh::core::any &value) -> wh::core::result<value_t> {
  if (const auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    if constexpr (std::copy_constructible<value_t>) {
      return *typed;
    } else {
      return wh::core::result<value_t>::failure(wh::core::errc::not_supported);
    }
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] auto read_any(wh::core::any &&value) -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] auto any_get(const wh::core::any &value) noexcept -> const value_t * {
  return wh::core::any_cast<value_t>(&value);
}

using wh::testing::helper::capture_exact_checkpoint;
using wh::testing::helper::checkpoint_entry_input;
using wh::testing::helper::find_checkpoint_entry_input;
using wh::testing::helper::invoke_graph_sync;
using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::make_auto_contract_edge_options;
using wh::testing::helper::make_int_add_node;
using wh::testing::helper::make_int_graph_stream;
using wh::testing::helper::set_checkpoint_entry_input;

struct nested_increment_graphs {
  wh::compose::graph child{};
  wh::compose::graph parent{};
  std::string child_name{};
  std::string child_path_key{};
  std::string parent_name{};
};

[[nodiscard]] auto make_nested_increment_graphs() -> nested_increment_graphs {
  wh::compose::graph_compile_options child_options{};
  child_options.name = "child_graph";
  wh::compose::graph child{std::move(child_options)};
  REQUIRE(child
              .add_lambda("leaf",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(child.add_entry_edge("leaf").has_value());
  REQUIRE(child.add_exit_edge("leaf").has_value());
  REQUIRE(child.compile().has_value());

  wh::compose::graph_compile_options parent_options{};
  parent_options.name = "parent_graph";
  wh::compose::graph parent{std::move(parent_options)};
  REQUIRE(parent.add_subgraph(wh::compose::make_subgraph_node("child", child)).has_value());
  REQUIRE(parent.add_entry_edge("child").has_value());
  REQUIRE(parent.add_exit_edge("child").has_value());
  REQUIRE(parent.compile().has_value());

  return nested_increment_graphs{
      .child = std::move(child),
      .parent = std::move(parent),
      .child_name = "child_graph",
      .child_path_key = "child",
      .parent_name = "parent_graph",
  };
}

struct deep_nested_increment_graphs {
  wh::compose::graph leaf{};
  wh::compose::graph middle{};
  wh::compose::graph parent{};
  std::string leaf_name{};
  std::string middle_name{};
  std::string middle_path_key{};
  std::string leaf_path_key{};
  std::string parent_name{};
};

[[nodiscard]] auto make_deep_nested_increment_graphs() -> deep_nested_increment_graphs {
  wh::compose::graph_compile_options leaf_options{};
  leaf_options.name = "leaf_graph";
  wh::compose::graph leaf{std::move(leaf_options)};
  REQUIRE(leaf.add_lambda("inc",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(leaf.add_entry_edge("inc").has_value());
  REQUIRE(leaf.add_exit_edge("inc").has_value());
  REQUIRE(leaf.compile().has_value());

  wh::compose::graph_compile_options middle_options{};
  middle_options.name = "middle_graph";
  wh::compose::graph middle{std::move(middle_options)};
  REQUIRE(middle.add_subgraph(wh::compose::make_subgraph_node("leaf", leaf)).has_value());
  REQUIRE(middle.add_entry_edge("leaf").has_value());
  REQUIRE(middle.add_exit_edge("leaf").has_value());
  REQUIRE(middle.compile().has_value());

  wh::compose::graph_compile_options parent_options{};
  parent_options.name = "parent_graph";
  wh::compose::graph parent{std::move(parent_options)};
  REQUIRE(parent.add_subgraph(wh::compose::make_subgraph_node("middle", middle)).has_value());
  REQUIRE(parent.add_entry_edge("middle").has_value());
  REQUIRE(parent.add_exit_edge("middle").has_value());
  REQUIRE(parent.compile().has_value());

  return deep_nested_increment_graphs{
      .leaf = std::move(leaf),
      .middle = std::move(middle),
      .parent = std::move(parent),
      .leaf_name = "leaf_graph",
      .middle_name = "middle_graph",
      .middle_path_key = "middle",
      .leaf_path_key = "middle/leaf",
      .parent_name = "parent_graph",
  };
}

} // namespace

TEST_CASE("compose checkpoint store supports staged commit and restore controls",
          "[core][compose][checkpoint][condition]") {
  wh::compose::checkpoint_store store{};

  wh::compose::checkpoint_state staged_state{};
  staged_state.branch = "main";
  set_checkpoint_entry_input(staged_state, 101);

  wh::compose::checkpoint_save_options staged_options{};
  staged_options.checkpoint_id = "write-A";
  staged_options.thread_key = "thread-A";
  staged_options.namespace_key = "ns-A";
  staged_options.branch = "main";
  REQUIRE(store.stage_write(staged_state, staged_options).has_value());

  wh::compose::checkpoint_load_options before_commit{};
  before_commit.checkpoint_id = "write-A";
  auto before = store.load(before_commit);
  REQUIRE(before.has_error());
  REQUIRE(before.error() == wh::core::errc::not_found);

  REQUIRE(store.commit_staged("write-A").has_value());
  auto loaded = store.load(before_commit);
  REQUIRE(loaded.has_value());
  auto loaded_start = checkpoint_entry_input(loaded.value());
  REQUIRE(loaded_start.has_value());
  REQUIRE(loaded_start.value() == 101);

  auto thread_id = store.latest_thread_checkpoint_id("thread-A");
  REQUIRE(thread_id.has_value());
  REQUIRE(thread_id.value() == "write-A");
  auto namespace_id = store.latest_namespace_checkpoint_id("ns-A");
  REQUIRE(namespace_id.has_value());
  REQUIRE(namespace_id.value() == "write-A");

  wh::compose::checkpoint_load_options force_new_run{};
  force_new_run.force_new_run = true;
  auto restore_plan = store.prepare_restore(force_new_run);
  REQUIRE(restore_plan.has_value());
  REQUIRE(!restore_plan.value().restore_from_checkpoint);
  REQUIRE(!restore_plan.value().checkpoint.has_value());

  wh::compose::checkpoint_state branch_state{};
  branch_state.branch = "branch-B";
  set_checkpoint_entry_input(branch_state, 102);
  wh::compose::checkpoint_save_options branch_write{};
  branch_write.checkpoint_id = "write-A";
  branch_write.branch = "branch-B";
  REQUIRE(store.save(branch_state, std::move(branch_write)).has_value());

  wh::compose::checkpoint_load_options branch_load{};
  branch_load.checkpoint_id = "write-A";
  branch_load.branch = std::string{"branch-B"};
  auto branch_loaded = store.load(branch_load);
  REQUIRE(branch_loaded.has_value());
  auto branch_start = checkpoint_entry_input(branch_loaded.value());
  REQUIRE(branch_start.has_value());
  REQUIRE(branch_start.value() == 102);

  auto history = store.history("write-A");
  REQUIRE(history.has_value());
  REQUIRE(history.value().get().size() == 2U);

  wh::compose::checkpoint_retention_policy retention{};
  retention.max_records_per_checkpoint_id = 1U;
  const auto pruned = store.prune(retention);
  REQUIRE(pruned.removed_total() == 1U);
  REQUIRE(pruned.removed_committed_record_ids.size() == 1U);
  REQUIRE(pruned.removed_pending_record_ids.empty());
  auto history_after_prune = store.history("write-A");
  REQUIRE(history_after_prune.has_value());
  REQUIRE(history_after_prune.value().get().size() == 1U);
}

TEST_CASE("compose checkpoint store prune supports layered capacity controls",
          "[core][compose][checkpoint][condition]") {
  wh::compose::checkpoint_store store{};

  for (int index = 1; index <= 3; ++index) {
    wh::compose::checkpoint_state state{};
    set_checkpoint_entry_input(state, index);
    wh::compose::checkpoint_save_options options{};
    options.checkpoint_id = "checkpoint-" + std::to_string(index);
    options.thread_key = "thread-" + std::to_string(index);
    options.namespace_key = "namespace-" + std::to_string(index);
    REQUIRE(store.save(std::move(state), std::move(options)).has_value());
  }

  const auto base = std::chrono::system_clock::now();
  {
    wh::compose::checkpoint_state state{};
    set_checkpoint_entry_input(state, 11);
    wh::compose::checkpoint_save_options options{};
    options.checkpoint_id = "pending-1";
    options.staged_at = base + std::chrono::seconds{1};
    REQUIRE(store.stage_write(std::move(state), std::move(options)).has_value());
  }
  {
    wh::compose::checkpoint_state state{};
    set_checkpoint_entry_input(state, 12);
    wh::compose::checkpoint_save_options options{};
    options.checkpoint_id = "pending-2";
    options.staged_at = base + std::chrono::seconds{2};
    REQUIRE(store.stage_write(std::move(state), std::move(options)).has_value());
  }

  wh::compose::checkpoint_retention_policy policy{};
  policy.max_pending_writes = 1U;
  policy.max_thread_index_entries = 1U;
  policy.max_namespace_index_entries = 1U;
  const auto removed = store.prune(policy);
  REQUIRE(removed.removed_total() == 5U);
  REQUIRE(removed.removed_committed_record_ids.empty());
  REQUIRE(removed.removed_pending_record_ids.size() == 1U);
  REQUIRE(removed.removed_thread_index_entries == 2U);
  REQUIRE(removed.removed_namespace_index_entries == 2U);

  std::size_t live_thread_entries = 0U;
  for (const std::string key : {"thread-1", "thread-2", "thread-3"}) {
    if (store.latest_thread_checkpoint_id(key).has_value()) {
      ++live_thread_entries;
    }
  }
  REQUIRE(live_thread_entries == 1U);

  std::size_t live_namespace_entries = 0U;
  for (const std::string key : {"namespace-1", "namespace-2", "namespace-3"}) {
    if (store.latest_namespace_checkpoint_id(key).has_value()) {
      ++live_namespace_entries;
    }
  }
  REQUIRE(live_namespace_entries == 1U);

  wh::compose::checkpoint_load_options dropped_pending{};
  dropped_pending.checkpoint_id = "pending-1";
  dropped_pending.include_pending = true;
  auto dropped_plan = store.prepare_restore(dropped_pending);
  REQUIRE(dropped_plan.has_error());
  REQUIRE(dropped_plan.error() == wh::core::errc::not_found);

  wh::compose::checkpoint_load_options kept_pending{};
  kept_pending.checkpoint_id = "pending-2";
  kept_pending.include_pending = true;
  auto kept_plan = store.prepare_restore(kept_pending);
  REQUIRE(kept_plan.has_value());
  REQUIRE(kept_plan.value().restore_from_checkpoint);
  REQUIRE(kept_plan.value().checkpoint.has_value());
  auto kept_start = checkpoint_entry_input(*kept_plan.value().checkpoint);
  REQUIRE(kept_start.has_value());
  REQUIRE(kept_start.value() == 12);
}

TEST_CASE("compose checkpoint store prepare_restore can replay pending writes",
          "[core][compose][checkpoint][condition]") {
  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options options{};
  options.checkpoint_id = "replay-id";

  wh::compose::checkpoint_state committed{};
  set_checkpoint_entry_input(committed, 10);
  REQUIRE(store.save(committed, options).has_value());

  wh::compose::checkpoint_state staged{};
  set_checkpoint_entry_input(staged, 11);
  REQUIRE(store.stage_write(staged, options).has_value());

  wh::compose::checkpoint_load_options replay{};
  replay.checkpoint_id = "replay-id";
  replay.include_pending = true;
  auto replay_plan = store.prepare_restore(replay);
  REQUIRE(replay_plan.has_value());
  REQUIRE(replay_plan.value().restore_from_checkpoint);
  REQUIRE(replay_plan.value().checkpoint.has_value());
  auto replay_start = checkpoint_entry_input(*replay_plan.value().checkpoint);
  REQUIRE(replay_start.has_value());
  REQUIRE(replay_start.value() == 11);

  wh::compose::checkpoint_load_options committed_only{};
  committed_only.checkpoint_id = "replay-id";
  committed_only.include_pending = false;
  auto committed_plan = store.prepare_restore(committed_only);
  REQUIRE(committed_plan.has_value());
  REQUIRE(committed_plan.value().restore_from_checkpoint);
  REQUIRE(committed_plan.value().checkpoint.has_value());
  auto committed_start = checkpoint_entry_input(*committed_plan.value().checkpoint);
  REQUIRE(committed_start.has_value());
  REQUIRE(committed_start.value() == 10);
}

TEST_CASE("compose graph runtime supports pluggable checkpoint backend callbacks",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_lambda(make_int_add_node("worker", 1)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  std::size_t prepare_calls = 0U;
  std::size_t save_calls = 0U;
  std::optional<int> saved_start_input{};
  wh::compose::checkpoint_backend backend{};
  backend.prepare_restore = wh::compose::checkpoint_backend_prepare_restore{
      [&graph, &prepare_calls](
          const wh::compose::checkpoint_load_options &,
          wh::core::run_context &) -> wh::core::result<wh::compose::checkpoint_restore_plan> {
        ++prepare_calls;
        wh::core::run_context capture_context{};
        auto checkpoint = capture_exact_checkpoint(graph, wh::core::any(5), capture_context);
        if (checkpoint.has_error()) {
          return wh::core::result<wh::compose::checkpoint_restore_plan>::failure(
              checkpoint.error());
        }
        return wh::compose::checkpoint_restore_plan{
            .restore_from_checkpoint = true,
            .checkpoint = std::move(checkpoint).value(),
        };
      }};
  backend.save = wh::compose::checkpoint_backend_save{
      [&save_calls, &saved_start_input](wh::compose::checkpoint_state &&checkpoint,
                                        wh::compose::checkpoint_save_options &&,
                                        wh::core::run_context &) -> wh::core::result<void> {
        ++save_calls;
        auto typed = checkpoint_entry_input(checkpoint);
        if (typed.has_value()) {
          saved_start_input = typed.value();
        }
        return {};
      }};

  wh::compose::graph_runtime_services services{};
  services.checkpoint.backend = std::addressof(backend);
  wh::compose::graph_invoke_controls controls{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "backend-id";
  controls.checkpoint.save = write_options;
  wh::core::run_context context{};
  auto invoked = invoke_graph_sync(graph, wh::core::any(std::monostate{}), context, controls,
                                   std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 6);
  REQUIRE(prepare_calls == 1U);
  REQUIRE(save_calls == 1U);
  REQUIRE(saved_start_input.has_value());
  REQUIRE(saved_start_input.value() == 5);
}

TEST_CASE("compose graph checkpoint serializer falls back to default and supports custom hooks",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  SECTION("default serializer path persists checkpoint when no custom serializer provided") {
    wh::compose::checkpoint_store store{};
    wh::compose::checkpoint_save_options write_options{};
    write_options.checkpoint_id = "serializer-default";

    wh::compose::graph_runtime_services services{};
    services.checkpoint.store = std::addressof(store);
    wh::compose::graph_invoke_controls controls{};
    controls.checkpoint.save = write_options;
    wh::core::run_context context{};
    auto invoked =
        invoke_graph_sync(graph, wh::core::any(4), context, controls, std::addressof(services));
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_value());

    auto persisted = store.load(
        wh::compose::checkpoint_load_options{.checkpoint_id = std::string{"serializer-default"}});
    REQUIRE(persisted.has_value());
    auto start_input = checkpoint_entry_input(persisted.value());
    REQUIRE(start_input.has_value());
    REQUIRE(start_input.value() == 4);
  }

  SECTION("custom serializer encode/decode hooks are used on persist path") {
    wh::compose::checkpoint_store store{};
    wh::compose::checkpoint_save_options write_options{};
    write_options.checkpoint_id = "serializer-custom";
    bool encode_called = false;
    bool decode_called = false;

    const wh::compose::checkpoint_serializer serializer{
        .encode =
            wh::compose::checkpoint_serializer_encode{
                [&encode_called](wh::compose::checkpoint_state &&state, wh::core::run_context &)
                    -> wh::core::result<wh::compose::graph_value> {
                  encode_called = true;
                  set_checkpoint_entry_input(state, 77);
                  return wh::core::any(std::move(state));
                }},
        .decode =
            wh::compose::checkpoint_serializer_decode{
                [&decode_called](wh::compose::graph_value &&payload, wh::core::run_context &)
                    -> wh::core::result<wh::compose::checkpoint_state> {
                  decode_called = true;
                  return read_any<wh::compose::checkpoint_state>(std::move(payload));
                }},
    };
    wh::compose::graph_runtime_services services{};
    services.checkpoint.store = std::addressof(store);
    services.checkpoint.serializer = std::addressof(serializer);
    wh::compose::graph_invoke_controls controls{};
    controls.checkpoint.save = write_options;
    wh::core::run_context context{};
    auto invoked =
        invoke_graph_sync(graph, wh::core::any(6), context, controls, std::addressof(services));
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_value());
    REQUIRE(encode_called);
    REQUIRE(decode_called);

    auto persisted = store.load(
        wh::compose::checkpoint_load_options{.checkpoint_id = std::string{"serializer-custom"}});
    REQUIRE(persisted.has_value());
    auto persisted_start = checkpoint_entry_input(persisted.value());
    REQUIRE(persisted_start.has_value());
    REQUIRE(persisted_start.value() == 77);
  }
}

TEST_CASE("compose graph checkpoint serializer validates session payload shape",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "serializer-error";
  wh::compose::checkpoint_state seed{};
  seed.checkpoint_id = "serializer-error";
  seed.restore_shape = graph.restore_shape();
  set_checkpoint_entry_input(seed, 5);
  REQUIRE(store
              .save(wh::compose::checkpoint_state{seed},
                    wh::compose::checkpoint_save_options{write_options})
              .has_value());

  SECTION("type mismatch serializer payload fails restore") {
    const wh::compose::checkpoint_serializer serializer{
        .encode =
            wh::compose::checkpoint_serializer_encode{
                [](wh::compose::checkpoint_state &&, wh::core::run_context &)
                    -> wh::core::result<wh::compose::graph_value> { return wh::core::any(7); }},
        .decode =
            wh::compose::checkpoint_serializer_decode{
                [](wh::compose::graph_value &&payload,
                   wh::core::run_context &) -> wh::core::result<wh::compose::checkpoint_state> {
                  return read_any<wh::compose::checkpoint_state>(std::move(payload));
                }},
    };
    wh::compose::graph_runtime_services services{};
    services.checkpoint.store = std::addressof(store);
    services.checkpoint.serializer = std::addressof(serializer);
    wh::compose::graph_invoke_controls controls{};
    controls.checkpoint.load =
        wh::compose::checkpoint_load_options{.checkpoint_id = std::string{"serializer-error"}};
    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(), context,
                                     controls, std::addressof(services));
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() == wh::core::errc::type_mismatch);
    REQUIRE(invoked.value().report.checkpoint_error.has_value());
    REQUIRE(invoked.value().report.checkpoint_error->code == wh::core::errc::type_mismatch);
    REQUIRE(invoked.value().report.checkpoint_error->operation == "restore_serializer_roundtrip");
  }

  SECTION("incomplete serializer callbacks fail restore") {
    const wh::compose::checkpoint_serializer serializer{
        .encode =
            wh::compose::checkpoint_serializer_encode{
                [](wh::compose::checkpoint_state &&state,
                   wh::core::run_context &) -> wh::core::result<wh::compose::graph_value> {
                  return wh::core::any(std::move(state));
                }},
        .decode = wh::compose::checkpoint_serializer_decode{nullptr},
    };
    wh::compose::graph_runtime_services services{};
    services.checkpoint.store = std::addressof(store);
    services.checkpoint.serializer = std::addressof(serializer);
    wh::compose::graph_invoke_controls controls{};
    controls.checkpoint.load =
        wh::compose::checkpoint_load_options{.checkpoint_id = std::string{"serializer-error"}};
    wh::core::run_context context{};
    auto invoked = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(), context,
                                     controls, std::addressof(services));
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() == wh::core::errc::invalid_argument);
    REQUIRE(invoked.value().report.checkpoint_error.has_value());
    REQUIRE(invoked.value().report.checkpoint_error->code == wh::core::errc::invalid_argument);
    REQUIRE(invoked.value().report.checkpoint_error->operation == "validate_runtime_config");
  }
}

TEST_CASE("compose graph runtime stream checkpoint conversion pair round-trips pending input",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{wh::compose::graph_boundary{
      .input = wh::compose::node_contract::stream, .output = wh::compose::node_contract::stream}};
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::stream>(
                  "worker",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> { return input; })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "stream-cp";

  wh::compose::checkpoint_stream_codec converter_pair{
      .to_value =
          wh::compose::checkpoint_stream_save{
              [](wh::compose::graph_stream_reader &&reader,
                 wh::core::run_context &) -> wh::core::result<wh::compose::graph_value> {
                int sum = 0;
                for (;;) {
                  auto chunk = reader.read();
                  if (chunk.has_error()) {
                    return wh::core::result<wh::compose::graph_value>::failure(chunk.error());
                  }
                  if (chunk.value().eof) {
                    break;
                  }
                  if (!chunk.value().value.has_value()) {
                    continue;
                  }
                  auto typed = read_any<int>(*chunk.value().value);
                  if (typed.has_error()) {
                    return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                  }
                  sum += typed.value();
                }
                return wh::core::any(sum);
              }},
      .to_stream =
          wh::compose::checkpoint_stream_load{
              [](const wh::compose::graph_value &value,
                 wh::core::run_context &) -> wh::core::result<wh::compose::graph_stream_reader> {
                auto typed = read_any<int>(value);
                if (typed.has_error()) {
                  return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
                }
                auto single = wh::compose::make_single_value_stream_reader(typed.value());
                if (single.has_error()) {
                  return wh::core::result<wh::compose::graph_stream_reader>::failure(
                      single.error());
                }
                return std::move(single).value();
              }},
  };
  wh::compose::checkpoint_stream_codecs registry{};
  registry.emplace(std::string{wh::compose::graph_start_node_key}, converter_pair);
  registry.emplace(std::string{"worker"}, std::move(converter_pair));
  registry.emplace(
      std::string{wh::compose::graph_end_node_key},
      wh::compose::checkpoint_stream_codec{
          .to_value =
              wh::compose::checkpoint_stream_save{
                  [](wh::compose::graph_stream_reader &&reader,
                     wh::core::run_context &) -> wh::core::result<wh::compose::graph_value> {
                    int sum = 0;
                    for (;;) {
                      auto chunk = reader.read();
                      if (chunk.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(chunk.error());
                      }
                      if (chunk.value().eof) {
                        break;
                      }
                      if (!chunk.value().value.has_value()) {
                        continue;
                      }
                      auto typed = read_any<int>(*chunk.value().value);
                      if (typed.has_error()) {
                        return wh::core::result<wh::compose::graph_value>::failure(typed.error());
                      }
                      sum += typed.value();
                    }
                    return wh::core::any(sum);
                  }},
          .to_stream =
              wh::compose::checkpoint_stream_load{
                  [](const wh::compose::graph_value &value, wh::core::run_context &)
                      -> wh::core::result<wh::compose::graph_stream_reader> {
                    auto typed = read_any<int>(value);
                    if (typed.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          typed.error());
                    }
                    auto single = wh::compose::make_single_value_stream_reader(typed.value());
                    if (single.has_error()) {
                      return wh::core::result<wh::compose::graph_stream_reader>::failure(
                          single.error());
                    }
                    return std::move(single).value();
                  }},
      });

  wh::compose::graph_runtime_services first_services{};
  first_services.checkpoint.store = std::addressof(store);
  first_services.checkpoint.stream_codecs = std::addressof(registry);
  wh::compose::graph_invoke_controls first_controls{};
  first_controls.checkpoint.save = write_options;
  wh::core::run_context first_context{};

  auto [writer, reader] = wh::compose::make_graph_stream();
  REQUIRE(writer.try_write(wh::core::any(2)).has_value());
  REQUIRE(writer.try_write(wh::core::any(3)).has_value());
  REQUIRE(writer.close().has_value());
  auto first = invoke_graph_sync(graph, wh::core::any(std::move(reader)), first_context,
                                 first_controls, std::addressof(first_services));
  REQUIRE(first.has_value());
  REQUIRE(first.value().output_status.has_value());

  wh::compose::checkpoint_load_options load_options{};
  load_options.checkpoint_id = "stream-cp";
  auto persisted = store.load(load_options);
  REQUIRE(persisted.has_value());
  const auto *entry_payload = find_checkpoint_entry_input(persisted.value());
  REQUIRE(entry_payload != nullptr);
  const auto *stream_payload =
      any_get<wh::compose::checkpoint_stream_value_payload>(*entry_payload);
  REQUIRE(stream_payload != nullptr);
  auto persisted_sum = read_any<int>(stream_payload->value);
  REQUIRE(persisted_sum.has_value());
  REQUIRE(persisted_sum.value() == 5);

  wh::compose::forwarded_checkpoint_map forwarded{};
  forwarded.emplace(std::string{"graph"}, persisted.value());
  wh::compose::graph_runtime_services second_services{};
  second_services.checkpoint.stream_codecs = std::addressof(registry);
  wh::compose::graph_invoke_controls second_controls{};
  second_controls.checkpoint.forwarded_once = std::move(forwarded);
  wh::core::run_context second_context{};
  auto resumed =
      invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(), second_context,
                        second_controls, std::addressof(second_services));
  REQUIRE(resumed.has_value());
  REQUIRE(resumed.value().output_status.has_value());
  auto resumed_stream =
      read_any<wh::compose::graph_stream_reader>(std::move(resumed).value().output_status.value());
  REQUIRE(resumed_stream.has_value());
  auto first_chunk = resumed_stream.value().read();
  REQUIRE(first_chunk.has_value());
  REQUIRE(first_chunk.value().value.has_value());
  auto restored_value = read_any<int>(*first_chunk.value().value);
  REQUIRE(restored_value.has_value());
  REQUIRE(restored_value.value() == 5);
}

TEST_CASE("compose graph runtime records stream-converter missing pair as structured error",
          "[core][compose][checkpoint][branch]") {
  wh::compose::graph graph{wh::compose::graph_boundary{
      .input = wh::compose::node_contract::stream, .output = wh::compose::node_contract::stream}};
  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::stream>(
                  "worker",
                  [](wh::compose::graph_stream_reader input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_stream_reader> { return input; })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "stream-missing";

  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.save = write_options;
  wh::core::run_context context{};

  auto [writer, reader] = wh::compose::make_graph_stream();
  REQUIRE(writer.try_write(wh::core::any(1)).has_value());
  REQUIRE(writer.close().has_value());
  auto invoked = invoke_graph_sync(graph, wh::core::any(std::move(reader)), context, controls,
                                   std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  REQUIRE(invoked.value().report.checkpoint_error.has_value());
  REQUIRE(invoked.value().report.checkpoint_error->code == wh::core::errc::not_supported);
  REQUIRE(invoked.value().report.checkpoint_error->operation == "persist_stream_convert");
}

TEST_CASE("compose graph runtime applies checkpoint NodePath state modifiers",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_lambda(make_int_add_node("worker", 1)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "path-mod";

  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.save = write_options;
  controls.checkpoint.before_save_nodes =
      wh::compose::checkpoint_node_hooks{wh::compose::checkpoint_node_hook{
          .path = wh::compose::make_node_path({"worker"}),
          .include_descendants = false,
          .modifier =
              wh::compose::checkpoint_node_modifier{
                  [](wh::compose::graph_node_state &state,
                     wh::core::run_context &) -> wh::core::result<void> {
                    state.attempts = 42U;
                    return {};
                  }},
      }};
  wh::core::run_context context{};

  auto invoked =
      invoke_graph_sync(graph, wh::core::any(3), context, controls, std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto persisted =
      store.load(wh::compose::checkpoint_load_options{.checkpoint_id = std::string{"path-mod"}});
  REQUIRE(persisted.has_value());
  const auto iter = std::find_if(
      persisted.value().runtime.lifecycle.begin(), persisted.value().runtime.lifecycle.end(),
      [](const wh::compose::graph_node_state &state) { return state.key == "worker"; });
  REQUIRE(iter != persisted.value().runtime.lifecycle.end());
  REQUIRE(iter->attempts == 42U);
}

TEST_CASE("compose graph runtime applies resume-data state overrides before invoke",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph.add_lambda(make_int_add_node("worker", 1)).has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "resume-override";

  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.save = write_options;
  wh::core::run_context context{};
  context.resume_info.emplace();
  auto worker_id = graph.node_id("worker");
  REQUIRE(worker_id.has_value());
  wh::compose::graph_node_state overridden{};
  overridden.key = "worker";
  overridden.node_id = worker_id.value();
  overridden.lifecycle = wh::compose::graph_node_lifecycle_state::failed;
  overridden.attempts = 7U;
  overridden.last_error = wh::core::errc::unavailable;
  REQUIRE(context.resume_info
              ->upsert("override-worker", wh::core::make_address({"graph", "worker"}),
                       wh::core::any{overridden})
              .has_value());

  wh::compose::graph_call_options invalid_call_options{};
  invalid_call_options.pregel_max_steps = static_cast<std::size_t>(1U);
  auto invoked = invoke_graph_sync(graph, wh::core::any(1), context, invalid_call_options,
                                   std::addressof(services), controls);
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_error());
  REQUIRE(invoked.value().output_status.error() == wh::core::errc::contract_violation);

  auto persisted = store.load(
      wh::compose::checkpoint_load_options{.checkpoint_id = std::string{"resume-override"}});
  REQUIRE(persisted.has_value());
  const auto iter = std::find_if(
      persisted.value().runtime.lifecycle.begin(), persisted.value().runtime.lifecycle.end(),
      [](const wh::compose::graph_node_state &state) { return state.key == "worker"; });
  REQUIRE(iter != persisted.value().runtime.lifecycle.end());
  REQUIRE(iter->attempts == 7U);
  REQUIRE(iter->lifecycle == wh::compose::graph_node_lifecycle_state::failed);
}

TEST_CASE("compose graph runtime checkpoint restore reloads pending input",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("inc",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("inc").has_value());
  REQUIRE(graph.add_exit_edge("inc").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "runtime-cp";
  wh::core::run_context capture_context{};
  auto checkpoint = capture_exact_checkpoint(graph, wh::core::any(10), capture_context);
  REQUIRE(checkpoint.has_value());
  checkpoint->checkpoint_id = "runtime-cp";
  REQUIRE(store.save(std::move(checkpoint).value(), write_options).has_value());

  wh::compose::checkpoint_load_options load_options{};
  load_options.checkpoint_id = "runtime-cp";
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls resume_controls{};
  resume_controls.checkpoint.load = load_options;
  resume_controls.checkpoint.save = write_options;
  wh::core::run_context resume_context{};
  auto resumed = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(),
                                   resume_context, resume_controls, std::addressof(services));
  REQUIRE(resumed.has_value());
  REQUIRE(resumed.value().output_status.has_value());
  auto resumed_typed = read_any<int>(resumed.value().output_status.value());
  REQUIRE(resumed_typed.has_value());
  REQUIRE(resumed_typed.value() == 11);
}

TEST_CASE("compose graph checkpoint restore can skip state pre-handlers for restored nodes",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("worker",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  auto make_handlers = [] {
    wh::compose::graph_state_handler_registry handlers{};
    handlers.emplace(
        "worker", wh::compose::graph_node_state_handlers{
                      .pre = [](const wh::compose::graph_state_cause &,
                                wh::compose::graph_process_state &, wh::compose::graph_value &input,
                                wh::core::run_context &) -> wh::core::result<void> {
                        auto typed = read_any<int>(input);
                        if (typed.has_error()) {
                          return wh::core::result<void>::failure(typed.error());
                        }
                        input = wh::core::any(typed.value() + 10);
                        return {};
                      },
                  });
    return handlers;
  };

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "skip-pre";
  wh::core::run_context checkpoint_context{};
  auto checkpoint = capture_exact_checkpoint(graph, wh::core::any(1), checkpoint_context);
  REQUIRE(checkpoint.has_value());
  checkpoint->checkpoint_id = "skip-pre";
  REQUIRE(store.save(std::move(checkpoint).value(), write_options).has_value());

  wh::compose::checkpoint_load_options no_skip{};
  no_skip.checkpoint_id = "skip-pre";
  no_skip.skip_pre_handlers = false;
  wh::compose::graph_runtime_services no_skip_services{};
  no_skip_services.checkpoint.store = std::addressof(store);
  auto no_skip_handlers = make_handlers();
  no_skip_services.state_handlers = std::addressof(no_skip_handlers);
  wh::compose::graph_invoke_controls no_skip_controls{};
  no_skip_controls.checkpoint.load = no_skip;
  wh::core::run_context no_skip_context{};
  auto resumed_no_skip =
      invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(), no_skip_context,
                        no_skip_controls, std::addressof(no_skip_services));
  REQUIRE(resumed_no_skip.has_value());
  REQUIRE(resumed_no_skip.value().output_status.has_value());
  auto resumed_no_skip_typed = read_any<int>(resumed_no_skip.value().output_status.value());
  REQUIRE(resumed_no_skip_typed.has_value());
  REQUIRE(resumed_no_skip_typed.value() == 12);

  wh::compose::checkpoint_load_options skip{};
  skip.checkpoint_id = "skip-pre";
  skip.skip_pre_handlers = true;
  wh::compose::graph_runtime_services skip_services{};
  skip_services.checkpoint.store = std::addressof(store);
  auto skip_handlers = make_handlers();
  skip_services.state_handlers = std::addressof(skip_handlers);
  wh::compose::graph_invoke_controls skip_controls{};
  skip_controls.checkpoint.load = skip;
  wh::core::run_context skip_context{};
  auto resumed_skip = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(),
                                        skip_context, skip_controls, std::addressof(skip_services));
  REQUIRE(resumed_skip.has_value());
  REQUIRE(resumed_skip.value().output_status.has_value());
  auto resumed_skip_typed = read_any<int>(resumed_skip.value().output_status.value());
  REQUIRE(resumed_skip_typed.has_value());
  REQUIRE(resumed_skip_typed.value() == 2);
}

TEST_CASE("compose graph runtime consumes forwarded checkpoint before store and only once",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("inc",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("inc").has_value());
  REQUIRE(graph.add_exit_edge("inc").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::core::run_context stored_capture_context{};
  auto stored_checkpoint =
      capture_exact_checkpoint(graph, wh::core::any(1), stored_capture_context);
  REQUIRE(stored_checkpoint.has_value());
  stored_checkpoint->checkpoint_id = "store-precedence";
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "store-precedence";
  REQUIRE(store.save(std::move(stored_checkpoint).value(), write_options).has_value());

  wh::core::run_context forwarded_capture_context{};
  auto forwarded_checkpoint =
      capture_exact_checkpoint(graph, wh::core::any(4), forwarded_capture_context);
  REQUIRE(forwarded_checkpoint.has_value());
  forwarded_checkpoint->checkpoint_id = "forwarded-precedence";
  wh::compose::forwarded_checkpoint_map forwarded{};
  forwarded.emplace("graph", std::move(forwarded_checkpoint).value());

  wh::core::run_context context{};
  wh::compose::checkpoint_load_options load_options{};
  load_options.checkpoint_id = "store-precedence";
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.load = load_options;
  controls.checkpoint.forwarded_once = std::move(forwarded);

  auto first = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(), context,
                                 std::move(controls), std::addressof(services));
  REQUIRE(first.has_value());
  REQUIRE(first.value().output_status.has_value());
  auto first_typed = read_any<int>(first.value().output_status.value());
  REQUIRE(first_typed.has_value());
  REQUIRE(first_typed.value() == 5);
  REQUIRE(first.value().report.remaining_forwarded_checkpoint_keys.empty());

  wh::compose::graph_invoke_controls second_controls{};
  second_controls.checkpoint.load = load_options;
  auto second = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(), context,
                                  std::move(second_controls), std::addressof(services));
  REQUIRE(second.has_value());
  REQUIRE(second.value().output_status.has_value());
  auto second_typed = read_any<int>(second.value().output_status.value());
  REQUIRE(second_typed.has_value());
  REQUIRE(second_typed.value() == 2);
  REQUIRE(second.value().report.remaining_forwarded_checkpoint_keys.empty());
}

TEST_CASE("compose nested subgraph restore consumes forwarded child checkpoint only",
          "[core][compose][checkpoint][subgraph]") {
  auto graphs = make_nested_increment_graphs();
  REQUIRE(graphs.child.snapshot().compile_options.name == graphs.child_name);
  REQUIRE(graphs.parent.options().name == graphs.parent_name);

  auto compiled_child = graphs.parent.compiled_node_by_key("child");
  REQUIRE(compiled_child.has_value());
  REQUIRE(compiled_child.value().get().meta.subgraph_snapshot.has_value());
  REQUIRE(compiled_child.value().get().meta.subgraph_snapshot->compile_options.name ==
          graphs.child_name);
  const auto parent_snapshot = graphs.parent.snapshot();
  const auto child_snapshot_iter = parent_snapshot.subgraphs.find("child");
  REQUIRE(child_snapshot_iter != parent_snapshot.subgraphs.end());
  REQUIRE(child_snapshot_iter->second.compile_options.name == graphs.child_name);

  std::size_t prepare_calls = 0U;
  wh::compose::checkpoint_backend backend{};
  backend.prepare_restore = wh::compose::checkpoint_backend_prepare_restore{
      [&prepare_calls](const wh::compose::checkpoint_load_options &, wh::core::run_context &)
          -> wh::core::result<wh::compose::checkpoint_restore_plan> {
        ++prepare_calls;
        return wh::compose::checkpoint_restore_plan{
            .restore_from_checkpoint = false,
            .checkpoint = std::nullopt,
        };
      }};
  backend.save = wh::compose::checkpoint_backend_save{
      [](wh::compose::checkpoint_state &&, wh::compose::checkpoint_save_options &&,
         wh::core::run_context &) -> wh::core::result<void> { return {}; }};

  wh::core::run_context child_capture_context{};
  auto child_checkpoint =
      capture_exact_checkpoint(graphs.child, wh::core::any(7), child_capture_context);
  REQUIRE(child_checkpoint.has_value());
  child_checkpoint->checkpoint_id = "child-forwarded";

  wh::compose::forwarded_checkpoint_map forwarded{};
  forwarded.emplace(graphs.child_path_key, std::move(child_checkpoint).value());

  wh::core::run_context context{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.backend = std::addressof(backend);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.forwarded_once = std::move(forwarded);
  auto invoked = invoke_graph_sync(graphs.parent, wh::compose::graph_input::restore_checkpoint(),
                                   context, controls, std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 8);
  REQUIRE(prepare_calls == 1U);
  REQUIRE(invoked.value().report.remaining_forwarded_checkpoint_keys.empty());
}

TEST_CASE("compose nested subgraph restore does not fall back to backend without forwarded child "
          "checkpoint",
          "[core][compose][checkpoint][subgraph]") {
  auto graphs = make_nested_increment_graphs();

  std::size_t prepare_calls = 0U;
  wh::compose::checkpoint_backend backend{};
  backend.prepare_restore = wh::compose::checkpoint_backend_prepare_restore{
      [&prepare_calls](const wh::compose::checkpoint_load_options &, wh::core::run_context &)
          -> wh::core::result<wh::compose::checkpoint_restore_plan> {
        ++prepare_calls;
        return wh::compose::checkpoint_restore_plan{
            .restore_from_checkpoint = false,
            .checkpoint = std::nullopt,
        };
      }};
  backend.save = wh::compose::checkpoint_backend_save{
      [](wh::compose::checkpoint_state &&, wh::compose::checkpoint_save_options &&,
         wh::core::run_context &) -> wh::core::result<void> { return {}; }};

  wh::core::run_context context{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.backend = std::addressof(backend);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.forwarded_once = wh::compose::forwarded_checkpoint_map{};
  auto invoked = invoke_graph_sync(graphs.parent, wh::core::any(1), context, controls,
                                   std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 2);
  REQUIRE(prepare_calls == 1U);
  REQUIRE(invoked.value().report.remaining_forwarded_checkpoint_keys.empty());
}

TEST_CASE("compose nested subgraph force-new-run bypasses backend and forwarded restore",
          "[core][compose][checkpoint][subgraph]") {
  auto graphs = make_nested_increment_graphs();

  std::size_t prepare_calls = 0U;
  wh::compose::checkpoint_backend backend{};
  backend.prepare_restore = wh::compose::checkpoint_backend_prepare_restore{
      [&prepare_calls](const wh::compose::checkpoint_load_options &, wh::core::run_context &)
          -> wh::core::result<wh::compose::checkpoint_restore_plan> {
        ++prepare_calls;
        return wh::compose::checkpoint_restore_plan{
            .restore_from_checkpoint = false,
            .checkpoint = std::nullopt,
        };
      }};
  backend.save = wh::compose::checkpoint_backend_save{
      [](wh::compose::checkpoint_state &&, wh::compose::checkpoint_save_options &&,
         wh::core::run_context &) -> wh::core::result<void> { return {}; }};

  wh::compose::checkpoint_state child_checkpoint{};
  child_checkpoint.checkpoint_id = "child-force-new-run";
  child_checkpoint.restore_shape = graphs.child.restore_shape();
  set_checkpoint_entry_input(child_checkpoint, 7);

  wh::compose::forwarded_checkpoint_map forwarded{};
  forwarded.emplace(graphs.child_path_key, std::move(child_checkpoint));

  wh::core::run_context context{};
  wh::compose::checkpoint_load_options load_options{};
  load_options.force_new_run = true;
  wh::compose::graph_runtime_services services{};
  services.checkpoint.backend = std::addressof(backend);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.load = load_options;
  controls.checkpoint.forwarded_once = std::move(forwarded);
  auto invoked = invoke_graph_sync(graphs.parent, wh::core::any(1), context, controls,
                                   std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 2);
  REQUIRE(prepare_calls == 0U);
  REQUIRE(std::find(invoked.value().report.remaining_forwarded_checkpoint_keys.begin(),
                    invoked.value().report.remaining_forwarded_checkpoint_keys.end(),
                    graphs.child_path_key) !=
          invoked.value().report.remaining_forwarded_checkpoint_keys.end());
}

TEST_CASE("compose deeply nested subgraph restore consumes only matching descendant checkpoints",
          "[core][compose][checkpoint][subgraph]") {
  auto graphs = make_deep_nested_increment_graphs();

  std::size_t prepare_calls = 0U;
  wh::compose::checkpoint_backend backend{};
  backend.prepare_restore = wh::compose::checkpoint_backend_prepare_restore{
      [&prepare_calls](const wh::compose::checkpoint_load_options &, wh::core::run_context &)
          -> wh::core::result<wh::compose::checkpoint_restore_plan> {
        ++prepare_calls;
        return wh::compose::checkpoint_restore_plan{
            .restore_from_checkpoint = false,
            .checkpoint = std::nullopt,
        };
      }};
  backend.save = wh::compose::checkpoint_backend_save{
      [](wh::compose::checkpoint_state &&, wh::compose::checkpoint_save_options &&,
         wh::core::run_context &) -> wh::core::result<void> { return {}; }};

  wh::core::run_context middle_capture_context{};
  auto middle_checkpoint =
      capture_exact_checkpoint(graphs.middle, wh::core::any(7), middle_capture_context);
  REQUIRE(middle_checkpoint.has_value());
  middle_checkpoint->checkpoint_id = "middle-forwarded";

  wh::core::run_context leaf_capture_context{};
  auto leaf_checkpoint =
      capture_exact_checkpoint(graphs.leaf, wh::core::any(7), leaf_capture_context);
  REQUIRE(leaf_checkpoint.has_value());
  leaf_checkpoint->checkpoint_id = "leaf-forwarded";

  wh::core::run_context ghost_capture_context{};
  auto ghost_checkpoint =
      capture_exact_checkpoint(graphs.leaf, wh::core::any(100), ghost_capture_context);
  REQUIRE(ghost_checkpoint.has_value());
  ghost_checkpoint->checkpoint_id = "ghost-forwarded";

  wh::compose::forwarded_checkpoint_map forwarded{};
  forwarded.emplace(graphs.middle_path_key, std::move(middle_checkpoint).value());
  forwarded.emplace(graphs.leaf_path_key, std::move(leaf_checkpoint).value());
  forwarded.emplace("ghost_graph", std::move(ghost_checkpoint).value());

  wh::core::run_context context{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.backend = std::addressof(backend);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.forwarded_once = std::move(forwarded);
  auto invoked = invoke_graph_sync(graphs.parent, wh::compose::graph_input::restore_checkpoint(),
                                   context, controls, std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 8);
  REQUIRE(prepare_calls == 1U);
  const auto &remaining = invoked.value().report.remaining_forwarded_checkpoint_keys;
  REQUIRE(std::find(remaining.begin(), remaining.end(), graphs.middle_path_key) == remaining.end());
  REQUIRE(std::find(remaining.begin(), remaining.end(), graphs.leaf_path_key) == remaining.end());
  REQUIRE(std::find(remaining.begin(), remaining.end(), std::string{"ghost_graph"}) !=
          remaining.end());
  REQUIRE(remaining.size() == 1U);
}

TEST_CASE("compose deeply nested subgraph force-new-run bypasses descendant forwarded checkpoints",
          "[core][compose][checkpoint][subgraph]") {
  auto graphs = make_deep_nested_increment_graphs();

  std::size_t prepare_calls = 0U;
  wh::compose::checkpoint_backend backend{};
  backend.prepare_restore = wh::compose::checkpoint_backend_prepare_restore{
      [&prepare_calls](const wh::compose::checkpoint_load_options &, wh::core::run_context &)
          -> wh::core::result<wh::compose::checkpoint_restore_plan> {
        ++prepare_calls;
        return wh::compose::checkpoint_restore_plan{
            .restore_from_checkpoint = false,
            .checkpoint = std::nullopt,
        };
      }};
  backend.save = wh::compose::checkpoint_backend_save{
      [](wh::compose::checkpoint_state &&, wh::compose::checkpoint_save_options &&,
         wh::core::run_context &) -> wh::core::result<void> { return {}; }};

  wh::core::run_context middle_capture_context{};
  auto middle_checkpoint =
      capture_exact_checkpoint(graphs.middle, wh::core::any(7), middle_capture_context);
  REQUIRE(middle_checkpoint.has_value());
  middle_checkpoint->checkpoint_id = "middle-force-new-run";

  wh::core::run_context leaf_capture_context{};
  auto leaf_checkpoint =
      capture_exact_checkpoint(graphs.leaf, wh::core::any(7), leaf_capture_context);
  REQUIRE(leaf_checkpoint.has_value());
  leaf_checkpoint->checkpoint_id = "leaf-force-new-run";

  wh::compose::forwarded_checkpoint_map forwarded{};
  forwarded.emplace(graphs.middle_path_key, std::move(middle_checkpoint).value());
  forwarded.emplace(graphs.leaf_path_key, std::move(leaf_checkpoint).value());

  wh::core::run_context context{};
  wh::compose::checkpoint_load_options load_options{};
  load_options.force_new_run = true;
  wh::compose::graph_runtime_services services{};
  services.checkpoint.backend = std::addressof(backend);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.load = load_options;
  controls.checkpoint.forwarded_once = std::move(forwarded);
  auto invoked = invoke_graph_sync(graphs.parent, wh::core::any(1), context, controls,
                                   std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 2);
  REQUIRE(prepare_calls == 0U);
  const auto &remaining = invoked.value().report.remaining_forwarded_checkpoint_keys;
  REQUIRE(std::find(remaining.begin(), remaining.end(), graphs.middle_path_key) != remaining.end());
  REQUIRE(std::find(remaining.begin(), remaining.end(), graphs.leaf_path_key) != remaining.end());
  REQUIRE(remaining.size() == 2U);
}

TEST_CASE("compose nested subgraph restore keys forwarded checkpoints by runtime path instead of "
          "graph name",
          "[core][compose][checkpoint][subgraph]") {
  wh::compose::graph_compile_options child_options{};
  child_options.name = "shared_child";
  wh::compose::graph child{std::move(child_options)};
  REQUIRE(child
              .add_lambda("inc",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(child.add_entry_edge("inc").has_value());
  REQUIRE(child.add_exit_edge("inc").has_value());
  REQUIRE(child.compile().has_value());

  wh::compose::graph_compile_options parent_options{};
  parent_options.name = "parent_graph";
  wh::compose::graph parent{std::move(parent_options)};
  REQUIRE(parent.add_subgraph(wh::compose::make_subgraph_node("left", child)).has_value());
  REQUIRE(parent.add_subgraph(wh::compose::make_subgraph_node("right", child)).has_value());
  REQUIRE(parent.add_entry_edge("left").has_value());
  REQUIRE(parent.add_entry_edge("right").has_value());
  REQUIRE(parent.add_exit_edge("right").has_value());
  REQUIRE(parent.add_exit_edge("left").has_value());
  REQUIRE(parent.compile().has_value());

  std::size_t prepare_calls = 0U;
  wh::compose::checkpoint_backend backend{};
  backend.prepare_restore = wh::compose::checkpoint_backend_prepare_restore{
      [&prepare_calls](const wh::compose::checkpoint_load_options &, wh::core::run_context &)
          -> wh::core::result<wh::compose::checkpoint_restore_plan> {
        ++prepare_calls;
        return wh::compose::checkpoint_restore_plan{
            .restore_from_checkpoint = false,
            .checkpoint = std::nullopt,
        };
      }};
  backend.save = wh::compose::checkpoint_backend_save{
      [](wh::compose::checkpoint_state &&, wh::compose::checkpoint_save_options &&,
         wh::core::run_context &) -> wh::core::result<void> { return {}; }};

  wh::core::run_context left_capture_context{};
  auto left_checkpoint = capture_exact_checkpoint(child, wh::core::any(1), left_capture_context);
  REQUIRE(left_checkpoint.has_value());
  left_checkpoint->checkpoint_id = "left-forwarded";

  wh::core::run_context right_capture_context{};
  auto right_checkpoint = capture_exact_checkpoint(child, wh::core::any(7), right_capture_context);
  REQUIRE(right_checkpoint.has_value());
  right_checkpoint->checkpoint_id = "right-forwarded";

  wh::compose::forwarded_checkpoint_map forwarded{};
  forwarded.emplace("left", std::move(left_checkpoint).value());
  forwarded.emplace("right", std::move(right_checkpoint).value());

  wh::core::run_context context{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.backend = std::addressof(backend);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.forwarded_once = std::move(forwarded);
  auto invoked = invoke_graph_sync(parent, wh::compose::graph_input::restore_checkpoint(), context,
                                   controls, std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<wh::compose::graph_value_map>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value().size() == 2U);
  const auto left_iter = typed.value().find("left");
  REQUIRE(left_iter != typed.value().end());
  REQUIRE(read_any<int>(left_iter->second).value() == 2);
  const auto right_iter = typed.value().find("right");
  REQUIRE(right_iter != typed.value().end());
  REQUIRE(read_any<int>(right_iter->second).value() == 8);
  REQUIRE(prepare_calls == 1U);
  REQUIRE(invoked.value().report.remaining_forwarded_checkpoint_keys.empty());
}

TEST_CASE("compose graph runtime force-new-run bypasses forwarded and store restore",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("inc",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("inc").has_value());
  REQUIRE(graph.add_exit_edge("inc").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_state stored_checkpoint{};
  stored_checkpoint.checkpoint_id = "force-new-run";
  stored_checkpoint.restore_shape = graph.restore_shape();
  set_checkpoint_entry_input(stored_checkpoint, 100);
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "force-new-run";
  REQUIRE(store.save(stored_checkpoint, write_options).has_value());

  wh::compose::checkpoint_state forwarded_checkpoint{};
  forwarded_checkpoint.checkpoint_id = "forwarded-force-new-run";
  forwarded_checkpoint.restore_shape = graph.restore_shape();
  set_checkpoint_entry_input(forwarded_checkpoint, 200);
  wh::compose::forwarded_checkpoint_map forwarded{};
  forwarded.emplace("graph", std::move(forwarded_checkpoint));

  wh::core::run_context context{};
  wh::compose::checkpoint_load_options load_options{};
  load_options.checkpoint_id = "force-new-run";
  load_options.force_new_run = true;
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.load = load_options;
  controls.checkpoint.forwarded_once = std::move(forwarded);
  auto invoked =
      invoke_graph_sync(graph, wh::core::any(1), context, controls, std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 2);
  REQUIRE(std::find(invoked.value().report.remaining_forwarded_checkpoint_keys.begin(),
                    invoked.value().report.remaining_forwarded_checkpoint_keys.end(),
                    std::string{"graph"}) !=
          invoked.value().report.remaining_forwarded_checkpoint_keys.end());
}

TEST_CASE("compose graph runtime missing pending input falls back to contract default",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("inc",
                          [](const wh::compose::graph_value &input, wh::core::run_context &,
                             const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            auto typed = read_any<int>(input);
                            if (typed.has_error()) {
                              return wh::core::result<wh::compose::graph_value>::failure(
                                  typed.error());
                            }
                            return wh::core::any(typed.value() + 1);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("inc").has_value());
  REQUIRE(graph.add_exit_edge("inc").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_state checkpoint{};
  checkpoint.checkpoint_id = "missing-rerun-default";
  checkpoint.restore_shape = graph.restore_shape();
  wh::compose::checkpoint_save_options write_options{};
  write_options.checkpoint_id = "missing-rerun-default";
  REQUIRE(store.save(checkpoint, write_options).has_value());

  wh::core::run_context context{};
  wh::compose::checkpoint_load_options load_options{};
  load_options.checkpoint_id = "missing-rerun-default";
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.load = load_options;
  auto invoked = invoke_graph_sync(graph, wh::compose::graph_input::restore_checkpoint(), context,
                                   controls, std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_error());
  REQUIRE(invoked.value().output_status.error() == wh::core::errc::contract_violation);
}

TEST_CASE("compose graph runtime fills missing pending input by stream contract",
          "[core][compose][checkpoint][condition]") {
  wh::compose::graph graph{};
  std::size_t stream_chunk_count = 0U;
  REQUIRE(graph.add_passthrough(wh::compose::make_passthrough_node("source")).has_value());
  REQUIRE(
      graph
          .add_lambda<wh::compose::node_contract::stream, wh::compose::node_contract::value>(
              "recover",
              [&stream_chunk_count](wh::compose::graph_stream_reader input, wh::core::run_context &,
                                    const wh::compose::graph_call_scope &)
                  -> wh::core::result<wh::compose::graph_value> {
                std::size_t chunks = 0U;
                while (true) {
                  auto chunk = input.read();
                  if (chunk.has_error()) {
                    return wh::core::result<wh::compose::graph_value>::failure(chunk.error());
                  }
                  auto item = std::move(chunk).value();
                  if (item.error != wh::core::errc::ok) {
                    return wh::core::result<wh::compose::graph_value>::failure(item.error);
                  }
                  if (item.eof) {
                    break;
                  }
                  if (item.value.has_value()) {
                    ++chunks;
                  }
                }
                stream_chunk_count = chunks;
                return wh::core::any(static_cast<int>(stream_chunk_count));
              },
              wh::compose::graph_add_node_options{
                  .allow_no_control = true,
              })
          .has_value());
  auto recover_options = make_auto_contract_edge_options();
  recover_options.no_control = true;
  REQUIRE(graph.add_edge("source", "recover", std::move(recover_options)).has_value());
  REQUIRE(graph.add_exit_edge("recover").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  context.resume_info.emplace();
  auto invoked = invoke_value_sync(graph, wh::core::any(std::monostate{}), context);
  REQUIRE(invoked.has_value());
  auto typed = read_any<int>(invoked.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 0);
  REQUIRE(stream_chunk_count == 0U);
}

TEST_CASE("compose graph runtime validates checkpoint store configuration",
          "[core][compose][checkpoint][branch]") {
  wh::compose::graph graph{};
  int run_count = 0;
  REQUIRE(graph
              .add_lambda("worker",
                          [&run_count](wh::compose::graph_value &input, wh::core::run_context &,
                                       const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            ++run_count;
                            return std::move(input);
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  SECTION("explicit load checkpoint-id without store fails at startup") {
    wh::core::run_context context{};
    wh::compose::checkpoint_load_options load_options{};
    load_options.checkpoint_id = "checkpoint-A";
    wh::compose::graph_invoke_controls controls{};
    controls.checkpoint.load = load_options;
    auto invoked = invoke_graph_sync(graph, wh::core::any(1), context, controls);
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() == wh::core::errc::contract_violation);
    REQUIRE(run_count == 0);
    REQUIRE(invoked.value().report.checkpoint_error.has_value());
    REQUIRE(invoked.value().report.checkpoint_error->code == wh::core::errc::contract_violation);
    REQUIRE(invoked.value().report.checkpoint_error->checkpoint_id == "checkpoint-A");
    REQUIRE(invoked.value().report.checkpoint_error->operation == "validate_runtime_config");
  }

  SECTION("explicit write checkpoint-id without store fails at startup") {
    wh::core::run_context context{};
    wh::compose::checkpoint_save_options write_options{};
    write_options.checkpoint_id = "checkpoint-B";
    wh::compose::graph_invoke_controls controls{};
    controls.checkpoint.save = write_options;
    auto invoked = invoke_graph_sync(graph, wh::core::any(1), context, controls);
    REQUIRE(invoked.has_value());
    REQUIRE(invoked.value().output_status.has_error());
    REQUIRE(invoked.value().output_status.error() == wh::core::errc::contract_violation);
    REQUIRE(run_count == 0);
    REQUIRE(invoked.value().report.checkpoint_error.has_value());
    REQUIRE(invoked.value().report.checkpoint_error->code == wh::core::errc::contract_violation);
    REQUIRE(invoked.value().report.checkpoint_error->checkpoint_id == "checkpoint-B");
    REQUIRE(invoked.value().report.checkpoint_error->operation == "validate_runtime_config");
  }
}

TEST_CASE("compose graph runtime records checkpoint persist failures as structured detail",
          "[core][compose][checkpoint][branch]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input, wh::core::run_context &,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> { return std::move(input); })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = std::addressof(store);
  wh::compose::graph_invoke_controls controls{};
  controls.checkpoint.before_save = wh::compose::checkpoint_state_modifier{
      [](wh::compose::checkpoint_state &, wh::core::run_context &) -> wh::core::result<void> {
        return wh::core::result<void>::failure(wh::core::errc::not_supported);
      }};
  wh::core::run_context context{};
  auto invoked =
      invoke_graph_sync(graph, wh::core::any(8), context, controls, std::addressof(services));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value().output_status.has_value());
  auto typed = read_any<int>(invoked.value().output_status.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value() == 8);
  REQUIRE(invoked.value().report.checkpoint_error.has_value());
  REQUIRE(invoked.value().report.checkpoint_error->code == wh::core::errc::not_supported);
  REQUIRE(invoked.value().report.checkpoint_error->checkpoint_id == "graph");
  REQUIRE(invoked.value().report.checkpoint_error->operation == "pre_save_modifier");
}
