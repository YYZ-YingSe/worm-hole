#include <benchmark/benchmark.h>

#include <exec/static_thread_pool.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "wh/compose/graph.hpp"
#include "wh/core/stdexec/concurrent_sender_vector.hpp"
#include "wh/document/document.hpp"
#include "wh/embedding/embedding.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/prompt/simple_chat_template.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/stream.hpp"
#include "wh/tool/tool.hpp"

namespace {

using invoke_status = wh::core::result<wh::compose::graph_invoke_result>;
using pool_scheduler = decltype(std::declval<exec::static_thread_pool &>().get_scheduler());

[[nodiscard]] auto inline_sync_options()
    -> wh::compose::graph_add_node_options {
  return wh::compose::graph_add_node_options{
      .sync_dispatch = wh::compose::sync_dispatch::inline_control};
}

struct bench_profile {
  std::size_t stream_items{8U};
  std::size_t documents{4U};
  std::size_t tool_calls{2U};
};

struct bench_case {
  wh::compose::graph_runtime_mode mode{wh::compose::graph_runtime_mode::dag};
  std::size_t worker_threads{4U};
  std::size_t inflight{8U};
  bench_profile profile{};
};

[[nodiscard]] auto effective_stream_items(const bench_profile &profile) noexcept
    -> std::size_t {
  return std::max<std::size_t>(profile.stream_items, 1U);
}

[[nodiscard]] auto effective_tool_calls(const bench_profile &profile) noexcept
    -> std::size_t {
  return std::max<std::size_t>(profile.tool_calls, 1U);
}

[[nodiscard]] auto error_text(const std::string_view prefix,
                              const wh::core::error_code code) -> std::string {
  std::string text{prefix};
  text += ": ";
  text += code.message();
  return text;
}

[[nodiscard]] auto report_text(const wh::compose::graph_run_report &report)
    -> std::string {
  auto completed_suffix = [&report]() -> std::string {
    if (report.completed_node_keys.empty()) {
      return {};
    }
    std::string text = ";completed=";
    bool first = true;
    for (const auto &key : report.completed_node_keys) {
      if (!first) {
        text.push_back(',');
      }
      first = false;
      text += key;
    }
    return text;
  };
  if (report.node_run_error.has_value()) {
    return "node=" + report.node_run_error->node + ";message=" +
           report.node_run_error->message + ";raw=" +
           report.node_run_error->raw_error.message() + completed_suffix();
  }
  if (report.graph_run_error.has_value()) {
    return "node=" + report.graph_run_error->node + ";message=" +
           report.graph_run_error->message +
           (report.graph_run_error->raw_error.has_value()
                ? ";raw=" + report.graph_run_error->raw_error->message()
                : std::string{}) +
           completed_suffix();
  }
  if (report.stream_read_error.has_value()) {
    return "node=" + report.stream_read_error->node + ";message=" +
           report.stream_read_error->message + ";raw=" +
           report.stream_read_error->raw_error.message() + completed_suffix();
  }
  return {};
}

template <typename value_t>
[[nodiscard]] auto any_cref(const wh::compose::graph_value &value)
    -> wh::core::result<std::reference_wrapper<const value_t>> {
  const auto *typed = wh::core::any_cast<value_t>(&value);
  if (typed == nullptr) {
    return wh::core::result<std::reference_wrapper<const value_t>>::failure(
        wh::core::errc::type_mismatch);
  }
  return std::cref(*typed);
}

[[nodiscard]] auto first_text_part(const wh::schema::message &message)
    -> std::string_view {
  for (const auto &part : message.parts) {
    if (const auto *text = std::get_if<wh::schema::text_part>(&part);
        text != nullptr) {
      return text->text;
    }
  }
  return {};
}

[[nodiscard]] auto read_named_int(const wh::compose::graph_value_map &values,
                                  const std::string_view key)
    -> wh::core::result<std::int64_t> {
  const auto iter = values.find(key);
  if (iter == values.end()) {
    return wh::core::result<std::int64_t>::failure(wh::core::errc::not_found);
  }
  auto typed = any_cref<std::int64_t>(iter->second);
  if (typed.has_error()) {
    return wh::core::result<std::int64_t>::failure(typed.error());
  }
  return typed.value().get();
}

template <typename mapper_t>
[[nodiscard]] auto sum_graph_stream(wh::compose::graph_stream_reader reader,
                                    mapper_t mapper)
    -> wh::core::result<std::int64_t> {
  std::int64_t total = 0;
  while (true) {
    auto next = reader.read();
    if (next.has_error()) {
      return wh::core::result<std::int64_t>::failure(next.error());
    }
    if (next.value().error.failed()) {
      return wh::core::result<std::int64_t>::failure(next.value().error);
    }
    if (next.value().eof) {
      return total;
    }
    if (!next.value().value.has_value()) {
      return wh::core::result<std::int64_t>::failure(
          wh::core::errc::type_mismatch);
    }

    auto mapped = mapper(*next.value().value);
    if (mapped.has_error()) {
      return wh::core::result<std::int64_t>::failure(mapped.error());
    }
    total += mapped.value();
  }
}

[[nodiscard]] auto make_user_message(const std::string_view text)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{std::string{text}});
  return message;
}

