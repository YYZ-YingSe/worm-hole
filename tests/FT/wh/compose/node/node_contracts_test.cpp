#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <stdexec/execution.hpp>

#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"
#include "wh/document/processor.hpp"
#include "wh/document/parser/text_parser.hpp"
#include "wh/model/echo_chat_model.hpp"
#include "wh/prompt/simple_chat_template.hpp"

namespace {

using wh::testing::helper::build_single_node_graph;
using wh::testing::helper::execute_single_compiled_node;
using wh::testing::helper::invoke_single_node_graph;
using wh::testing::helper::make_user_message;
using wh::testing::helper::read_any;
using wh::testing::helper::register_test_callbacks;
using wh::testing::helper::sync_embedding_impl;
using wh::testing::helper::sync_indexer_batch_impl;
using wh::testing::helper::sync_retriever_impl;
using wh::testing::helper::sync_tool_stream_impl;

[[nodiscard]] auto make_streaming_subgraph_graph()
    -> wh::core::result<wh::compose::graph> {
  wh::compose::graph graph{};
  auto added = graph.add_lambda(
      "emit",
      [](const wh::compose::graph_value &, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return wh::core::any(wh::compose::make_single_value_stream_reader(7));
      });
  if (added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(added.error());
  }
  auto entry = graph.add_entry_edge("emit");
  if (entry.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(entry.error());
  }
  auto exit = graph.add_exit_edge("emit");
  if (exit.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(exit.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(compiled.error());
  }
  return graph;
}

} // namespace

TEST_CASE("compose component node binds embedding component explicitly",
          "[core][compose][node][functional]") {
  wh::embedding::embedding component{sync_embedding_impl{
      [](const wh::embedding::embedding_request &request)
          -> wh::core::result<wh::embedding::embedding_response> {
        wh::embedding::embedding_response output{};
        output.reserve(request.inputs.size());
        for (const auto &input : request.inputs) {
          output.push_back(
              std::vector<double>{static_cast<double>(input.size())});
        }
        return output;
      }}};
  auto node = wh::compose::make_component_node<
      wh::compose::component_kind::embedding, wh::compose::node_contract::value,
      wh::compose::node_contract::value>("embed", std::move(component));
  wh::core::run_context context{};
  auto output = invoke_single_node_graph(
      node,
      wh::core::any(
          wh::embedding::embedding_request{.inputs = std::vector<std::string>{"a", "abcd"}}),
      context);
  REQUIRE(output.has_value());
  auto typed = read_any<wh::embedding::embedding_response>(output.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value().size() == 2U);
  REQUIRE(typed.value()[0] == std::vector<double>{1.0});
  REQUIRE(typed.value()[1] == std::vector<double>{4.0});
}

TEST_CASE("compose component node binds chat model invoke and stream explicitly",
          "[core][compose][node][functional]") {
  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello model"));

  auto invoke_node = wh::compose::make_component_node<
      wh::compose::component_kind::model, wh::compose::node_contract::value,
      wh::compose::node_contract::value>("chat-invoke",
                                         wh::model::echo_chat_model{});
  wh::core::run_context context{};
  auto invoke_output =
      execute_single_compiled_node(invoke_node, wh::core::any(request), context);
  REQUIRE(invoke_output.has_value());
  auto invoke_typed =
      read_any<wh::model::chat_response>(invoke_output.value());
  REQUIRE(invoke_typed.has_value());
  REQUIRE(std::get<wh::schema::text_part>(
              invoke_typed.value().message.parts.front())
              .text == "hello model");

  auto stream_node = wh::compose::make_component_node<
      wh::compose::component_kind::model, wh::compose::node_contract::value,
      wh::compose::node_contract::stream>("chat-stream",
                                          wh::model::echo_chat_model{});
  auto stream_output = execute_single_compiled_node(
      stream_node, wh::core::any(std::move(request)), context);
  REQUIRE(stream_output.has_value());
  auto stream_reader =
      read_any<wh::compose::graph_stream_reader>(std::move(stream_output).value());
  REQUIRE(stream_reader.has_value());
  auto chunk = stream_reader.value().read();
  REQUIRE(chunk.has_value());
  REQUIRE(chunk.value().value.has_value());
  auto message =
      read_any<wh::schema::message>(std::move(*chunk.value().value));
  REQUIRE(message.has_value());
  REQUIRE(std::get<wh::schema::text_part>(message.value().parts.front()).text ==
          "hello model");
}

TEST_CASE("compose component node binds prompt retriever and indexer explicitly",
          "[core][compose][node][functional]") {
  wh::prompt::simple_chat_template prompt(
      {wh::prompt::prompt_message_template{
          wh::schema::message_role::user, "Hello {{name}}", "user"}});
  wh::prompt::template_context placeholders{};
  placeholders.emplace("name", wh::prompt::template_value{"compose"});
  auto prompt_node = wh::compose::make_component_node<
      wh::compose::component_kind::prompt, wh::compose::node_contract::value,
      wh::compose::node_contract::value>("prompt", std::move(prompt));
  wh::core::run_context context{};
  auto prompt_output = invoke_single_node_graph(
      prompt_node,
      wh::core::any(wh::prompt::prompt_render_request{placeholders, {}}), context);
  REQUIRE(prompt_output.has_value());
  auto rendered =
      read_any<std::vector<wh::schema::message>>(prompt_output.value());
  REQUIRE(rendered.has_value());
  REQUIRE(rendered.value().size() == 1U);
  REQUIRE(std::get<wh::schema::text_part>(rendered.value().front().parts.front())
              .text == "Hello compose");

  wh::retriever::retriever retriever{sync_retriever_impl{
      [](const wh::retriever::retriever_request &request)
          -> wh::core::result<wh::retriever::retriever_response> {
        return wh::retriever::retriever_response{
            wh::schema::document{"hit:" + request.query}};
      }}};
  auto retriever_node = wh::compose::make_component_node<
      wh::compose::component_kind::retriever, wh::compose::node_contract::value,
      wh::compose::node_contract::value>("retriever", std::move(retriever));
  auto retriever_output = invoke_single_node_graph(
      retriever_node,
      wh::core::any(
          wh::retriever::retriever_request{.query = "needle", .index = "idx"}),
      context);
  REQUIRE(retriever_output.has_value());
  auto retrieved =
      read_any<wh::retriever::retriever_response>(retriever_output.value());
  REQUIRE(retrieved.has_value());
  REQUIRE(retrieved.value().size() == 1U);
  REQUIRE(retrieved.value().front().content() == "hit:needle");

  wh::indexer::indexer indexer{sync_indexer_batch_impl{
      [](const wh::indexer::indexer_request &request)
          -> wh::core::result<wh::indexer::indexer_response> {
        wh::indexer::indexer_response response{};
        response.success_count = request.documents.size();
        response.document_ids.resize(request.documents.size(), "indexed");
        return response;
      }}};
  auto indexer_node = wh::compose::make_component_node<
      wh::compose::component_kind::indexer, wh::compose::node_contract::value,
      wh::compose::node_contract::value>("indexer", std::move(indexer));
  auto indexer_output = invoke_single_node_graph(
      indexer_node,
      wh::core::any(wh::indexer::indexer_request{
          .documents = {wh::schema::document{"doc-1"}}}),
      context);
  REQUIRE(indexer_output.has_value());
  auto indexed =
      read_any<wh::indexer::indexer_response>(indexer_output.value());
  REQUIRE(indexed.has_value());
  REQUIRE(indexed.value().success_count == 1U);
  REQUIRE(indexed.value().document_ids == std::vector<std::string>{"indexed"});
}

TEST_CASE("compose component node binds document component explicitly",
          "[core][compose][node][functional]") {
  auto node = wh::compose::make_component_node<
      wh::compose::component_kind::document, wh::compose::node_contract::value,
      wh::compose::node_contract::value>(
      "document", wh::document::document{
                      wh::document::document_processor{
                          wh::document::parser::make_text_parser()}});

  wh::document::document_request request{};
  request.source_kind = wh::document::document_source_kind::content;
  request.source = "hello document";
  request.options.set_base(wh::document::loader_common_options{
      .parser = wh::document::parser_options{.uri = "mem://doc.txt"}});

  wh::core::run_context context{};
  auto output = invoke_single_node_graph(node, wh::core::any(std::move(request)),
                                         context);
  REQUIRE(output.has_value());
  auto typed = read_any<wh::document::document_batch>(output.value());
  REQUIRE(typed.has_value());
  REQUIRE(typed.value().size() == 1U);
  REQUIRE(typed.value().front().content() == "hello document");
  REQUIRE(typed.value().front().metadata_or<std::string>("_source") ==
          "mem://doc.txt");
}

TEST_CASE("compose component node binds tool stream component explicitly",
          "[core][compose][node][functional]") {
  wh::core::run_context context{};
  wh::schema::tool_schema_definition info{};
  info.name = "echo";
  wh::tool::tool component{
      std::move(info),
      sync_tool_stream_impl{
          [](const std::string_view input, const wh::tool::tool_options &)
              -> wh::core::result<wh::tool::tool_output_stream_reader> {
            auto [writer, reader] =
                wh::schema::stream::make_pipe_stream<std::string>(2U);
            auto wrote = writer.try_write(std::string{input});
            if (wrote.has_error()) {
              return wh::core::result<wh::tool::tool_output_stream_reader>::failure(
                  wrote.error());
            }
            auto closed = writer.close();
            if (closed.has_error()) {
              return wh::core::result<wh::tool::tool_output_stream_reader>::failure(
                  closed.error());
            }
            return wh::tool::tool_output_stream_reader{std::move(reader)};
          }}};
  auto stream_node = wh::compose::make_component_node<
      wh::compose::component_kind::tool, wh::compose::node_contract::value,
      wh::compose::node_contract::stream>("echo-stream", std::move(component));
  auto stream_output = execute_single_compiled_node(
      stream_node, wh::core::any(wh::tool::tool_request{.input_json = "payload"}),
      context);
  REQUIRE(stream_output.has_value());
  auto stream_reader =
      read_any<wh::compose::graph_stream_reader>(std::move(stream_output).value());
  REQUIRE(stream_reader.has_value());
  auto next = stream_reader.value().read();
  REQUIRE(next.has_value());
  REQUIRE(next.value().value.has_value());
  auto text = read_any<std::string>(std::move(*next.value().value));
  REQUIRE(text.has_value());
  REQUIRE(text.value() == "payload");
  auto eof = stream_reader.value().read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}

TEST_CASE("compose component node binds custom exact contracts explicitly",
          "[core][compose][node][functional]") {
  struct custom_value_value_component {
    [[nodiscard]] auto invoke(const int &value, wh::core::run_context &) const
        -> wh::core::result<int> {
      return value + 5;
    }
  };

  struct custom_value_stream_component {
    [[nodiscard]] auto stream(const int &value, wh::core::run_context &) const
        -> wh::core::result<wh::compose::graph_stream_reader> {
      return wh::compose::make_single_value_stream_reader(value + 10);
    }
  };

  struct custom_stream_value_component {
    [[nodiscard]] auto invoke(wh::compose::graph_stream_reader reader,
                              wh::core::run_context &) const
        -> wh::core::result<int> {
      auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
      if (chunks.has_error()) {
        return wh::core::result<int>::failure(chunks.error());
      }
      int sum = 0;
      for (const auto &chunk : chunks.value()) {
        auto typed = read_any<int>(chunk);
        if (typed.has_error()) {
          return wh::core::result<int>::failure(typed.error());
        }
        sum += typed.value();
      }
      return sum;
    }
  };

  struct custom_stream_stream_component {
    [[nodiscard]] auto stream(wh::compose::graph_stream_reader reader,
                              wh::core::run_context &) const
        -> wh::core::result<wh::compose::graph_stream_reader> {
      auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
      if (chunks.has_error()) {
        return wh::core::result<wh::compose::graph_stream_reader>::failure(
            chunks.error());
      }
      std::vector<wh::compose::graph_value> outputs{};
      outputs.reserve(chunks.value().size());
      for (const auto &chunk : chunks.value()) {
        auto typed = read_any<int>(chunk);
        if (typed.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(
              typed.error());
        }
        outputs.push_back(wh::core::any(typed.value() * 2));
      }
      return wh::compose::make_values_stream_reader(std::move(outputs));
    }
  };

  wh::core::run_context context{};

  auto value_value = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::value, int, int>("custom-vv",
                                                   custom_value_value_component{});
  auto value_value_output =
      execute_single_compiled_node(value_value, wh::core::any(7), context);
  REQUIRE(value_value_output.has_value());
  auto value_value_typed = read_any<int>(value_value_output.value());
  REQUIRE(value_value_typed.has_value());
  REQUIRE(value_value_typed.value() == 12);

  auto value_stream = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::stream, int, wh::compose::graph_stream_reader>(
      "custom-vs", custom_value_stream_component{});
  auto value_stream_output =
      execute_single_compiled_node(value_stream, wh::core::any(4), context);
  REQUIRE(value_stream_output.has_value());
  auto value_stream_reader =
      read_any<wh::compose::graph_stream_reader>(std::move(value_stream_output).value());
  REQUIRE(value_stream_reader.has_value());
  auto value_stream_chunks =
      wh::compose::collect_graph_stream_reader(std::move(value_stream_reader).value());
  REQUIRE(value_stream_chunks.has_value());
  REQUIRE(value_stream_chunks.value().size() == 1U);
  auto lifted = read_any<int>(value_stream_chunks.value()[0]);
  REQUIRE(lifted.has_value());
  REQUIRE(lifted.value() == 14);

  auto [stream_value_writer, stream_value_reader] = wh::compose::make_graph_stream();
  REQUIRE(stream_value_writer.try_write(wh::core::any(2)).has_value());
  REQUIRE(stream_value_writer.try_write(wh::core::any(3)).has_value());
  REQUIRE(stream_value_writer.close().has_value());
  auto stream_value = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::stream,
      wh::compose::node_contract::value, wh::compose::graph_stream_reader, int>(
      "custom-sv", custom_stream_value_component{});
  auto stream_value_output = execute_single_compiled_node(
      stream_value, wh::core::any(std::move(stream_value_reader)), context);
  REQUIRE(stream_value_output.has_value());
  auto summed = read_any<int>(stream_value_output.value());
  REQUIRE(summed.has_value());
  REQUIRE(summed.value() == 5);

  auto [stream_stream_writer, stream_stream_reader] =
      wh::compose::make_graph_stream();
  REQUIRE(stream_stream_writer.try_write(wh::core::any(3)).has_value());
  REQUIRE(stream_stream_writer.try_write(wh::core::any(4)).has_value());
  REQUIRE(stream_stream_writer.close().has_value());
  auto stream_stream = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::stream,
      wh::compose::node_contract::stream, wh::compose::graph_stream_reader,
      wh::compose::graph_stream_reader>("custom-ss",
                                        custom_stream_stream_component{});
  auto stream_stream_output = execute_single_compiled_node(
      stream_stream, wh::core::any(std::move(stream_stream_reader)), context);
  REQUIRE(stream_stream_output.has_value());
  auto stream_stream_result =
      read_any<wh::compose::graph_stream_reader>(std::move(stream_stream_output).value());
  REQUIRE(stream_stream_result.has_value());
  auto stream_stream_chunks =
      wh::compose::collect_graph_stream_reader(std::move(stream_stream_result).value());
  REQUIRE(stream_stream_chunks.has_value());
  REQUIRE(stream_stream_chunks.value().size() == 2U);
  auto first = read_any<int>(stream_stream_chunks.value()[0]);
  auto second = read_any<int>(stream_stream_chunks.value()[1]);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first.value() == 6);
  REQUIRE(second.value() == 8);
}

