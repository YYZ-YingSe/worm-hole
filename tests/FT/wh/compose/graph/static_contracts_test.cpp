#include <exception>
#include <memory>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/authored.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/node.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/document/document.hpp"
#include "wh/embedding/embedding.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/prompt/chat_template.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/stream.hpp"
#include "wh/tool/tool.hpp"

namespace {

struct public_only_subgraph_stub {
  wh::compose::graph graph_{};
};

struct graph_view_only_subgraph_stub {
  wh::compose::graph graph_{};

  [[nodiscard]] auto graph_view() const noexcept -> const wh::compose::graph & {
    return graph_;
  }
};

template <wh::compose::node_contract From, wh::compose::node_contract To,
          wh::compose::node_exec_mode Exec, typename lambda_t>
concept lambda_node_factory_available = requires {
  wh::compose::make_lambda_node<From, To, Exec>("node",
                                                std::declval<lambda_t>());
};

template <wh::core::component_kind Kind, wh::compose::node_contract From,
          wh::compose::node_contract To, wh::compose::node_exec_mode Exec,
          typename component_t>
concept explicit_component_node_factory_available = requires {
  wh::compose::make_component_node<Kind, From, To, Exec>(
      "node", std::declval<component_t>());
};

template <wh::compose::node_contract From, wh::compose::node_contract To,
          typename request_t, typename response_t,
          wh::compose::node_exec_mode Exec, typename component_t>
concept custom_component_node_factory_available = requires {
  wh::compose::make_component_node<wh::core::component_kind::custom, From, To,
                                   request_t, response_t, Exec>(
      "node", std::declval<component_t>());
};

template <wh::compose::node_contract From, wh::compose::node_contract To,
          wh::compose::node_exec_mode Exec>
concept tools_node_factory_available = requires {
  wh::compose::make_tools_node<From, To, Exec>("tools",
                                               wh::compose::tool_registry{});
};

template <typename graph_t>
concept subgraph_node_factory_available = requires {
  wh::compose::make_subgraph_node("child", std::declval<graph_t>());
};

template <typename graph_t>
concept chain_append_subgraph_available =
    requires(wh::compose::chain chain) {
      chain.append_subgraph("child", std::declval<graph_t>());
    };

template <typename graph_t>
concept parallel_add_subgraph_available =
    requires(wh::compose::parallel parallel) {
      parallel.add_subgraph("child", std::declval<graph_t>());
    };

template <typename graph_t>
concept workflow_add_subgraph_step_available =
    requires(wh::compose::workflow workflow) {
      workflow.add_subgraph_step("child", std::declval<graph_t>());
    };

inline constexpr bool workflow_step_lookup_available =
    requires(wh::compose::workflow workflow) {
  workflow.step("node");
};

inline constexpr bool workflow_end_available =
    requires(wh::compose::workflow workflow) {
  workflow.end();
};

template <typename workflow_t>
concept workflow_legacy_dependency_api_available =
    requires(workflow_t workflow) {
      workflow.add_dependency(wh::compose::workflow_dependency{});
    };

template <typename workflow_t>
concept workflow_legacy_value_branch_api_available =
    requires(workflow_t workflow, wh::compose::value_branch branch) {
      workflow.add_value_branch("node", std::move(branch));
    };

template <typename workflow_t>
concept workflow_legacy_stream_branch_api_available =
    requires(workflow_t workflow, wh::compose::stream_branch branch) {
      workflow.add_stream_branch("node", std::move(branch));
    };

struct no_scheduler_env {
  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
      -> stdexec::never_stop_token {
    return {};
  }
};

struct inline_scheduler_env {
  stdexec::inline_scheduler scheduler{};

  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
      -> stdexec::never_stop_token {
    return {};
  }

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
      -> stdexec::inline_scheduler {
    return scheduler;
  }