class bench_chat_model_impl {
public:
  bench_chat_model_impl(pool_scheduler scheduler, bench_profile profile)
      : scheduler_(std::move(scheduler)), profile_(profile) {}

  [[nodiscard]] auto invoke(const wh::model::chat_request &request) const
      -> wh::core::result<wh::model::chat_response> {
    if (request.messages.empty()) {
      return wh::core::result<wh::model::chat_response>::failure(
          wh::core::errc::invalid_argument);
    }

    wh::schema::message response{};
    response.role = wh::schema::message_role::assistant;
    response.parts.emplace_back(
        wh::schema::text_part{std::string{first_text_part(request.messages.back())}});
    response.meta.usage.prompt_tokens =
        static_cast<std::int64_t>(request.messages.size());
    response.meta.usage.completion_tokens = 1;
    response.meta.usage.total_tokens =
        response.meta.usage.prompt_tokens + response.meta.usage.completion_tokens;
    return wh::model::chat_response{std::move(response), response.meta};
  }

  [[nodiscard]] auto stream_sender(wh::model::chat_request request) const {
    return stdexec::starts_on(
        scheduler_, stdexec::just(std::move(request)) |
                        stdexec::then([profile = profile_](wh::model::chat_request moved_request)
                                           -> wh::core::result<wh::model::chat_message_stream_reader> {
                          if (moved_request.messages.empty()) {
                            return wh::core::result<wh::model::chat_message_stream_reader>::failure(
                                wh::core::errc::invalid_argument);
                          }

                          const auto count = effective_stream_items(profile);
                          std::vector<wh::schema::message> chunks{};
                          chunks.reserve(count);
                          const auto prompt = std::string{
                              first_text_part(moved_request.messages.back())};
                          for (std::size_t index = 0U; index < count; ++index) {
                            wh::schema::message chunk{};
                            chunk.role = wh::schema::message_role::assistant;
                            chunk.parts.emplace_back(wh::schema::text_part{
                                prompt + "#" + std::to_string(index)});
                            chunks.push_back(std::move(chunk));
                          }
                          return wh::model::chat_message_stream_reader{
                              wh::schema::stream::make_values_stream_reader(
                                  std::move(chunks))};
                        }));
  }

private:
  pool_scheduler scheduler_;
  bench_profile profile_{};
};

class bench_document_impl {
public:
  explicit bench_document_impl(bench_profile profile) : profile_(profile) {}

  [[nodiscard]] auto process(const wh::document::document_request &request,
                             wh::core::run_context &) const
      -> wh::core::result<wh::document::document_batch> {
    wh::document::document_batch output{};
    output.reserve(profile_.documents);
    for (std::size_t index = 0U; index < profile_.documents; ++index) {
      wh::schema::document document{
          request.source + ":doc:" + std::to_string(index)};
      document.with_score(1.0 + static_cast<double>(index));
      document.with_sub_index("bench");
      output.push_back(std::move(document));
    }
    return output;
  }

private:
  bench_profile profile_{};
};

class bench_embedding_impl {
public:
  bench_embedding_impl(pool_scheduler scheduler, bench_profile profile)
      : scheduler_(std::move(scheduler)), profile_(profile) {}

  [[nodiscard]] auto embed_sender(wh::embedding::embedding_request request) const {
    return stdexec::starts_on(
        scheduler_, stdexec::just(std::move(request)) |
                        stdexec::then([profile = profile_](wh::embedding::embedding_request moved_request)
                                           -> wh::core::result<wh::embedding::embedding_response> {
                          wh::embedding::embedding_response output{};
                          output.reserve(moved_request.inputs.size());
                          for (const auto &input : moved_request.inputs) {
                            output.push_back(std::vector<double>{
                                static_cast<double>(input.size()),
                                static_cast<double>(profile.documents),
                                static_cast<double>(profile.stream_items)});
                          }
                          return output;
                        }));
  }

private:
  pool_scheduler scheduler_;
  bench_profile profile_{};
};

class bench_retriever_impl {
public:
  explicit bench_retriever_impl(bench_profile profile) : profile_(profile) {}

  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request) const
      -> wh::core::result<wh::retriever::retriever_response> {
    wh::retriever::retriever_response output{};
    output.reserve(profile_.documents);
    for (std::size_t index = 0U; index < profile_.documents; ++index) {
      wh::schema::document document{
          "hit:" + request.query + ":" + std::to_string(index)};
      document.with_score(10.0 - static_cast<double>(index));
      document.with_sub_index(request.sub_index.empty() ? "bench" : request.sub_index);
      output.push_back(std::move(document));
    }
    return output;
  }