TEST_CASE("compose authored nodes preserve explicit execution mode metadata",
          "[core][compose][node][functional]") {
  auto sync_lambda = wh::compose::make_lambda_node(
      "sync-lambda",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> { return std::move(input); });
  REQUIRE(sync_lambda.exec_mode() == wh::compose::node_exec_mode::sync);
  REQUIRE(sync_lambda.exec_origin() == wh::compose::node_exec_origin::authored);
  auto compiled_sync_lambda = build_single_node_graph(sync_lambda);
  REQUIRE(compiled_sync_lambda.has_value());
  REQUIRE(compiled_sync_lambda->node != nullptr);
  REQUIRE(compiled_sync_lambda->node->meta.exec_mode ==
          wh::compose::node_exec_mode::sync);
  REQUIRE(compiled_sync_lambda->node->meta.exec_origin ==
          wh::compose::node_exec_origin::authored);

  auto async_lambda = wh::compose::make_lambda_node<
      wh::compose::node_contract::value, wh::compose::node_contract::value,
      wh::compose::node_exec_mode::async>(
      "async-lambda",
      [](const wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) {
        return stdexec::just(
            wh::core::result<wh::compose::graph_value>{input});
      });
  auto compiled_async_lambda = build_single_node_graph(async_lambda);
  REQUIRE(compiled_async_lambda.has_value());
  REQUIRE(compiled_async_lambda->node->meta.exec_mode ==
          wh::compose::node_exec_mode::async);
  REQUIRE(compiled_async_lambda->node->meta.exec_origin ==
          wh::compose::node_exec_origin::authored);

  struct embedding_counts {
    int sync_calls{0};
    int async_calls{0};
  };

  struct dual_embedding_impl {
    std::shared_ptr<embedding_counts> counts;

    [[nodiscard]] auto embed(
        const wh::embedding::embedding_request &request) const
        -> wh::core::result<wh::embedding::embedding_response> {
      ++counts->sync_calls;
      return wh::embedding::embedding_response{
          std::vector<double>{static_cast<double>(request.inputs.size())}};
    }

    [[nodiscard]] auto embed_sender(wh::embedding::embedding_request &&request) const {
      ++counts->async_calls;
      return stdexec::just(
          wh::core::result<wh::embedding::embedding_response>{
              wh::embedding::embedding_response{std::vector<double>{
                  static_cast<double>(request.inputs.size() + 10U)}}});
    }
  };

  auto shared_counts = std::make_shared<embedding_counts>();
  auto sync_component = wh::compose::make_component_node<
      wh::compose::component_kind::embedding, wh::compose::node_contract::value,
      wh::compose::node_contract::value>(
      "sync-component", wh::embedding::embedding{dual_embedding_impl{shared_counts}});
  auto compiled_sync_component = build_single_node_graph(sync_component);
  REQUIRE(compiled_sync_component.has_value());
  REQUIRE(compiled_sync_component->node->meta.exec_mode ==
          wh::compose::node_exec_mode::sync);
  REQUIRE(compiled_sync_component->node->meta.exec_origin ==
          wh::compose::node_exec_origin::authored);

  auto async_component = wh::compose::make_component_node<
      wh::compose::component_kind::embedding, wh::compose::node_contract::value,
      wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
      "async-component", wh::embedding::embedding{dual_embedding_impl{shared_counts}});
  auto compiled_async_component = build_single_node_graph(async_component);
  REQUIRE(compiled_async_component.has_value());
  REQUIRE(compiled_async_component->node->meta.exec_mode ==
          wh::compose::node_exec_mode::async);
  REQUIRE(compiled_async_component->node->meta.exec_origin ==
          wh::compose::node_exec_origin::authored);

  auto passthrough = wh::compose::make_passthrough_node("pass");
  auto compiled_passthrough = build_single_node_graph(passthrough);
  REQUIRE(compiled_passthrough.has_value());
  REQUIRE(compiled_passthrough->node->meta.exec_mode ==
          wh::compose::node_exec_mode::sync);
  REQUIRE(compiled_passthrough->node->meta.exec_origin ==
          wh::compose::node_exec_origin::lowered);

  wh::compose::tool_registry registry{};
  registry.insert_or_assign(
      "echo", wh::compose::tool_entry{
                  .invoke =
                      [](const wh::compose::tool_call &call, wh::tool::call_scope)
                          -> wh::core::result<wh::compose::graph_value> {
                    return wh::core::any(call.arguments);
                  }});
  auto tools_node = wh::compose::make_tools_node("tools", std::move(registry));
  auto compiled_tools = build_single_node_graph(tools_node);
  REQUIRE(compiled_tools.has_value());
  REQUIRE(compiled_tools->node->meta.exec_mode ==
          wh::compose::node_exec_mode::sync);
  REQUIRE(compiled_tools->node->meta.exec_origin ==
          wh::compose::node_exec_origin::authored);

  wh::compose::tool_registry async_registry{};
  async_registry.insert_or_assign(
      "echo", wh::compose::tool_entry{
                  .async_invoke =
                      [](wh::compose::tool_call call, wh::tool::call_scope)
                          -> wh::compose::tools_invoke_sender {
                    return stdexec::just(
                        wh::core::result<wh::compose::graph_value>{
                            wh::core::any(std::string{"async:"} + call.arguments)});
                  }});
  auto async_tools_node = wh::compose::make_tools_node<
      wh::compose::node_contract::value, wh::compose::node_contract::value,
      wh::compose::node_exec_mode::async>("tools-async",
                                          std::move(async_registry));
  auto compiled_async_tools = build_single_node_graph(async_tools_node);
  REQUIRE(compiled_async_tools.has_value());
  REQUIRE(compiled_async_tools->node->meta.exec_mode ==
          wh::compose::node_exec_mode::async);
  REQUIRE(compiled_async_tools->node->meta.exec_origin ==
          wh::compose::node_exec_origin::authored);

  auto child_graph = make_streaming_subgraph_graph();
  REQUIRE(child_graph.has_value());
  auto subgraph = wh::compose::make_subgraph_node(
      "child-subgraph", std::move(child_graph).value());
  auto compiled_subgraph = build_single_node_graph(subgraph);
  REQUIRE(compiled_subgraph.has_value());
  REQUIRE(compiled_subgraph->node->meta.exec_mode ==
          wh::compose::node_exec_mode::async);
  REQUIRE(compiled_subgraph->node->meta.exec_origin ==
          wh::compose::node_exec_origin::lowered);
}

