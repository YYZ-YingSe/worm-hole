#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "wh/document/processor.hpp"

TEST_CASE("document processor runs loader transformer parser pipeline",
          "[UT][wh/document/processor.hpp][document_processor::process][branch][boundary]") {
  wh::document::document_processor processor{};
  processor.set_loader([](std::string uri, const wh::document::loader_options &)
                           -> wh::core::result<std::string> {
    return "loaded:" + uri;
  });
  processor.set_transformer([](std::string value) -> wh::core::result<std::string> {
    return value + ":transformed";
  });

  wh::document::document_request request{};
  request.source_kind = wh::document::document_source_kind::uri;
  request.source = "file.txt";
  request.options.set_base(wh::document::loader_common_options{
      .parser = {.uri = "override://parser"},
  });

  wh::core::run_context context{};
  auto result = processor.process(request, context);
  REQUIRE(result.has_value());
  REQUIRE(result.value().size() == 1U);
  REQUIRE(result.value().front().content() == "loaded:file.txt:transformed");
  REQUIRE(result.value().front().metadata_or<std::string>("_source") ==
          "override://parser");
}

TEST_CASE("document processor surfaces loader and parser failures",
          "[UT][wh/document/processor.hpp][document_processor::set_loader][branch]") {
  wh::document::document_processor processor{};
  processor.set_loader([](std::string, const wh::document::loader_options &)
                           -> wh::core::result<std::string> {
    return wh::core::result<std::string>::failure(wh::core::errc::canceled);
  });

  wh::document::document_request request{};
  request.source_kind = wh::document::document_source_kind::uri;
  request.source = "file.txt";
  wh::core::run_context context{};
  auto failed = processor.process(request, context);
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::canceled);

  processor.set_loader(nullptr);
  processor.set_parser(wh::document::parser::parser{});
  request.source_kind = wh::document::document_source_kind::content;
  request.source = "hello";
  auto parser_failed = processor.process(request, context);
  REQUIRE(parser_failed.has_error());
  REQUIRE(parser_failed.error() == wh::core::errc::not_supported);
}

TEST_CASE("document processor skips loader for content requests and surfaces transformer failures",
          "[UT][wh/document/processor.hpp][document_processor::set_transformer][condition][branch]") {
  wh::document::document_processor processor{};
  std::size_t loader_calls = 0U;
  processor.set_loader([&loader_calls](std::string,
                                       const wh::document::loader_options &)
                           -> wh::core::result<std::string> {
    ++loader_calls;
    return "loaded";
  });
  processor.set_transformer([](std::string) -> wh::core::result<std::string> {
    return wh::core::result<std::string>::failure(wh::core::errc::timeout);
  });

  wh::document::document_request request{};
  request.source_kind = wh::document::document_source_kind::content;
  request.source = "inline";
  wh::core::run_context context{};

  const auto failed = processor.process(request, context);
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::timeout);
  REQUIRE(loader_calls == 0U);
}