private:
  bench_profile profile_{};
};

class bench_indexer_impl {
public:
  [[nodiscard]] auto write(const wh::indexer::indexer_request &request) const
      -> wh::core::result<wh::indexer::indexer_response> {
    wh::indexer::indexer_response output{};
    output.success_count = request.documents.size();
    output.document_ids.reserve(request.documents.size());
    for (std::size_t index = 0U; index < request.documents.size(); ++index) {
      output.document_ids.push_back("indexed:" + std::to_string(index));
    }
    return output;
  }
};

class bench_tool_component_impl {
public:
  explicit bench_tool_component_impl(bench_profile profile) : profile_(profile) {}

  [[nodiscard]] auto stream(const wh::tool::tool_request &request) const
      -> wh::core::result<wh::tool::tool_output_stream_reader> {
    const auto count = effective_stream_items(profile_);
    std::vector<std::string> chunks{};
    chunks.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
      chunks.push_back(request.input_json + ":tool:" + std::to_string(index));
    }
    return wh::tool::tool_output_stream_reader{
        wh::schema::stream::make_values_stream_reader(std::move(chunks))};
  }

private:
  bench_profile profile_{};
};

[[nodiscard]] auto make_tool_schema(const std::string_view name)
    -> wh::schema::tool_schema_definition {
  wh::schema::tool_schema_definition schema{};
  schema.name = std::string{name};
  schema.description = "benchmark tool";
  return schema;
}

[[nodiscard]] auto make_tools_sync_registry() -> wh::compose::tool_registry {
  wh::compose::tool_registry registry{};
  registry.emplace(
      "bench.echo",
      wh::compose::tool_entry{
          .invoke =
              [](const wh::compose::tool_call &call, wh::tool::call_scope)
                  -> wh::core::result<wh::compose::graph_value> {
            return wh::compose::graph_value{
                call.tool_name + ":" + call.arguments};
          }});
  registry.emplace(
      "bench.reverse",
      wh::compose::tool_entry{
          .invoke =
              [](const wh::compose::tool_call &call, wh::tool::call_scope)
                  -> wh::core::result<wh::compose::graph_value> {
            auto reversed = call.arguments;
            std::reverse(reversed.begin(), reversed.end());
            return wh::compose::graph_value{std::move(reversed)};
          }});
  return registry;
}

[[nodiscard]] auto make_tools_async_registry(const pool_scheduler &scheduler,
                                             const bench_profile profile)
    -> wh::compose::tool_registry {
  wh::compose::tool_registry registry{};
  registry.emplace(
      "bench.echo",
      wh::compose::tool_entry{
          .async_stream =
              [scheduler, profile](wh::compose::tool_call call, wh::tool::call_scope)
                  -> wh::compose::tools_stream_sender {
            return wh::compose::tools_stream_sender{
                stdexec::starts_on(
                    scheduler,
                    stdexec::just(std::move(call)) |
                        stdexec::then([profile](wh::compose::tool_call moved_call)
                                           -> wh::core::result<wh::compose::graph_stream_reader> {
                          std::vector<wh::compose::graph_value> values{};
                          values.reserve(effective_stream_items(profile));
                          for (std::size_t index = 0U;
                               index < effective_stream_items(profile); ++index) {
                            values.emplace_back(moved_call.arguments + ":async:" +
                                                std::to_string(index));
                          }
                          auto reader =
                              wh::compose::make_values_stream_reader(std::move(values));
                          if (reader.has_error()) {
                            return wh::core::result<
                                wh::compose::graph_stream_reader>::failure(
                                reader.error());
                          }
                          return std::move(reader).value();
                        }))};
          }});
  registry.emplace(
      "bench.reverse",
      wh::compose::tool_entry{
          .async_stream =
              [scheduler, profile](wh::compose::tool_call call, wh::tool::call_scope)
                  -> wh::compose::tools_stream_sender {
            return wh::compose::tools_stream_sender{
                stdexec::starts_on(
                    scheduler,
                    stdexec::just(std::move(call)) |
                        stdexec::then([profile](wh::compose::tool_call moved_call)
                                           -> wh::core::result<wh::compose::graph_stream_reader> {
                          auto reversed = moved_call.arguments;
                          std::reverse(reversed.begin(), reversed.end());
                          std::vector<wh::compose::graph_value> values{};
                          values.reserve(effective_stream_items(profile));
                          for (std::size_t index = 0U;
                               index < effective_stream_items(profile); ++index) {
                            values.emplace_back(reversed + ":async:" +
                                                std::to_string(index));
                          }
                          auto reader =
                              wh::compose::make_values_stream_reader(std::move(values));
                          if (reader.has_error()) {
                            return wh::core::result<
                                wh::compose::graph_stream_reader>::failure(
                                reader.error());
                          }
                          return std::move(reader).value();
                        }))};
          }});
  return registry;
}

