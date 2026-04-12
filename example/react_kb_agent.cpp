#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/agent/bind.hpp"
#include "wh/compose/graph.hpp"
#include "wh/core/any.hpp"
#include "wh/core/json.hpp"
#include "wh/document/keys.hpp"
#include "wh/flow.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/prompt/simple_chat_template.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/document.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/stream.hpp"
#include "wh/schema/tool.hpp"
#include "wh/tool/tool.hpp"

namespace {

struct knowledge_base_state {
  std::vector<wh::schema::document> documents{};
};

[[nodiscard]] auto tokenize(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> tokens{};
  std::string current{};

  const auto flush = [&]() {
    if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  };

  for (const auto ch : text) {
    if (std::isalnum(ch) != 0) {
      current.push_back(static_cast<char>(std::tolower(ch)));
    } else {
      flush();
    }
  }
  flush();
  return tokens;
}

[[nodiscard]] auto split_parent_document(std::string_view content)
    -> std::vector<wh::schema::document> {
  std::vector<wh::schema::document> chunks{};
  std::size_t begin = 0U;
  while (begin <= content.size()) {
    const auto next = content.find('|', begin);
    const auto part = content.substr(begin, next == std::string_view::npos
                                                ? content.size() - begin
                                                : next - begin);
    if (!part.empty()) {
      chunks.emplace_back(std::string{part});
    }
    if (next == std::string_view::npos) {
      break;
    }
    begin = next + 1U;
  }
  return chunks;
}

[[nodiscard]] auto make_memory_indexer(
    const std::shared_ptr<knowledge_base_state> &kb) {
  struct impl {
    std::shared_ptr<knowledge_base_state> kb{};

    [[nodiscard]] auto write(const wh::indexer::indexer_request &request) const
        -> wh::core::result<wh::indexer::indexer_response> {
      kb->documents = request.documents;

      wh::indexer::indexer_response response{};
      response.success_count = request.documents.size();
      for (std::size_t index = 0U; index < request.documents.size(); ++index) {
        const auto *sub_id = request.documents[index].metadata_ptr<std::string>(
            wh::document::sub_id_metadata_key);
        response.document_ids.push_back(
            sub_id != nullptr ? *sub_id : "doc-" + std::to_string(index));
      }
      return response;
    }
  };

  return wh::indexer::indexer{impl{kb}};
}

[[nodiscard]] auto make_memory_retriever(
    const std::shared_ptr<knowledge_base_state> &kb) {
  struct impl {
    std::shared_ptr<knowledge_base_state> kb{};

    [[nodiscard]] auto retrieve(
        const wh::retriever::retriever_request &request) const
        -> wh::core::result<wh::retriever::retriever_response> {
      const auto query_tokens = tokenize(request.query);
      wh::retriever::retriever_response results{};

      for (const auto &document : kb->documents) {
        const auto document_tokens = tokenize(document.content());
        std::size_t score = 0U;
        for (const auto &token : query_tokens) {
          if (std::find(document_tokens.begin(), document_tokens.end(), token) !=
              document_tokens.end()) {
            ++score;
          }
        }
        if (score == 0U) {
          continue;
        }

        auto ranked = document;
        ranked.with_score(static_cast<double>(score));
        results.push_back(std::move(ranked));
      }

      return results;
    }
  };

  return wh::retriever::retriever{impl{kb}};
}

[[nodiscard]] auto make_tool_arguments_json(const std::string &query)
    -> std::string {
  std::string escaped{};
  escaped.reserve(query.size());
  for (const char ch : query) {
    if (ch == '\\' || ch == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return std::string{"{\"query\":\""} + escaped + "\"}";
}

[[nodiscard]] auto first_text_part(const wh::schema::message &message)
    -> std::string {
  for (const auto &part : message.parts) {
    if (const auto *text = std::get_if<wh::schema::text_part>(&part);
        text != nullptr) {
      return text->text;
    }
  }
  return {};
}

[[nodiscard]] auto last_message_by_role(
    const std::vector<wh::schema::message> &messages,
    const wh::schema::message_role role) -> const wh::schema::message * {
  for (auto iter = messages.rbegin(); iter != messages.rend(); ++iter) {
    if (iter->role == role) {
      return std::addressof(*iter);
    }
  }
  return nullptr;
}

class scripted_react_model_impl {
public:
  scripted_react_model_impl() = default;
  explicit scripted_react_model_impl(
      std::vector<wh::schema::tool_schema_definition> tools)
      : bound_tools_(std::move(tools)) {}

  [[nodiscard]] auto invoke(const wh::model::chat_request &request) const
      -> wh::model::chat_invoke_result {
    return wh::model::chat_response{make_message(request), {}};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &request) const
      -> wh::model::chat_message_stream_result {
    return wh::model::chat_message_stream_reader{
        wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
            make_message(request))};
  }

  [[nodiscard]] auto bind_tools(
      std::span<const wh::schema::tool_schema_definition> tools) const
      -> scripted_react_model_impl {
    return scripted_react_model_impl{
        std::vector<wh::schema::tool_schema_definition>{tools.begin(),
                                                        tools.end()}};
  }

private:
  [[nodiscard]] auto make_message(const wh::model::chat_request &request) const
      -> wh::schema::message {
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;

    if (const auto *tool_result =
            last_message_by_role(request.messages, wh::schema::message_role::tool);
        tool_result != nullptr) {
      message.parts.emplace_back(wh::schema::text_part{
          "Answer: " + first_text_part(*tool_result)});
      return message;
    }

    const auto *user_message =
        last_message_by_role(request.messages, wh::schema::message_role::user);
    const auto query =
        user_message != nullptr ? first_text_part(*user_message) : std::string{};
    const auto tool_name = !bound_tools_.empty() ? bound_tools_.front().name
                                                 : "search_kb";

    message.parts.emplace_back(
        wh::schema::text_part{"Searching the indexed knowledge base."});
    message.parts.emplace_back(wh::schema::tool_call_part{
        .index = 0U,
        .id = "search-call-1",
        .type = "function",
        .name = tool_name,
        .arguments = make_tool_arguments_json(query),
        .complete = true,
    });
    return message;
  }

  std::vector<wh::schema::tool_schema_definition> bound_tools_{};
};

struct search_tool_impl {
  std::shared_ptr<knowledge_base_state> kb{};

  [[nodiscard]] auto invoke(const wh::tool::tool_request &request) const
      -> wh::tool::tool_invoke_result {
    auto parsed = wh::core::parse_json(request.input_json);
    if (parsed.has_error()) {
      return wh::tool::tool_invoke_result::failure(parsed.error());
    }

    auto query_node = wh::core::json_find_member(parsed.value(), "query");
    const auto *query_value = query_node.has_value() ? query_node.value() : nullptr;
    if (query_value == nullptr || !query_value->IsString()) {
      return wh::tool::tool_invoke_result::failure(
          wh::core::errc::invalid_argument);
    }

    const auto query =
        std::string{query_value->GetString(),
                    static_cast<std::size_t>(query_value->GetStringLength())};

    auto flow = wh::flow::retrieval::multi_query{make_memory_retriever(kb)};
    auto max_queries = flow.set_max_queries(3U);
    if (max_queries.has_error()) {
      return wh::tool::tool_invoke_result::failure(max_queries.error());
    }

    wh::retriever::retriever_request retrieve_request{};
    retrieve_request.query = query;

    wh::core::run_context context{};
    auto awaited = stdexec::sync_wait(flow.retrieve(retrieve_request, context));
    if (!awaited.has_value()) {
      return wh::tool::tool_invoke_result::failure(wh::core::errc::internal_error);
    }

    auto status = std::move(std::get<0>(awaited.value()));
    if (status.has_error()) {
      return wh::tool::tool_invoke_result::failure(status.error());
    }
    if (status->empty()) {
      return std::string{"No indexed note matched the query."};
    }

    std::string output{};
    for (std::size_t index = 0U; index < status->size(); ++index) {
      if (index != 0U) {
        output.append("\n");
      }
      output.append(status->at(index).content());
    }
    return output;
  }
};

} // namespace

auto main() -> int {
  auto kb = std::make_shared<knowledge_base_state>();

  auto indexing = wh::flow::indexing::parent{
      make_memory_indexer(kb),
      [](const wh::schema::document &document, wh::core::run_context &)
          -> wh::core::result<std::vector<wh::schema::document>> {
        auto chunks = split_parent_document(document.content());
        if (chunks.empty()) {
          return wh::core::result<std::vector<wh::schema::document>>::failure(
              wh::core::errc::invalid_argument);
        }
        return chunks;
      },
      [](const wh::schema::document &, const std::size_t count,
         wh::core::run_context &) -> wh::core::result<std::vector<std::string>> {
        std::vector<std::string> ids{};
        ids.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
          ids.push_back("chunk-" + std::to_string(index));
        }
        return ids;
      }};

