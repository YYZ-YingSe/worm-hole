#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/embedding/embedding.hpp"

namespace {

struct sync_embedding_impl {
  [[nodiscard]] auto embed(const wh::embedding::embedding_request &request) const
      -> wh::embedding::detail::embedding_result {
    return wh::embedding::embedding_response{
        std::vector<double>{static_cast<double>(request.inputs.size())}};
  }
};

struct async_embedding_impl {
  [[nodiscard]] auto embed_sender(const wh::embedding::embedding_request &request) const {
    return stdexec::just(
        wh::core::result<wh::embedding::embedding_response>{
            wh::embedding::embedding_response{
                std::vector<double>{static_cast<double>(request.inputs.size() + 1U)}}});
  }
};

struct move_embedding_impl {
  [[nodiscard]] auto embed(wh::embedding::embedding_request &&request) const
      -> wh::embedding::detail::embedding_result {
    return wh::embedding::embedding_response{
        std::vector<double>{static_cast<double>(request.inputs.size())},
        std::vector<double>{request.inputs.empty() ? 0.0 : 1.0}};
  }
};

} // namespace

TEST_CASE("embedding wrapper forwards sync implementations",
          "[UT][wh/embedding/embedding.hpp][embedding::embed][branch][boundary]") {
  wh::embedding::embedding wrapped{sync_embedding_impl{}};
  REQUIRE(wrapped.descriptor().kind == wh::core::component_kind::embedding);

  wh::embedding::embedding_request request{};
  request.inputs = {"a", "b"};
  wh::core::run_context context{};
  auto result = wrapped.embed(request, context);
  REQUIRE(result.has_value());
  REQUIRE(result.value().size() == 1U);
  REQUIRE(result.value().front() == std::vector<double>{2.0});
}

TEST_CASE("embedding wrapper normalizes async sender outputs",
          "[UT][wh/embedding/embedding.hpp][embedding::async_embed][branch]") {
  wh::embedding::embedding wrapped{async_embedding_impl{}};
  wh::embedding::embedding_request request{};
  request.inputs = {"a", "b"};
  wh::core::run_context context{};

  auto awaited = stdexec::sync_wait(wrapped.async_embed(request, context));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().front() == std::vector<double>{3.0});
}

TEST_CASE(
    "embedding detail state derives model metadata and move-sync helper selects rvalue overload",
    "[UT][wh/embedding/embedding.hpp][detail::make_callback_state][condition][branch][boundary]") {
  wh::embedding::embedding_request request{};
  request.inputs = {"x", "y", "z"};
  request.options.set_base(wh::embedding::embedding_common_options{
      .model_id = "embed-v1",
      .failure_policy = wh::embedding::batch_failure_policy::partial_success});

  const auto state = wh::embedding::detail::make_callback_state(request);
  REQUIRE(state.run_info.name == "Embedding");
  REQUIRE(state.event.model_id == "embed-v1");
  REQUIRE(state.event.batch_size == 3U);
  REQUIRE(state.event.usage.prompt_tokens == 3);
  REQUIRE(state.event.usage.total_tokens == 3);

  auto moved = wh::embedding::detail::run_sync_embedding_impl(
      move_embedding_impl{}, std::move(request));
  REQUIRE(moved.has_value());
  REQUIRE(moved.value().size() == 2U);
  REQUIRE(moved.value().front() == std::vector<double>{3.0});
}