[[nodiscard]] auto make_tool_batch(const std::string_view seed,
                                   const bench_profile profile)
    -> wh::compose::tool_batch {
  wh::compose::tool_batch batch{};
  batch.calls.reserve(effective_tool_calls(profile));
  static constexpr std::array<std::string_view, 2U> names{
      "bench.echo", "bench.reverse"};
  for (std::size_t index = 0U; index < effective_tool_calls(profile); ++index) {
    batch.calls.push_back(wh::compose::tool_call{
        .call_id = "call-" + std::to_string(index),
        .tool_name = std::string{names[index % names.size()]},
        .arguments = std::string{seed} + ":" + std::to_string(index),
    });
  }
  return batch;
}

[[nodiscard]] auto make_compile_options(const std::string_view name,
                                        const bench_case &config)
    -> wh::compose::graph_compile_options {
  wh::compose::graph_compile_options options{};
  options.name = std::string{name};
  options.mode = config.mode;
  options.dispatch_policy = wh::compose::graph_dispatch_policy::same_wave;
  options.trigger_mode = wh::compose::graph_trigger_mode::all_predecessors;
  options.fan_in_policy = wh::compose::graph_fan_in_policy::require_all_sources;
  options.max_parallel_nodes = std::max<std::size_t>(config.worker_threads, 4U);
  options.max_parallel_per_node = std::max<std::size_t>(config.worker_threads, 2U);
  options.retain_cold_data = false;
  return options;
}

[[nodiscard]] auto build_child_graph(const bench_case &config,
                                     const pool_scheduler &scheduler)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph child{make_compile_options("compose-real-bench-child", config)};

  auto added = child.add_lambda(
      "inner_value",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto value = any_cref<std::int64_t>(input);
        if (value.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              value.error());
        }
        return wh::compose::graph_value{value.value().get() * 2};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = child.add_lambda<wh::compose::node_contract::value,
                           wh::compose::node_contract::value,
                           wh::compose::node_exec_mode::async>(
      "inner_async",
      [scheduler, profile = config.profile](
          wh::compose::graph_value &input, wh::core::run_context &,
          const wh::compose::graph_call_scope &) -> wh::compose::graph_sender {
        auto base = any_cref<std::int64_t>(input);
        if (base.has_error()) {
          return wh::compose::graph_sender{
              wh::core::detail::ready_sender(
                  wh::core::result<wh::compose::graph_value>::failure(
                      base.error()))};
        }
        const auto lifted = base.value().get() +
                            static_cast<std::int64_t>(profile.stream_items);
        return wh::compose::graph_sender{
            stdexec::starts_on(
                scheduler,
                stdexec::just() |
                    stdexec::then(
                        [lifted]() -> wh::core::result<wh::compose::graph_value> {
                          return wh::compose::graph_value{lifted};
                        }))};
      });
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  auto linked = child.add_entry_edge("inner_value");
  if (linked.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(linked.error());
  }
  linked = child.add_edge("inner_value", "inner_async");
  if (linked.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(linked.error());
  }
  linked = child.add_exit_edge("inner_async");
  if (linked.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(linked.error());
  }

  return child;
}

