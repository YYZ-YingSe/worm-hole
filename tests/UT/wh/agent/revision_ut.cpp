#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/agent/revision.hpp"

TEST_CASE("agent revision contracts carry context windows decisions and typed readers",
          "[UT][wh/agent/revision.hpp][review_decision_reader][condition][branch][boundary]") {
  wh::schema::message input{};
  input.role = wh::schema::message_role::user;

  wh::agent::agent_output draft{};
  draft.final_message.role = wh::schema::message_role::assistant;
  wh::agent::agent_output review{};
  review.final_message.role = wh::schema::message_role::assistant;

  const std::vector<wh::schema::message> input_messages{input};
  const std::vector<wh::agent::agent_output> drafts{draft};
  const std::vector<wh::agent::agent_output> reviews{review};

  wh::agent::revision_context context{
      .input_messages = input_messages,
      .draft_history = drafts,
      .current_draft = &drafts.front(),
      .review_history = reviews,
      .current_review = &reviews.front(),
      .remaining_iterations = 3U,
  };
  REQUIRE(context.input_messages.size() == 1U);
  REQUIRE(context.draft_history.size() == 1U);
  REQUIRE(context.current_draft == &drafts.front());
  REQUIRE(context.review_history.size() == 1U);
  REQUIRE(context.current_review == &reviews.front());
  REQUIRE(context.remaining_iterations == 3U);

  wh::agent::review_decision decision{.kind = wh::agent::review_decision_kind::revise};
  REQUIRE(decision.kind == wh::agent::review_decision_kind::revise);

  wh::agent::revision_request_builder builder =
      [](const wh::agent::revision_context &revision,
         wh::core::run_context &) -> wh::core::result<std::vector<wh::schema::message>> {
    return std::vector<wh::schema::message>(revision.input_messages.begin(),
                                            revision.input_messages.end());
  };
  wh::core::run_context run{};
  auto built = builder(context, run);
  REQUIRE(built.has_value());
  REQUIRE(built.value().size() == 1U);

  wh::agent::review_decision_reader reader =
      [](const wh::agent::agent_output &,
         wh::core::run_context &) -> wh::core::result<wh::agent::review_decision> {
    return wh::agent::review_decision{.kind = wh::agent::review_decision_kind::accept};
  };
  auto read = reader(draft, run);
  REQUIRE(read.has_value());
  REQUIRE(read.value().kind == wh::agent::review_decision_kind::accept);
}

TEST_CASE("agent revision context defaults to empty windows and null current pointers",
          "[UT][wh/agent/revision.hpp][revision_context][condition][branch][boundary]") {
  wh::agent::revision_context context{};

  REQUIRE(context.input_messages.empty());
  REQUIRE(context.draft_history.empty());
  REQUIRE(context.current_draft == nullptr);
  REQUIRE(context.review_history.empty());
  REQUIRE(context.current_review == nullptr);
  REQUIRE(context.remaining_iterations == 0U);
}

TEST_CASE("agent revision request builder can project current draft and review state",
          "[UT][wh/agent/revision.hpp][revision_request_builder][condition][branch][boundary]") {
  wh::agent::agent_output draft{};
  draft.final_message.role = wh::schema::message_role::assistant;
  draft.final_message.parts.emplace_back(wh::schema::text_part{"draft"});
  wh::agent::agent_output review{};
  review.final_message.role = wh::schema::message_role::assistant;
  review.final_message.parts.emplace_back(wh::schema::text_part{"review"});
  const std::vector<wh::agent::agent_output> drafts{draft};
  const std::vector<wh::agent::agent_output> reviews{review};
  wh::agent::revision_context context{
      .draft_history = drafts,
      .current_draft = &drafts.front(),
      .review_history = reviews,
      .current_review = &reviews.front(),
      .remaining_iterations = 1U,
  };

  wh::agent::revision_request_builder builder =
      [](const wh::agent::revision_context &revision,
         wh::core::run_context &) -> wh::core::result<std::vector<wh::schema::message>> {
    std::vector<wh::schema::message> messages{};
    if (revision.current_draft != nullptr) {
      messages.push_back(revision.current_draft->final_message);
    }
    if (revision.current_review != nullptr) {
      messages.push_back(revision.current_review->final_message);
    }
    return messages;
  };

  wh::core::run_context run{};
  auto built = builder(context, run);
  REQUIRE(built.has_value());
  REQUIRE(built->size() == 2U);
}