TEST_CASE("compose component node explicit async mode uses async handlers",
          "[core][compose][node][functional]") {
  struct embedding_counts {
    int sync_calls{0};
    int async_calls{0};
  };

  struct dual_embedding_impl {
    std::shared_ptr<embedding_counts> counts;

    [[nodiscard]] auto embed(
        const wh::embedding::embedding_request &request) const
        -> wh::core::result<wh::embedding::embedding_response> {
      ++counts->sync_calls;
      return wh::embedding::embedding_response{
          std::vector<double>{static_cast<double>(request.inputs.size())}};
    }

    [[nodiscard]] auto embed_sender(wh::embedding::embedding_request &&request) const {
      ++counts->async_calls;
      return stdexec::just(
          wh::core::result<wh::embedding::embedding_response>{
              wh::embedding::embedding_response{std::vector<double>{
                  static_cast<double>(request.inputs.size() + 100U)}}});
    }
  };

  auto shared_counts = std::make_shared<embedding_counts>();
  auto sync_node = wh::compose::make_component_node<
      wh::compose::component_kind::embedding, wh::compose::node_contract::value,
      wh::compose::node_contract::value>(
      "embed-sync", wh::embedding::embedding{dual_embedding_impl{shared_counts}});
  auto async_node = wh::compose::make_component_node<
      wh::compose::component_kind::embedding, wh::compose::node_contract::value,
      wh::compose::node_contract::value, wh::compose::node_exec_mode::async>(
      "embed-async", wh::embedding::embedding{dual_embedding_impl{shared_counts}});

  wh::core::run_context context{};
  auto sync_output = invoke_single_node_graph(
      sync_node, wh::core::any(wh::embedding::embedding_request{.inputs = {"abc"}}),
      context);
  REQUIRE(sync_output.has_value());
  auto sync_typed =
      read_any<wh::embedding::embedding_response>(sync_output.value());
  REQUIRE(sync_typed.has_value());
  REQUIRE(sync_typed.value()[0] == std::vector<double>{1.0});

  auto async_output = invoke_single_node_graph(
      async_node, wh::core::any(wh::embedding::embedding_request{.inputs = {"abc"}}),
      context);
  REQUIRE(async_output.has_value());
  auto async_typed =
      read_any<wh::embedding::embedding_response>(async_output.value());
  REQUIRE(async_typed.has_value());
  REQUIRE(async_typed.value()[0] == std::vector<double>{101.0});
  REQUIRE(shared_counts->sync_calls == 1);
  REQUIRE(shared_counts->async_calls == 1);

  int start_count = 0;
  int end_count = 0;
  int error_count = 0;
  std::optional<wh::embedding::embedding_callback_event> end_event{};
  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context),
      [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage,
          const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::embedding::embedding_callback_event>();
        REQUIRE(typed != nullptr);
        if (stage == wh::core::callback_stage::start) {
          ++start_count;
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          ++end_count;
          end_event = *typed;
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          ++error_count;
        }
      },
      "compose-async-component");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  auto callback_output = invoke_single_node_graph(
      async_node,
      wh::core::any(wh::embedding::embedding_request{.inputs = {"abc", "de"}}),
      callback_context);
  REQUIRE(callback_output.has_value());
  auto callback_typed =
      read_any<wh::embedding::embedding_response>(callback_output.value());
  REQUIRE(callback_typed.has_value());
  REQUIRE(callback_typed.value()[0] == std::vector<double>{102.0});
  REQUIRE(start_count == 1);
  REQUIRE(end_count == 1);
  REQUIRE(error_count == 0);
  REQUIRE(end_event.has_value());
  REQUIRE(end_event->batch_size == 2U);
  REQUIRE(end_event->usage.prompt_tokens == 2);
  REQUIRE(end_event->usage.completion_tokens == 1);
  REQUIRE(end_event->usage.total_tokens == 3);
  REQUIRE(shared_counts->async_calls == 2);

  struct custom_counts {
    int sync_calls{0};
    int async_calls{0};
  };

  struct dual_custom_component {
    std::shared_ptr<custom_counts> counts;

    [[nodiscard]] auto invoke(const int &value, wh::core::run_context &) const
        -> wh::core::result<int> {
      ++counts->sync_calls;
      return value + 1;
    }

    [[nodiscard]] auto async_invoke(const int &value, wh::core::run_context &) const {
      ++counts->async_calls;
      return stdexec::just(wh::core::result<int>{value + 20});
    }
  };

  auto custom_shared_counts = std::make_shared<custom_counts>();
  auto custom_async = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::value, int, int,
      wh::compose::node_exec_mode::async>("custom-async",
                                          dual_custom_component{custom_shared_counts});
  auto custom_output =
      invoke_single_node_graph(custom_async, wh::core::any(7), context);
  REQUIRE(custom_output.has_value());
  auto custom_typed = read_any<int>(custom_output.value());
  REQUIRE(custom_typed.has_value());
  REQUIRE(custom_typed.value() == 27);
  REQUIRE(custom_shared_counts->sync_calls == 0);
  REQUIRE(custom_shared_counts->async_calls == 1);
}