  template <typename cpo_t>
  [[nodiscard]] auto query(
      stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
      -> stdexec::inline_scheduler {
    return scheduler;
  }
};

template <typename env_t> struct graph_invoke_receiver {
  using receiver_concept = stdexec::receiver_t;

  env_t env_{};

  auto set_value(wh::core::result<wh::compose::graph_invoke_result>) && noexcept
      -> void {}

  auto set_error(std::exception_ptr) && noexcept -> void {}

  auto set_stopped() && noexcept -> void {}

  [[nodiscard]] auto get_env() const noexcept -> env_t { return env_; }
};

template <typename env_t> struct graph_stream_read_receiver {
  using receiver_concept = stdexec::receiver_t;

  env_t env_{};

  auto set_value(wh::schema::stream::stream_result<
                 wh::schema::stream::stream_chunk<wh::compose::graph_value>>) && noexcept
      -> void {}

  auto set_error(std::exception_ptr) && noexcept -> void {}

  auto set_stopped() && noexcept -> void {}

  [[nodiscard]] auto get_env() const noexcept -> env_t { return env_; }
};

template <typename env_t> struct graph_stream_write_receiver {
  using receiver_concept = stdexec::receiver_t;

  env_t env_{};

  auto set_value(wh::core::result<void>) && noexcept -> void {}

  auto set_error(std::exception_ptr) && noexcept -> void {}

  auto set_stopped() && noexcept -> void {}

  [[nodiscard]] auto get_env() const noexcept -> env_t { return env_; }
};

template <typename receiver_t>
concept graph_invoke_sender_connectable = requires(wh::compose::graph graph,
                                                   wh::core::run_context context,
                                                   receiver_t receiver) {
  stdexec::connect(graph.invoke(context, wh::compose::graph_invoke_request{}),
                   std::move(receiver));
};

template <typename receiver_t>
concept graph_stream_read_sender_connectable =
    requires(wh::compose::graph_stream_reader reader, receiver_t receiver) {
      stdexec::connect(std::move(reader).read_async(), std::move(receiver));
    };

template <typename receiver_t>
concept graph_stream_write_sender_connectable =
    requires(wh::compose::graph_stream_writer writer, receiver_t receiver) {
      stdexec::connect(std::move(writer).write_async(wh::compose::graph_value{}),
                       std::move(receiver));
    };

template <wh::compose::node_contract Contract>
concept passthrough_node_factory_available = requires {
  wh::compose::make_passthrough_node<Contract>("node");
};

static_assert(graph_invoke_sender_connectable<
              graph_invoke_receiver<inline_scheduler_env>>);
static_assert(!graph_invoke_sender_connectable<
              graph_invoke_receiver<no_scheduler_env>>);
static_assert(graph_stream_read_sender_connectable<
              graph_stream_read_receiver<inline_scheduler_env>>);
static_assert(!graph_stream_read_sender_connectable<
              graph_stream_read_receiver<no_scheduler_env>>);
static_assert(graph_stream_write_sender_connectable<
              graph_stream_write_receiver<inline_scheduler_env>>);
static_assert(!graph_stream_write_sender_connectable<
              graph_stream_write_receiver<no_scheduler_env>>);

struct lambda_vv_sync_ok {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_value> {
    return wh::core::result<wh::compose::graph_value>::failure(
        wh::core::errc::not_supported);
  }
};

struct lambda_vv_async_ok {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(
        wh::core::result<wh::compose::graph_value>::failure(
            wh::core::errc::not_supported));
  }
};

struct lambda_vs_sync_ok {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_stream_reader> {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(
        wh::core::errc::not_supported);
  }
};

struct lambda_vs_async_ok {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(
        wh::core::result<wh::compose::graph_stream_reader>::failure(
            wh::core::errc::not_supported));
  }
};

struct lambda_vs_async_bad_sender {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(wh::compose::graph_stream_reader{});
  }
};

using graph_values_reader =
    wh::schema::stream::values_stream_reader<
        std::vector<wh::compose::graph_value>>;