[[nodiscard]] auto build_real_graph(const bench_case &config,
                                    const pool_scheduler &scheduler)
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph graph{make_compile_options("compose-real-bench", config)};

  wh::prompt::simple_chat_template prompt{
      std::vector<wh::prompt::prompt_message_template>{
          {wh::schema::message_role::system, "system: {{topic}}", "system"},
          {wh::schema::message_role::user, "user: {{topic}}", "user"}}};
  auto model = wh::model::chat_model{bench_chat_model_impl{scheduler, config.profile}};
  auto document = wh::document::document{bench_document_impl{config.profile}};
  auto embedding = wh::embedding::embedding{
      bench_embedding_impl{scheduler, config.profile}};
  auto retriever = wh::retriever::retriever{bench_retriever_impl{config.profile}};
  auto indexer = wh::indexer::indexer{bench_indexer_impl{}};
  auto tool_component = wh::tool::tool{
      make_tool_schema("bench_component_stream"),
      bench_tool_component_impl{config.profile}};
  auto child = build_child_graph(config, scheduler);
  if (child.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(child.error());
  }

  auto added = graph.add_lambda(
      "prompt_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto seed = any_cref<std::string>(input);
        if (seed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(seed.error());
        }
        wh::prompt::prompt_render_request request{};
        request.context.emplace("topic", wh::prompt::template_value{seed.value().get()});
        return wh::compose::graph_value{std::move(request)};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_component<wh::compose::component_kind::prompt,
                              wh::compose::node_contract::value,
                              wh::compose::node_contract::value>("prompt",
                                                                  prompt);
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "model_invoke_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto messages = any_cref<std::vector<wh::schema::message>>(input);
        if (messages.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              messages.error());
        }
        wh::model::chat_request request{};
        request.messages = messages.value().get();
        return wh::compose::graph_value{std::move(request)};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_component<wh::compose::component_kind::model,
                              wh::compose::node_contract::value,
                              wh::compose::node_contract::value>("model_invoke",
                                                                  model);
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "invoke_summary",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto response = any_cref<wh::model::chat_response>(input);
        if (response.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              response.error());
        }
        return wh::compose::graph_value{static_cast<std::int64_t>(
            first_text_part(response.value().get().message).size())};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "model_stream_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto seed = any_cref<std::string>(input);
        if (seed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(seed.error());
        }
        wh::model::chat_request request{};
        request.messages.push_back(make_user_message(seed.value().get()));
        return wh::compose::graph_value{std::move(request)};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_component<wh::compose::component_kind::model,
                              wh::compose::node_contract::value,
                              wh::compose::node_contract::stream,
                              wh::compose::node_exec_mode::async>("model_stream",
                                                                   model);
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_passthrough<wh::compose::node_contract::stream>("stream_pass");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda<wh::compose::node_contract::stream,
                           wh::compose::node_contract::value>(
      "stream_collect",
      [](wh::compose::graph_stream_reader input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto total = sum_graph_stream(
            std::move(input), [](const wh::compose::graph_value &value)
                                  -> wh::core::result<std::int64_t> {
              auto message = any_cref<wh::schema::message>(value);
              if (message.has_error()) {
                return wh::core::result<std::int64_t>::failure(message.error());
              }
              return static_cast<std::int64_t>(
                  first_text_part(message.value().get()).size());
            });
        if (total.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              total.error());
        }
        return wh::compose::graph_value{total.value()};
      });
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_passthrough("stream_collect_pad");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "document_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto seed = any_cref<std::string>(input);
        if (seed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(seed.error());
        }
        wh::document::document_request request{};
        request.source_kind = wh::document::document_source_kind::content;
        request.source = seed.value().get();
        return wh::compose::graph_value{std::move(request)};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_passthrough("tool_component_pad_1");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  added = graph.add_passthrough("tool_component_pad_2");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_component<wh::compose::component_kind::document,
                              wh::compose::node_contract::value,
                              wh::compose::node_contract::value>("document",
                                                                  document);
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_passthrough("tools_sync_pad_1");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  added = graph.add_passthrough("tools_sync_pad_2");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "embedding_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto documents = any_cref<wh::document::document_batch>(input);
        if (documents.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              documents.error());
        }
        wh::embedding::embedding_request request{};
        request.inputs.reserve(documents.value().get().size());
        for (const auto &document_value : documents.value().get()) {
          request.inputs.push_back(document_value.content());
        }
        return wh::compose::graph_value{std::move(request)};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_passthrough("tools_async_pad_1");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  added = graph.add_passthrough("tools_async_pad_2");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_component<wh::compose::component_kind::embedding,
                              wh::compose::node_contract::value,
                              wh::compose::node_contract::value,
                              wh::compose::node_exec_mode::async>("embedding",
                                                                   embedding);
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "embedding_summary",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto embeddings = any_cref<wh::embedding::embedding_response>(input);
        if (embeddings.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              embeddings.error());
        }
        return wh::compose::graph_value{static_cast<std::int64_t>(
            embeddings.value().get().size())};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "retriever_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto seed = any_cref<std::string>(input);
        if (seed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(seed.error());
        }
        wh::retriever::retriever_request request{};
        request.query = seed.value().get();
        request.index = "bench";
        request.sub_index = "bench";
        return wh::compose::graph_value{std::move(request)};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_component<wh::compose::component_kind::retriever,
                              wh::compose::node_contract::value,
                              wh::compose::node_contract::value>("retriever",
                                                                  retriever);
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "indexer_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto documents = any_cref<wh::retriever::retriever_response>(input);
        if (documents.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              documents.error());
        }
        wh::indexer::indexer_request request{};
        request.documents = documents.value().get();
        request.embedding = {1.0, 2.0, 3.0};
        return wh::compose::graph_value{std::move(request)};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_component<wh::compose::component_kind::indexer,
                              wh::compose::node_contract::value,
                              wh::compose::node_contract::value>("indexer",
                                                                  indexer);
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "indexer_summary",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto response = any_cref<wh::indexer::indexer_response>(input);
        if (response.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              response.error());
        }
        return wh::compose::graph_value{static_cast<std::int64_t>(
            response.value().get().success_count)};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "tool_component_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto seed = any_cref<std::string>(input);
        if (seed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(seed.error());
        }
        return wh::compose::graph_value{
            wh::tool::tool_request{.input_json = seed.value().get()}};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_component<wh::compose::component_kind::tool,
                              wh::compose::node_contract::value,
                              wh::compose::node_contract::stream>("tool_component",
                                                                   tool_component);
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda<wh::compose::node_contract::stream,
                           wh::compose::node_contract::value>(
      "tool_component_collect",
      [](wh::compose::graph_stream_reader input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto total = sum_graph_stream(
            std::move(input), [](const wh::compose::graph_value &value)
                                  -> wh::core::result<std::int64_t> {
              auto text = any_cref<std::string>(value);
              if (text.has_error()) {
                return wh::core::result<std::int64_t>::failure(text.error());
              }
              return static_cast<std::int64_t>(text.value().get().size());
            });
        if (total.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              total.error());
        }
        return wh::compose::graph_value{total.value()};
      });
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "tools_request",
      [profile = config.profile](wh::compose::graph_value &input,
                                 wh::core::run_context &,
                                 const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto seed = any_cref<std::string>(input);
        if (seed.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(seed.error());
        }
        return wh::compose::graph_value{
            make_tool_batch(seed.value().get(), profile)};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_tools<wh::compose::node_contract::value,
                          wh::compose::node_contract::value>("tools_sync",
                                                              make_tools_sync_registry());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "tools_sync_summary",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto results = any_cref<std::vector<wh::compose::tool_result>>(input);
        if (results.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              results.error());
        }
        std::int64_t total = 0;
        for (const auto &entry : results.value().get()) {
          auto text = any_cref<std::string>(entry.value);
          if (text.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                text.error());
          }
          total += static_cast<std::int64_t>(text.value().get().size());
        }
        return wh::compose::graph_value{total};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_tools<wh::compose::node_contract::value,
                          wh::compose::node_contract::stream,
                          wh::compose::node_exec_mode::async>(
      "tools_async", make_tools_async_registry(scheduler, config.profile), {},
      wh::compose::tools_options{.sequential = false});
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda<wh::compose::node_contract::stream,
                           wh::compose::node_contract::value>(
      "tools_async_collect",
      [](wh::compose::graph_stream_reader input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto total = sum_graph_stream(
            std::move(input), [](const wh::compose::graph_value &value)
                                  -> wh::core::result<std::int64_t> {
              auto event = any_cref<wh::compose::tool_event>(value);
              if (event.has_error()) {
                return wh::core::result<std::int64_t>::failure(event.error());
              }
              auto text = any_cref<std::string>(event.value().get().value);
              if (text.has_error()) {
                return wh::core::result<std::int64_t>::failure(text.error());
              }
              return static_cast<std::int64_t>(text.value().get().size());
            });
        if (total.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              total.error());
        }
        return wh::compose::graph_value{total.value()};
      });
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda<wh::compose::node_contract::value,
                           wh::compose::node_contract::value,
                           wh::compose::node_exec_mode::async>(
      "async_lambda",
      [scheduler, profile = config.profile](
          wh::compose::graph_value &input, wh::core::run_context &,
          const wh::compose::graph_call_scope &) -> wh::compose::graph_sender {
        auto seed = any_cref<std::string>(input);
        if (seed.has_error()) {
          return wh::compose::graph_sender{
              wh::core::detail::ready_sender(
                  wh::core::result<wh::compose::graph_value>::failure(
                      seed.error()))};
        }
        const auto value = static_cast<std::int64_t>(seed.value().get().size() +
                                                     profile.stream_items);
        return wh::compose::graph_sender{
            stdexec::starts_on(
                scheduler,
                stdexec::just() |
                    stdexec::then(
                        [value]() -> wh::core::result<wh::compose::graph_value> {
                          return wh::compose::graph_value{value};
                        }))};
      });
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_subgraph("subgraph", std::move(child).value());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_passthrough("subgraph_pad_1");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  added = graph.add_passthrough("subgraph_pad_2");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  added = graph.add_passthrough("subgraph_pad_3");
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  added = graph.add_lambda(
      "finalize",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto values = any_cref<wh::compose::graph_value_map>(input);
        if (values.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              values.error());
        }
        const auto &map = values.value().get();
        auto aligned_subgraph = read_named_int(map, "subgraph_pad_3");
        if (aligned_subgraph.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              aligned_subgraph.error());
        }
        auto invoke = read_named_int(map, "invoke_summary");
        if (invoke.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              invoke.error());
        }
        auto stream = read_named_int(map, "stream_collect_pad");
        if (stream.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              stream.error());
        }
        auto embedding = read_named_int(map, "embedding_summary");
        if (embedding.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              embedding.error());
        }
        auto indexer = read_named_int(map, "indexer_summary");
        if (indexer.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              indexer.error());
        }
        auto tool_component = read_named_int(map, "tool_component_pad_2");
        if (tool_component.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              tool_component.error());
        }
        auto tools_sync = read_named_int(map, "tools_sync_pad_2");
        if (tools_sync.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              tools_sync.error());
        }
        auto tools_async = read_named_int(map, "tools_async_pad_2");
        if (tools_async.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              tools_async.error());
        }
        return wh::compose::graph_value{
            aligned_subgraph.value() + invoke.value() + stream.value() +
            embedding.value() + indexer.value() + tool_component.value() +
            tools_sync.value() + tools_async.value()};
      },
      inline_sync_options());
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }

  for (const auto *entry_target : {"prompt_request", "model_stream_request",
                                   "document_request", "retriever_request",
                                   "tool_component_request", "tools_request",
                                   "async_lambda"}) {
    auto linked = graph.add_entry_edge(entry_target);
    if (linked.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(linked.error());
    }
  }

  for (const auto &[from, to] : std::array{
           std::pair{"prompt_request", "prompt"},
           std::pair{"prompt", "model_invoke_request"},
           std::pair{"model_invoke_request", "model_invoke"},
           std::pair{"model_invoke", "invoke_summary"},
           std::pair{"model_stream_request", "model_stream"},
           std::pair{"model_stream", "stream_pass"},
           std::pair{"stream_pass", "stream_collect"},
           std::pair{"document_request", "document"},
           std::pair{"document", "embedding_request"},
           std::pair{"embedding_request", "embedding"},
           std::pair{"embedding", "embedding_summary"},
           std::pair{"retriever_request", "retriever"},
           std::pair{"retriever", "indexer_request"},
           std::pair{"indexer_request", "indexer"},
           std::pair{"indexer", "indexer_summary"},
           std::pair{"tool_component_request", "tool_component"},
           std::pair{"tool_component", "tool_component_collect"},
           std::pair{"tools_request", "tools_sync"},
           std::pair{"tools_sync", "tools_sync_summary"},
           std::pair{"tools_sync_summary", "tools_sync_pad_1"},
           std::pair{"tools_sync_pad_1", "tools_sync_pad_2"},
           std::pair{"tools_request", "tools_async"},
           std::pair{"tools_async", "tools_async_collect"},
           std::pair{"tools_async_collect", "tools_async_pad_1"},
           std::pair{"tools_async_pad_1", "tools_async_pad_2"},
           std::pair{"async_lambda", "subgraph"},
           std::pair{"subgraph", "subgraph_pad_1"},
           std::pair{"subgraph_pad_1", "subgraph_pad_2"},
           std::pair{"subgraph_pad_2", "subgraph_pad_3"},
           std::pair{"subgraph_pad_3", "finalize"},
           std::pair{"invoke_summary", "finalize"},
           std::pair{"stream_collect", "stream_collect_pad"},
           std::pair{"stream_collect_pad", "finalize"},
           std::pair{"embedding_summary", "finalize"},
           std::pair{"indexer_summary", "finalize"},
           std::pair{"tool_component_collect", "tool_component_pad_1"},
           std::pair{"tool_component_pad_1", "tool_component_pad_2"},
           std::pair{"tool_component_pad_2", "finalize"},
           std::pair{"tools_sync_pad_2", "finalize"},
           std::pair{"tools_async_pad_2", "finalize"},
       }) {
    auto linked = graph.add_edge(from, to);
    if (linked.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(linked.error());
    }
  }

  auto exit = graph.add_exit_edge("finalize");
  if (exit.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(exit.error());
  }

  return graph;
}