  wh::schema::document handbook{
      "Reset the build cache with ./build.sh --clean-root.|"
      "CI workflows live under .github/workflows.|"
      "Release artifacts are produced from build/release."};
  handbook.set_metadata(std::string{wh::document::parent_id_metadata_key},
                        "ops-handbook");

  wh::indexer::indexer_request write_request{};
  write_request.documents.push_back(std::move(handbook));

  wh::core::run_context write_context{};
  auto indexed = stdexec::sync_wait(indexing.write(write_request, write_context));
  if (!indexed.has_value()) {
    return 1;
  }
  auto write_status = std::move(std::get<0>(indexed.value()));
  if (write_status.has_error() || write_status->success_count != 3U) {
    return 2;
  }

  wh::prompt::simple_chat_template prompt({
      {wh::schema::message_role::system,
       "You are a support agent. Always use search_kb before answering factual questions.",
       "system"},
      {wh::schema::message_role::user, "{{question}}", "user"},
  });

  wh::prompt::template_context context{};
  context.emplace("question",
                  wh::prompt::template_value{"Where do CI workflows live?"});

  wh::core::run_context prompt_context{};
  auto rendered = prompt.render({context, {}}, prompt_context);
  if (rendered.has_error()) {
    return 3;
  }

  auto search_tool = wh::tool::tool{
      wh::schema::tool_schema_definition{
          .name = "search_kb",
          .description = "Search the indexed knowledge base.",
          .parameters = {{
              .name = "query",
              .type = wh::schema::tool_parameter_type::string,
              .description = "Natural-language search query.",
              .required = true,
          }},
      },
      search_tool_impl{kb}};