struct lambda_vs_sync_canonical_ok {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<graph_values_reader> {
    return graph_values_reader{std::vector<wh::compose::graph_value>{}};
  }
};

struct lambda_vs_async_canonical_ok {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(
        wh::core::result<graph_values_reader>::failure(
            wh::core::errc::not_supported));
  }
};

struct lambda_sv_sync_ok {
  auto operator()(wh::compose::graph_stream_reader, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_value> {
    return wh::core::result<wh::compose::graph_value>::failure(
        wh::core::errc::not_supported);
  }
};

struct lambda_sv_async_ok {
  auto operator()(wh::compose::graph_stream_reader, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(
        wh::core::result<wh::compose::graph_value>::failure(
            wh::core::errc::not_supported));
  }
};

struct lambda_sv_sync_bad_input {
  auto operator()(wh::compose::graph_value &, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_value> {
    return wh::core::result<wh::compose::graph_value>::failure(
        wh::core::errc::not_supported);
  }
};

struct lambda_ss_async_ok {
  auto operator()(wh::compose::graph_stream_reader, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const {
    return stdexec::just(
        wh::core::result<wh::compose::graph_stream_reader>::failure(
            wh::core::errc::not_supported));
  }
};

struct lambda_ss_sync_ok {
  auto operator()(wh::compose::graph_stream_reader, wh::core::run_context &,
                  const wh::compose::graph_call_scope &) const
      -> wh::core::result<wh::compose::graph_stream_reader> {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(
        wh::core::errc::not_supported);
  }
};

struct model_component_stub {
  auto invoke(wh::model::chat_request, wh::core::run_context &) const
      -> wh::model::chat_invoke_result {
    return wh::model::chat_invoke_result::failure(wh::core::errc::not_supported);
  }

  auto stream(wh::model::chat_request, wh::core::run_context &) const
      -> wh::model::chat_message_stream_result {
    return wh::model::chat_message_stream_result::failure(
        wh::core::errc::not_supported);
  }

  auto async_invoke(wh::model::chat_request, wh::core::run_context &) const {
    return stdexec::just(
        wh::model::chat_invoke_result::failure(wh::core::errc::not_supported));
  }

  auto async_stream(wh::model::chat_request, wh::core::run_context &) const {
    return stdexec::just(
        wh::model::chat_message_stream_result::failure(
            wh::core::errc::not_supported));
  }
};

struct model_component_bad_async_stub {
  auto async_invoke(wh::model::chat_request, wh::core::run_context &) const {
    return stdexec::just(wh::model::chat_response{});
  }
};

struct prompt_component_stub {
  auto render(wh::prompt::prompt_render_request, wh::core::run_context &) const
      -> wh::core::result<std::vector<wh::schema::message>> {
    return wh::core::result<std::vector<wh::schema::message>>::failure(
        wh::core::errc::not_supported);
  }

  auto async_render(wh::prompt::prompt_render_request,
                    wh::core::run_context &) const {
    return stdexec::just(
        wh::core::result<std::vector<wh::schema::message>>::failure(
            wh::core::errc::not_supported));
  }
};

struct embedding_component_stub {
  auto embed(wh::embedding::embedding_request, wh::core::run_context &) const
      -> wh::core::result<wh::embedding::embedding_response> {
    return wh::core::result<wh::embedding::embedding_response>::failure(
        wh::core::errc::not_supported);
  }

  auto async_embed(wh::embedding::embedding_request,
                   wh::core::run_context &) const {
    return stdexec::just(
        wh::core::result<wh::embedding::embedding_response>::failure(
            wh::core::errc::not_supported));
  }
};

struct retriever_component_stub {
  auto retrieve(wh::retriever::retriever_request, wh::core::run_context &) const
      -> wh::core::result<wh::retriever::retriever_response> {
    return wh::core::result<wh::retriever::retriever_response>::failure(
        wh::core::errc::not_supported);
  }

