#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <exec/task.hpp>

#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node.hpp"
#include "wh/core/error.hpp"
#include "wh/document/document.hpp"
#include "wh/embedding/embedding.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/prompt/chat_template.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/message.hpp"
#include "wh/tool/tool.hpp"

namespace {

template <wh::compose::component_kind Kind, wh::compose::node_contract From,
          wh::compose::node_contract To, wh::compose::node_exec_mode Exec, typename component_t>
concept explicit_component_node_factory_available = requires {
  wh::compose::make_component_node<Kind, From, To, Exec>("node", std::declval<component_t>());
};

template <wh::compose::node_contract From, wh::compose::node_contract To, typename request_t,
          typename response_t, wh::compose::node_exec_mode Exec, typename component_t>
concept custom_component_node_factory_available = requires {
  wh::compose::make_component_node<wh::compose::component_kind::custom, From, To, request_t,
                                   response_t, Exec>("node", std::declval<component_t>());
};

struct model_exec_task_component {
  auto async_invoke(wh::model::chat_request, wh::core::run_context &) const
      -> exec::task<wh::model::chat_invoke_result> {
    co_return wh::model::chat_invoke_result::failure(wh::core::errc::not_supported);
  }

  auto async_stream(wh::model::chat_request, wh::core::run_context &) const
      -> exec::task<wh::model::chat_message_stream_result> {
    co_return wh::model::chat_message_stream_result::failure(wh::core::errc::not_supported);
  }
};

struct prompt_exec_task_component {
  auto async_render(wh::prompt::prompt_render_request, wh::core::run_context &) const
      -> exec::task<wh::core::result<std::vector<wh::schema::message>>> {
    co_return wh::core::result<std::vector<wh::schema::message>>::failure(
        wh::core::errc::not_supported);
  }
};

struct embedding_exec_task_component {
  auto async_embed(wh::embedding::embedding_request, wh::core::run_context &) const
      -> exec::task<wh::core::result<wh::embedding::embedding_response>> {
    co_return wh::core::result<wh::embedding::embedding_response>::failure(
        wh::core::errc::not_supported);
  }
};

struct retriever_exec_task_component {
  auto async_retrieve(wh::retriever::retriever_request, wh::core::run_context &) const
      -> exec::task<wh::core::result<wh::retriever::retriever_response>> {
    co_return wh::core::result<wh::retriever::retriever_response>::failure(
        wh::core::errc::not_supported);
  }
};

struct indexer_exec_task_component {
  auto async_write(wh::indexer::indexer_request, wh::core::run_context &) const
      -> exec::task<wh::core::result<wh::indexer::indexer_response>> {
    co_return wh::core::result<wh::indexer::indexer_response>::failure(
        wh::core::errc::not_supported);
  }
};

struct document_exec_task_component {
  auto async_process(wh::document::document_request, wh::core::run_context &) const
      -> exec::task<wh::core::result<wh::document::document_batch>> {
    co_return wh::core::result<wh::document::document_batch>::failure(
        wh::core::errc::not_supported);
  }
};

struct tool_exec_task_component {
  auto async_invoke(wh::tool::tool_request, wh::core::run_context &) const
      -> exec::task<wh::tool::tool_invoke_result> {
    co_return wh::tool::tool_invoke_result::failure(wh::core::errc::not_supported);
  }

  auto async_stream(wh::tool::tool_request, wh::core::run_context &) const
      -> exec::task<wh::tool::tool_output_stream_result> {
    co_return wh::tool::tool_output_stream_result::failure(wh::core::errc::not_supported);
  }
};

struct custom_value_value_exec_task_component {
  auto async_invoke(const int &, wh::core::run_context &) const
      -> exec::task<wh::core::result<int>> {
    co_return wh::core::result<int>::failure(wh::core::errc::not_supported);
  }
};

struct custom_value_stream_exec_task_component {
  auto async_stream(const int &, wh::core::run_context &) const
      -> exec::task<wh::core::result<wh::compose::graph_stream_reader>> {
    co_return wh::core::result<wh::compose::graph_stream_reader>::failure(
        wh::core::errc::not_supported);
  }
};

struct custom_stream_value_exec_task_component {
  auto async_invoke(wh::compose::graph_stream_reader, wh::core::run_context &) const
      -> exec::task<wh::core::result<int>> {
    co_return wh::core::result<int>::failure(wh::core::errc::not_supported);
  }
};

struct custom_stream_stream_exec_task_component {
  auto async_stream(wh::compose::graph_stream_reader, wh::core::run_context &) const
      -> exec::task<wh::core::result<wh::compose::graph_stream_reader>> {
    co_return wh::core::result<wh::compose::graph_stream_reader>::failure(
        wh::core::errc::not_supported);
  }
};

static_assert(explicit_component_node_factory_available<
              wh::compose::component_kind::model, wh::compose::node_contract::value,
              wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
              model_exec_task_component>);
static_assert(explicit_component_node_factory_available<
              wh::compose::component_kind::model, wh::compose::node_contract::value,
              wh::compose::node_contract::stream, wh::compose::node_exec_mode::async,
              model_exec_task_component>);
static_assert(explicit_component_node_factory_available<
              wh::compose::component_kind::prompt, wh::compose::node_contract::value,
              wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
              prompt_exec_task_component>);
static_assert(explicit_component_node_factory_available<
              wh::compose::component_kind::embedding, wh::compose::node_contract::value,
              wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
              embedding_exec_task_component>);
static_assert(explicit_component_node_factory_available<
              wh::compose::component_kind::retriever, wh::compose::node_contract::value,
              wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
              retriever_exec_task_component>);
static_assert(explicit_component_node_factory_available<
              wh::compose::component_kind::indexer, wh::compose::node_contract::value,
              wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
              indexer_exec_task_component>);
static_assert(explicit_component_node_factory_available<
              wh::compose::component_kind::document, wh::compose::node_contract::value,
              wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
              document_exec_task_component>);
static_assert(explicit_component_node_factory_available<
              wh::compose::component_kind::tool, wh::compose::node_contract::value,
              wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
              tool_exec_task_component>);
static_assert(explicit_component_node_factory_available<
              wh::compose::component_kind::tool, wh::compose::node_contract::value,
              wh::compose::node_contract::stream, wh::compose::node_exec_mode::async,
              tool_exec_task_component>);

static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::value, wh::compose::node_contract::value, int, int,
              wh::compose::node_exec_mode::async, custom_value_value_exec_task_component>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::value, wh::compose::node_contract::stream, int,
              wh::compose::graph_stream_reader, wh::compose::node_exec_mode::async,
              custom_value_stream_exec_task_component>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::stream, wh::compose::node_contract::value,
              wh::compose::graph_stream_reader, int, wh::compose::node_exec_mode::async,
              custom_stream_value_exec_task_component>);
