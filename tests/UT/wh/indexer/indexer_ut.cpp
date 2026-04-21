#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/indexer/indexer.hpp"

namespace {

struct single_indexer_impl {
  [[nodiscard]] auto write_one(const wh::schema::document &document,
                               const wh::indexer::indexer_options &) const
      -> wh::core::result<std::string> {
    return document.content() + "-id";
  }
};

struct async_indexer_impl {
  [[nodiscard]] auto write_sender(const wh::indexer::indexer_request &request) const {
    wh::indexer::indexer_response response{};
    response.success_count = request.documents.size();
    response.document_ids.reserve(request.documents.size());
    for (const auto &document : request.documents) {
      response.document_ids.push_back(document.content() + "-async");
    }
    return stdexec::just(wh::core::result<wh::indexer::indexer_response>{std::move(response)});
  }
};

struct failing_single_indexer_impl {
  [[nodiscard]] auto write_one(const wh::schema::document &,
                               const wh::indexer::indexer_options &) const
      -> wh::core::result<std::string> {
    return wh::core::result<std::string>::failure(wh::core::errc::timeout);
  }
};

} // namespace

TEST_CASE("indexer wrapper expands single-document handlers across batches",
          "[UT][wh/indexer/indexer.hpp][indexer::write][branch][boundary]") {
  wh::indexer::indexer wrapped{single_indexer_impl{}};

  wh::indexer::indexer_request request{};
  request.documents = {wh::schema::document{"a"}, wh::schema::document{"b"}};
  wh::core::run_context context{};
  auto result = wrapped.write(request, context);
  REQUIRE(result.has_value());
  REQUIRE(result.value().document_ids == std::vector<std::string>{"a-id", "b-id"});
  REQUIRE(result.value().success_count == 2U);
  REQUIRE(result.value().failure_count == 0U);
}

TEST_CASE("indexer wrapper normalizes async sender outputs",
          "[UT][wh/indexer/indexer.hpp][indexer::async_write][branch]") {
  wh::indexer::indexer wrapped{async_indexer_impl{}};
  wh::indexer::indexer_request request{};
  request.documents = {wh::schema::document{"a"}};
  wh::core::run_context context{};

  auto awaited = stdexec::sync_wait(wrapped.async_write(request, context));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().document_ids == std::vector<std::string>{"a-async"});
}

TEST_CASE(
    "indexer detail enforces combined-embedding preconditions and failure policies",
    "[UT][wh/indexer/indexer.hpp][detail::run_sync_indexer_impl][condition][branch][boundary]") {
  wh::indexer::indexer_request combined_request{};
  combined_request.documents = {wh::schema::document{"a"}};
  combined_request.options.set_base(
      wh::indexer::indexer_common_options{.failure_policy = wh::indexer::write_failure_policy::stop,
                                          .embedding_model = "",
                                          .combine_with_embedding = true});

  auto invalid =
      wh::indexer::detail::run_sync_indexer_impl(failing_single_indexer_impl{}, combined_request);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  wh::indexer::indexer_request retry_request{};
  retry_request.documents = {wh::schema::document{"a"}};
  retry_request.options.set_base(wh::indexer::indexer_common_options{
      .failure_policy = wh::indexer::write_failure_policy::retry, .max_retries = 1U});
  auto exhausted =
      wh::indexer::detail::run_sync_indexer_impl(failing_single_indexer_impl{}, retry_request);
  REQUIRE(exhausted.has_error());
  REQUIRE(exhausted.error() == wh::core::errc::retry_exhausted);

  wh::indexer::indexer_request skip_request{};
  skip_request.documents = {wh::schema::document{"a"}, wh::schema::document{"b"}};
  skip_request.options.set_base(wh::indexer::indexer_common_options{
      .failure_policy = wh::indexer::write_failure_policy::skip});
  auto skipped =
      wh::indexer::detail::run_sync_indexer_impl(failing_single_indexer_impl{}, skip_request);
  REQUIRE(skipped.has_value());
  REQUIRE(skipped.value().document_ids.empty());
  REQUIRE(skipped.value().success_count == 0U);
  REQUIRE(skipped.value().failure_count == 2U);
}