  auto async_retrieve(wh::retriever::retriever_request,
                      wh::core::run_context &) const {
    return stdexec::just(
        wh::core::result<wh::retriever::retriever_response>::failure(
            wh::core::errc::not_supported));
  }
};

struct indexer_component_stub {
  auto write(wh::indexer::indexer_request, wh::core::run_context &) const
      -> wh::core::result<wh::indexer::indexer_response> {
    return wh::core::result<wh::indexer::indexer_response>::failure(
        wh::core::errc::not_supported);
  }

  auto async_write(wh::indexer::indexer_request,
                   wh::core::run_context &) const {
    return stdexec::just(
        wh::core::result<wh::indexer::indexer_response>::failure(
            wh::core::errc::not_supported));
  }
};

struct document_component_stub {
  auto process(wh::document::document_request, wh::core::run_context &) const
      -> wh::core::result<wh::document::document_batch> {
    return wh::core::result<wh::document::document_batch>::failure(
        wh::core::errc::not_supported);
  }

  auto async_process(wh::document::document_request,
                     wh::core::run_context &) const {
    return stdexec::just(
        wh::core::result<wh::document::document_batch>::failure(
            wh::core::errc::not_supported));
  }
};

struct tool_component_stub {
  auto invoke(wh::tool::tool_request, wh::core::run_context &) const
      -> wh::tool::tool_invoke_result {
    return wh::tool::tool_invoke_result::failure(wh::core::errc::not_supported);
  }

  auto stream(wh::tool::tool_request, wh::core::run_context &) const
      -> wh::tool::tool_output_stream_result {
    return wh::tool::tool_output_stream_result::failure(
        wh::core::errc::not_supported);
  }

  auto async_invoke(wh::tool::tool_request, wh::core::run_context &) const {
    return stdexec::just(
        wh::tool::tool_invoke_result::failure(wh::core::errc::not_supported));
  }