auto invoke_once(const wh::compose::graph &graph, const pool_scheduler &scheduler,
                 std::string seed) -> exec::task<invoke_status> {
  co_await stdexec::schedule(scheduler);
  wh::core::run_context context{};
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(std::move(seed));
  co_return co_await graph.invoke(context, std::move(request));
}

auto invoke_many(const wh::compose::graph &graph, const pool_scheduler &scheduler,
                 const std::size_t request_count, const std::size_t inflight)
    -> exec::task<std::vector<invoke_status>> {
  std::vector<exec::task<invoke_status>> senders{};
  senders.reserve(request_count);
  for (std::size_t index = 0U; index < request_count; ++index) {
    senders.push_back(
        invoke_once(graph, scheduler, "seed-" + std::to_string(index)));
  }
  co_return co_await wh::core::detail::make_concurrent_sender_vector<invoke_status>(
      std::move(senders), inflight);
}

[[nodiscard]] auto parse_mode(const std::int64_t raw)
    -> wh::compose::graph_runtime_mode {
  return raw == 0 ? wh::compose::graph_runtime_mode::dag
                  : wh::compose::graph_runtime_mode::pregel;
}

[[nodiscard]] auto make_compile_case(const benchmark::State &state) -> bench_case {
  bench_case config{};
  config.mode = parse_mode(state.range(0));
  config.worker_threads = 4U;
  config.inflight = 8U;
  config.profile.stream_items = 8U;
  config.profile.documents = 4U;
  config.profile.tool_calls = 2U;
  return config;
}