static_assert(custom_component_node_factory_available<
              wh::compose::node_contract::stream, wh::compose::node_contract::stream,
              wh::compose::graph_stream_reader, wh::compose::graph_stream_reader,
              wh::compose::node_exec_mode::async, custom_stream_stream_exec_task_component>);

} // namespace

TEST_CASE("component node factories accept exec task backed async surfaces",
          "[UT][wh/compose/node/component.hpp][make_component_node][async][exec::task][static]") {
  STATIC_REQUIRE(explicit_component_node_factory_available<
                 wh::compose::component_kind::model, wh::compose::node_contract::value,
                 wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
                 model_exec_task_component>);
  STATIC_REQUIRE(explicit_component_node_factory_available<
                 wh::compose::component_kind::model, wh::compose::node_contract::value,
                 wh::compose::node_contract::stream, wh::compose::node_exec_mode::async,
                 model_exec_task_component>);
  STATIC_REQUIRE(explicit_component_node_factory_available<
                 wh::compose::component_kind::prompt, wh::compose::node_contract::value,
                 wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
                 prompt_exec_task_component>);
  STATIC_REQUIRE(explicit_component_node_factory_available<
                 wh::compose::component_kind::embedding, wh::compose::node_contract::value,
                 wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
                 embedding_exec_task_component>);
  STATIC_REQUIRE(explicit_component_node_factory_available<
                 wh::compose::component_kind::retriever, wh::compose::node_contract::value,
                 wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
                 retriever_exec_task_component>);
  STATIC_REQUIRE(explicit_component_node_factory_available<
                 wh::compose::component_kind::indexer, wh::compose::node_contract::value,
                 wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
                 indexer_exec_task_component>);
  STATIC_REQUIRE(explicit_component_node_factory_available<
                 wh::compose::component_kind::document, wh::compose::node_contract::value,
                 wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
                 document_exec_task_component>);
  STATIC_REQUIRE(explicit_component_node_factory_available<
                 wh::compose::component_kind::tool, wh::compose::node_contract::value,
                 wh::compose::node_contract::value, wh::compose::node_exec_mode::async,
                 tool_exec_task_component>);
  STATIC_REQUIRE(explicit_component_node_factory_available<
                 wh::compose::component_kind::tool, wh::compose::node_contract::value,
                 wh::compose::node_contract::stream, wh::compose::node_exec_mode::async,
                 tool_exec_task_component>);

  STATIC_REQUIRE(custom_component_node_factory_available<
                 wh::compose::node_contract::value, wh::compose::node_contract::value, int, int,
                 wh::compose::node_exec_mode::async, custom_value_value_exec_task_component>);
  STATIC_REQUIRE(custom_component_node_factory_available<
                 wh::compose::node_contract::value, wh::compose::node_contract::stream, int,
                 wh::compose::graph_stream_reader, wh::compose::node_exec_mode::async,
                 custom_value_stream_exec_task_component>);
  STATIC_REQUIRE(custom_component_node_factory_available<
                 wh::compose::node_contract::stream, wh::compose::node_contract::value,
                 wh::compose::graph_stream_reader, int, wh::compose::node_exec_mode::async,
                 custom_stream_value_exec_task_component>);
  STATIC_REQUIRE(custom_component_node_factory_available<
                 wh::compose::node_contract::stream, wh::compose::node_contract::stream,
                 wh::compose::graph_stream_reader, wh::compose::graph_stream_reader,
                 wh::compose::node_exec_mode::async, custom_stream_stream_exec_task_component>);
}