  auto async_stream(wh::tool::tool_request, wh::core::run_context &) const {
    return stdexec::just(
        wh::tool::tool_output_stream_result::failure(
            wh::core::errc::not_supported));
  }
};

struct custom_value_sync_stub {
  auto invoke(int, wh::core::run_context &) const -> wh::core::result<int> {
    return wh::core::result<int>::failure(wh::core::errc::not_supported);
  }
};

struct custom_value_move_only_sync_stub {
  auto invoke(int, wh::core::run_context &) const
      -> wh::core::result<std::unique_ptr<int>> {
    return wh::core::result<std::unique_ptr<int>>::failure(
        wh::core::errc::not_supported);
  }
};

struct custom_value_reader_sync_stub {
  auto invoke(int, wh::core::run_context &) const
      -> wh::core::result<wh::compose::graph_stream_reader> {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(
        wh::core::errc::not_supported);
  }
};

struct custom_value_any_sync_stub {
  auto invoke(int, wh::core::run_context &) const
      -> wh::core::result<wh::core::any> {
    return wh::core::result<wh::core::any>::failure(
        wh::core::errc::not_supported);
  }
};

struct custom_value_async_stub {
  auto async_invoke(int, wh::core::run_context &) const {
    return stdexec::just(
        wh::core::result<int>::failure(wh::core::errc::not_supported));
  }
};

struct custom_stream_sync_stub {
  auto stream(int, wh::core::run_context &) const
      -> wh::core::result<wh::compose::graph_stream_reader> {
    return wh::core::result<wh::compose::graph_stream_reader>::failure(
        wh::core::errc::not_supported);
  }
};

struct custom_stream_value_sync_stub {
  auto invoke(wh::compose::graph_stream_reader, wh::core::run_context &) const
      -> wh::core::result<int> {
    return wh::core::result<int>::failure(wh::core::errc::not_supported);
  }
};

struct custom_stream_async_stub {
  auto async_stream(wh::compose::graph_stream_reader,
                    wh::core::run_context &) const {
    return stdexec::just(
        wh::core::result<wh::compose::graph_stream_reader>::failure(
            wh::core::errc::not_supported));
  }
};

struct custom_stream_output_async_stub {
  auto async_stream(int, wh::core::run_context &) const {
    return stdexec::just(wh::core::result<wh::compose::graph_stream_reader>::failure(
        wh::core::errc::not_supported));
  }
};

struct custom_stream_value_async_stub {
  auto async_invoke(wh::compose::graph_stream_reader,
                    wh::core::run_context &) const {
    return stdexec::just(
        wh::core::result<int>::failure(wh::core::errc::not_supported));
  }
};

struct custom_stream_canonical_sync_stub {
  auto stream(int, wh::core::run_context &) const
      -> wh::core::result<graph_values_reader> {
    return graph_values_reader{std::vector<wh::compose::graph_value>{}};
  }
};

struct custom_stream_canonical_async_stub {
  auto async_stream(int, wh::core::run_context &) const {
    return stdexec::just(
        wh::core::result<graph_values_reader>::failure(
            wh::core::errc::not_supported));
  }
};

struct custom_bad_async_stub {
  auto async_stream(int, wh::core::run_context &) const {
    return stdexec::just(wh::compose::graph_stream_reader{});
  }
};

struct custom_value_bad_async_stub {
  auto async_invoke(int, wh::core::run_context &) const {
    return stdexec::just(1);
  }
};

struct custom_value_reader_request_sync_stub {
  auto invoke(wh::compose::graph_stream_reader, wh::core::run_context &) const
      -> wh::core::result<int> {
    return wh::core::result<int>::failure(wh::core::errc::not_supported);
  }
};

struct custom_stream_wrong_request_sync_stub {
  auto invoke(int, wh::core::run_context &) const -> wh::core::result<int> {
    return wh::core::result<int>::failure(wh::core::errc::not_supported);
  }
};

struct custom_stream_bad_response_sync_stub {
  auto stream(int, wh::core::run_context &) const -> wh::core::result<int> {
    return wh::core::result<int>::failure(wh::core::errc::not_supported);
  }
};

static_assert(lambda_node_factory_available<wh::compose::node_contract::value,
                                            wh::compose::node_contract::value,
                                            wh::compose::node_exec_mode::sync,
                                            lambda_vv_sync_ok>);
static_assert(lambda_node_factory_available<wh::compose::node_contract::value,
                                            wh::compose::node_contract::value,
                                            wh::compose::node_exec_mode::async,
                                            lambda_vv_async_ok>);
static_assert(lambda_node_factory_available<wh::compose::node_contract::value,
                                            wh::compose::node_contract::stream,
                                            wh::compose::node_exec_mode::sync,
                                            lambda_vs_sync_ok>);
static_assert(lambda_node_factory_available<wh::compose::node_contract::value,
                                            wh::compose::node_contract::stream,
                                            wh::compose::node_exec_mode::async,
                                            lambda_vs_async_ok>);
static_assert(lambda_node_factory_available<wh::compose::node_contract::value,
                                            wh::compose::node_contract::stream,
                                            wh::compose::node_exec_mode::sync,
                                            lambda_vs_sync_canonical_ok>);
static_assert(lambda_node_factory_available<wh::compose::node_contract::value,
                                            wh::compose::node_contract::stream,
                                            wh::compose::node_exec_mode::async,
                                            lambda_vs_async_canonical_ok>);
static_assert(lambda_node_factory_available<wh::compose::node_contract::stream,
                                            wh::compose::node_contract::value,
                                            wh::compose::node_exec_mode::sync,
                                            lambda_sv_sync_ok>);
static_assert(lambda_node_factory_available<wh::compose::node_contract::stream,
                                            wh::compose::node_contract::value,
                                            wh::compose::node_exec_mode::async,
                                            lambda_sv_async_ok>);
static_assert(lambda_node_factory_available<wh::compose::node_contract::stream,
                                            wh::compose::node_contract::stream,
                                            wh::compose::node_exec_mode::sync,
                                            lambda_ss_sync_ok>);
static_assert(lambda_node_factory_available<wh::compose::node_contract::stream,
                                            wh::compose::node_contract::stream,
                                            wh::compose::node_exec_mode::async,
                                            lambda_ss_async_ok>);
static_assert(!lambda_node_factory_available<wh::compose::node_contract::value,
                                             wh::compose::node_contract::stream,
                                             wh::compose::node_exec_mode::async,
                                             lambda_vs_async_bad_sender>);
static_assert(!lambda_node_factory_available<wh::compose::node_contract::stream,
                                             wh::compose::node_contract::value,
                                             wh::compose::node_exec_mode::sync,
                                             lambda_sv_sync_bad_input>);

static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::model,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              model_component_stub>);
static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::model,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::async,
              model_component_stub>);
static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::model,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::async,
              model_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::model,
              wh::compose::node_contract::stream,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              model_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::model,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::async,
              model_component_bad_async_stub>);