[[nodiscard]] auto make_invoke_case(const benchmark::State &state) -> bench_case {
  bench_case config{};
  config.mode = parse_mode(state.range(0));
  config.worker_threads =
      static_cast<std::size_t>(std::max<std::int64_t>(state.range(1), 2));
  config.inflight =
      static_cast<std::size_t>(std::max<std::int64_t>(state.range(2), 1));
  config.profile.stream_items =
      static_cast<std::size_t>(std::max<std::int64_t>(state.range(3), 1));
  config.profile.documents = std::max<std::size_t>(config.profile.stream_items / 2U, 4U);
  config.profile.tool_calls = std::max<std::size_t>(config.profile.stream_items / 4U, 2U);
  return config;
}

[[nodiscard]] auto case_label(const bench_case &config) -> std::string {
  std::string label = config.mode == wh::compose::graph_runtime_mode::dag ? "dag" : "pregel";
  label += "/w";
  label += std::to_string(config.worker_threads);
  label += "/i";
  label += std::to_string(config.inflight);
  label += "/s";
  label += std::to_string(config.profile.stream_items);
  return label;
}

[[nodiscard]] auto go_default_worker_hint() -> int {
  const auto concurrency = std::thread::hardware_concurrency();
  if (concurrency == 0U) {
    return 4;
  }
  return static_cast<int>(std::max(2U, concurrency));
}

