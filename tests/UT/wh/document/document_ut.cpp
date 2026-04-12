#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/document/document.hpp"

namespace {

struct sync_document_impl {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"SyncDocument", wh::core::component_kind::document};
  }

  [[nodiscard]] auto process(const wh::document::document_request &request,
                             wh::core::run_context &) const
      -> wh::core::result<wh::document::document_batch> {
    return wh::document::document_batch{
        wh::schema::document{request.source + ":sync"}};
  }
};

struct async_document_impl {
  [[nodiscard]] auto process_sender(const wh::document::document_request &request) const {
    return stdexec::just(
        wh::core::result<wh::document::document_batch>{
            wh::document::document_batch{
                wh::schema::document{request.source + ":async"}}});
  }
};

} // namespace

TEST_CASE("document wrapper forwards sync impls and descriptors",
          "[UT][wh/document/document.hpp][document::process][branch][boundary]") {
  wh::document::document wrapped{sync_document_impl{}};
  REQUIRE(wrapped.descriptor().type_name == "SyncDocument");
  REQUIRE(wrapped.descriptor().kind == wh::core::component_kind::document);

  wh::document::document_request request{};
  request.source = "hello";
  wh::core::run_context context{};
  auto result = wrapped.process(request, context);
  REQUIRE(result.has_value());
  REQUIRE(result.value().size() == 1U);
  REQUIRE(result.value().front().content() == "hello:sync");
}

TEST_CASE("document wrapper normalizes async sender results",
          "[UT][wh/document/document.hpp][document::async_process][branch]") {
  wh::document::document wrapped{async_document_impl{}};
  wh::document::document_request request{};
  request.source = "hello";
  wh::core::run_context context{};

  auto sender = wrapped.async_process(request, context);
  auto awaited = stdexec::sync_wait(std::move(sender));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<0>(*awaited).value().front().content() == "hello:async");
}

TEST_CASE("document wrapper falls back to default descriptor when impl omits one",
          "[UT][wh/document/document.hpp][document::descriptor][condition][boundary]") {
  wh::document::document wrapped{async_document_impl{}};

  const auto descriptor = wrapped.descriptor();
  REQUIRE(descriptor.type_name == "Document");
  REQUIRE(descriptor.kind == wh::core::component_kind::document);
}