static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::prompt,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              prompt_component_stub>);
static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::prompt,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::async,
              prompt_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::prompt,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::sync,
              prompt_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::prompt,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::async,
              prompt_component_stub>);

static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::embedding,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              embedding_component_stub>);
static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::embedding,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::async,
              embedding_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::embedding,
              wh::compose::node_contract::stream,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              embedding_component_stub>);

static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::retriever,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              retriever_component_stub>);
static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::retriever,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::async,
              retriever_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::retriever,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::sync,
              retriever_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::retriever,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::async,
              retriever_component_stub>);

static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::indexer,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              indexer_component_stub>);
static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::indexer,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::async,
              indexer_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::indexer,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::sync,
              indexer_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::indexer,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::async,
              indexer_component_stub>);

static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::document,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              document_component_stub>);
static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::document,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::async,
              document_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::document,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::sync,
              document_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::document,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::async,
              document_component_stub>);

static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::tool,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              tool_component_stub>);
static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::tool,
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::async,
              tool_component_stub>);
static_assert(explicit_component_node_factory_available<
              wh::core::component_kind::tool,
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              wh::compose::node_exec_mode::async,
              tool_component_stub>);
static_assert(!explicit_component_node_factory_available<
              wh::core::component_kind::tool,
              wh::compose::node_contract::stream,
              wh::compose::node_contract::value,
              wh::compose::node_exec_mode::sync,
              tool_component_stub>);

static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              int,
              int,
              wh::compose::node_exec_mode::sync,
              custom_value_sync_stub>);
static_assert(!custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              int,
              std::unique_ptr<int>,
              wh::compose::node_exec_mode::sync,
              custom_value_move_only_sync_stub>);
static_assert(!custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              int,
              wh::compose::graph_stream_reader,
              wh::compose::node_exec_mode::sync,
              custom_value_reader_sync_stub>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              int,
              wh::core::any,
              wh::compose::node_exec_mode::sync,
              custom_value_any_sync_stub>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              int,
              int,
              wh::compose::node_exec_mode::async,
              custom_value_async_stub>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              int,
              wh::compose::graph_stream_reader,
              wh::compose::node_exec_mode::sync,
              custom_stream_sync_stub>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              int,
              wh::compose::graph_stream_reader,
              wh::compose::node_exec_mode::async,
              custom_stream_output_async_stub>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              int,
              graph_values_reader,
              wh::compose::node_exec_mode::sync,
              custom_stream_canonical_sync_stub>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              int,
              graph_values_reader,
              wh::compose::node_exec_mode::async,
              custom_stream_canonical_async_stub>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::stream,
              wh::compose::node_contract::value,
              wh::compose::graph_stream_reader,
              int,
              wh::compose::node_exec_mode::sync,
              custom_stream_value_sync_stub>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::stream,
              wh::compose::node_contract::value,
              wh::compose::graph_stream_reader,
              int,
              wh::compose::node_exec_mode::async,
              custom_stream_value_async_stub>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::stream,
              wh::compose::node_contract::stream,
              wh::compose::graph_stream_reader,
              wh::compose::graph_stream_reader,
              wh::compose::node_exec_mode::async,
              custom_stream_async_stub>);