auto BM_compose_real_graph_compile(benchmark::State &state) -> void {
  const auto config = make_compile_case(state);
  exec::static_thread_pool pool{static_cast<std::uint32_t>(config.worker_threads)};
  state.SetLabel(case_label(config));

  for (auto _ : state) {
    auto graph = build_real_graph(config, pool.get_scheduler());
    if (graph.has_error()) {
      state.SkipWithError(error_text("build_real_graph", graph.error()).c_str());
      return;
    }
    auto compiled = graph.value().compile();
    if (compiled.has_error()) {
      state.SkipWithError(error_text("graph.compile", compiled.error()).c_str());
      return;
    }
    benchmark::DoNotOptimize(graph.value().compile_order().size());
    benchmark::ClobberMemory();
  }
}

auto BM_compose_real_graph_invoke(benchmark::State &state) -> void {
  const auto config = make_invoke_case(state);
  exec::static_thread_pool pool{static_cast<std::uint32_t>(config.worker_threads)};
  auto graph = build_real_graph(config, pool.get_scheduler());
  if (graph.has_error()) {
    state.SkipWithError(error_text("build_real_graph", graph.error()).c_str());
    return;
  }
  auto compiled = graph.value().compile();
  if (compiled.has_error()) {
    state.SkipWithError(error_text("graph.compile", compiled.error()).c_str());
    return;
  }

  const auto request_count = std::max<std::size_t>(config.inflight * 2U, 1U);
  state.SetLabel(case_label(config));

  for (auto _ : state) {
    auto waited =
        stdexec::sync_wait(invoke_many(graph.value(), pool.get_scheduler(),
                                       request_count, config.inflight));
    if (!waited.has_value()) {
      state.SkipWithError("invoke_many stopped");
      return;
    }

    const auto &results = std::get<0>(waited.value());
    std::int64_t aggregate = 0;
    for (const auto &status : results) {
      if (status.has_error()) {
        state.SkipWithError(error_text("graph.invoke", status.error()).c_str());
        return;
      }
      if (status.value().output_status.has_error()) {
        std::string detail =
            error_text("graph.output", status.value().output_status.error());
        const auto report_detail = report_text(status.value().report);
        if (!report_detail.empty()) {
          detail += " ";
          detail += report_detail;
        }
        state.SkipWithError(detail.c_str());
        return;
      }
      const auto &output_value = status.value().output_status.value();
      auto output = any_cref<std::int64_t>(output_value);
      if (output.has_error()) {
        std::string detail = error_text("graph.output_cast", output.error());
        detail += " actual=";
        detail += output_value.type().name();
        state.SkipWithError(detail.c_str());
        return;
      }
      aggregate += output.value().get();
    }

    benchmark::DoNotOptimize(aggregate);
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(request_count));
}

auto apply_compile_cases(benchmark::Benchmark *bench) -> void {
  bench->Args({0});
  bench->Args({1});
}

auto apply_invoke_cases(benchmark::Benchmark *bench) -> void {
  std::vector<int> workers_list{2, 4, go_default_worker_hint()};
  std::sort(workers_list.begin(), workers_list.end());
  workers_list.erase(std::unique(workers_list.begin(), workers_list.end()),
                     workers_list.end());

  for (const int mode : {0, 1}) {
    for (const int workers : workers_list) {
      for (const int inflight : {4, 16}) {
        for (const int stream_items : {4, 32}) {
          bench->Args({mode, workers, inflight, stream_items});
        }
      }
    }
  }
}

BENCHMARK(BM_compose_real_graph_compile)->Apply(apply_compile_cases);

BENCHMARK(BM_compose_real_graph_invoke)
    ->Apply(apply_invoke_cases)
    ->UseRealTime();

} // namespace