  wh::agent::react authored{"support_agent", "Knowledge-base support agent"};
  if (!authored
           .append_instruction(
               "Use the tool result to answer. Do not invent facts.")
           .has_value()) {
    return 4;
  }
  if (!authored.set_model(wh::model::chat_model{scripted_react_model_impl{}})
           .has_value()) {
    return 5;
  }
  if (!authored.add_tool(search_tool).has_value()) {
    return 6;
  }
  if (!authored.set_tools_node_options({}).has_value()) {
    return 7;
  }
  if (!authored.set_output_key("answer").has_value()) {
    return 8;
  }
  if (!authored
           .set_output_mode(wh::agent::react_output_mode::stream)
           .has_value()) {
    return 9;
  }

  auto agent = wh::agent::make_agent(std::move(authored));
  if (agent.has_error() || !agent->executable()) {
    return 10;
  }

  auto graph = agent->lower_graph();
  if (graph.has_error()) {
    return 11;
  }
  if (!graph->compiled() && !graph->compile().has_value()) {
    return 12;
  }

  wh::compose::graph_invoke_request request{};
  request.input = wh::core::any{rendered.value()};

  wh::core::run_context run_context{};
  auto invoked = stdexec::sync_wait(graph->invoke(run_context, std::move(request)));
  if (!invoked.has_value()) {
    return 13;
  }

  auto invoke_status = std::move(std::get<0>(invoked.value()));
  if (invoke_status.has_error() || !invoke_status->output_status.has_value()) {
    return 14;
  }

  auto *output = wh::core::any_cast<wh::agent::agent_output>(
      &invoke_status->output_status.value());
  if (output == nullptr) {
    return 15;
  }

  auto answer_iter = output->output_values.find("answer");
  if (answer_iter == output->output_values.end()) {
    return 16;
  }
  const auto *answer = wh::core::any_cast<std::string>(&answer_iter->second);
  if (answer == nullptr) {
    return 17;
  }

  return answer->find(".github/workflows") != std::string::npos ? 0 : 18;
}