static_assert(!custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              int,
              int,
              wh::compose::node_exec_mode::async,
              custom_value_bad_async_stub>);
static_assert(!custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              int,
              wh::compose::graph_stream_reader,
              wh::compose::node_exec_mode::async,
              custom_bad_async_stub>);
static_assert(!custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::value,
              wh::compose::graph_stream_reader,
              int,
              wh::compose::node_exec_mode::sync,
              custom_value_reader_request_sync_stub>);
static_assert(!custom_component_node_factory_available<
              wh::compose::node_contract::stream,
              wh::compose::node_contract::value,
              int,
              int,
              wh::compose::node_exec_mode::sync,
              custom_stream_wrong_request_sync_stub>);
static_assert(!custom_component_node_factory_available<
              wh::compose::node_contract::value,
              wh::compose::node_contract::stream,
              int,
              int,
              wh::compose::node_exec_mode::sync,
              custom_stream_bad_response_sync_stub>);

static_assert(tools_node_factory_available<wh::compose::node_contract::value,
                                           wh::compose::node_contract::value,
                                           wh::compose::node_exec_mode::sync>);
static_assert(tools_node_factory_available<wh::compose::node_contract::value,
                                           wh::compose::node_contract::stream,
                                           wh::compose::node_exec_mode::sync>);
static_assert(tools_node_factory_available<wh::compose::node_contract::value,
                                           wh::compose::node_contract::value,
                                           wh::compose::node_exec_mode::async>);
static_assert(tools_node_factory_available<wh::compose::node_contract::value,
                                           wh::compose::node_contract::stream,
                                           wh::compose::node_exec_mode::async>);
static_assert(!tools_node_factory_available<wh::compose::node_contract::stream,
                                            wh::compose::node_contract::value,
                                            wh::compose::node_exec_mode::sync>);
static_assert(!tools_node_factory_available<wh::compose::node_contract::stream,
                                            wh::compose::node_contract::value,
                                            wh::compose::node_exec_mode::async>);
static_assert(!tools_node_factory_available<wh::compose::node_contract::stream,
                                            wh::compose::node_contract::stream,
                                            wh::compose::node_exec_mode::sync>);
static_assert(!tools_node_factory_available<wh::compose::node_contract::stream,
                                            wh::compose::node_contract::stream,
                                            wh::compose::node_exec_mode::async>);

static_assert(subgraph_node_factory_available<wh::compose::graph>);
static_assert(!subgraph_node_factory_available<public_only_subgraph_stub>);
static_assert(subgraph_node_factory_available<graph_view_only_subgraph_stub>);
static_assert(chain_append_subgraph_available<wh::compose::graph>);
static_assert(!chain_append_subgraph_available<public_only_subgraph_stub>);
static_assert(chain_append_subgraph_available<graph_view_only_subgraph_stub>);
static_assert(parallel_add_subgraph_available<wh::compose::graph>);
static_assert(!parallel_add_subgraph_available<public_only_subgraph_stub>);
static_assert(parallel_add_subgraph_available<graph_view_only_subgraph_stub>);
static_assert(workflow_add_subgraph_step_available<wh::compose::graph>);
static_assert(!workflow_add_subgraph_step_available<public_only_subgraph_stub>);
static_assert(workflow_add_subgraph_step_available<graph_view_only_subgraph_stub>);
static_assert(workflow_step_lookup_available);
static_assert(workflow_end_available);
static_assert(!workflow_legacy_dependency_api_available<wh::compose::workflow>);
static_assert(!workflow_legacy_value_branch_api_available<wh::compose::workflow>);
static_assert(!workflow_legacy_stream_branch_api_available<wh::compose::workflow>);
static_assert(passthrough_node_factory_available<
              wh::compose::node_contract::value>);
static_assert(passthrough_node_factory_available<
              wh::compose::node_contract::stream>);

} // namespace
