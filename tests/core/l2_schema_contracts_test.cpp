#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "wh/core/json.hpp"
#include "wh/schema/document.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/tool.hpp"

TEST_CASE("message merge validates identity and merges tool fragments",
          "[core][l2][message][condition]") {
  wh::schema::message first{};
  first.message_id = "m1";
  first.role = wh::schema::message_role::assistant;
  first.name = "assistant-1";
  first.parts.emplace_back(wh::schema::text_part{"hello "});
  first.parts.emplace_back(wh::schema::tool_call_part{
      .index = 0U,
      .id = "call-1",
      .type = "function",
      .name = "calc",
      .arguments = R"({"a":)",
      .complete = false,
  });
  first.meta.usage.prompt_tokens = 8;
  first.meta.usage.total_tokens = 10;

  wh::schema::message second{};
  second.message_id = "m1";
  second.role = wh::schema::message_role::assistant;
  second.name = "assistant-1";
  second.parts.emplace_back(wh::schema::text_part{"world"});
  second.parts.emplace_back(wh::schema::tool_call_part{
      .index = 0U,
      .id = "call-1",
      .type = "function",
      .name = "calc",
      .arguments = R"(1})",
      .complete = true,
  });
  second.meta.finish_reason = "stop";
  second.meta.usage.prompt_tokens = 7;
  second.meta.usage.completion_tokens = 9;
  second.meta.usage.total_tokens = 17;

  const std::array<wh::schema::message, 2U> chunks{first, second};
  const auto merged = wh::schema::merge_message_chunks(chunks);
  REQUIRE(merged.has_value());
  REQUIRE(merged.value().parts.size() == 2U);
  REQUIRE(std::get<wh::schema::text_part>(merged.value().parts.front()).text ==
          "hello world");

  const auto &tool_call =
      std::get<wh::schema::tool_call_part>(merged.value().parts.back());
  REQUIRE(tool_call.id == "call-1");
  REQUIRE(tool_call.name == "calc");
  REQUIRE(tool_call.arguments == R"({"a":1})");
  REQUIRE(tool_call.complete == false);

  REQUIRE(merged.value().meta.finish_reason == "stop");
  REQUIRE(merged.value().meta.usage.prompt_tokens == 8);
  REQUIRE(merged.value().meta.usage.completion_tokens == 9);
  REQUIRE(merged.value().meta.usage.total_tokens == 17);
}

TEST_CASE("message template strict mode reports missing placeholders",
          "[core][l2][message][boundary]") {
  wh::schema::placeholder_context context{};
  context.emplace("name", wh::schema::placeholder_value{"worm-hole", false});
  context.emplace("optional_note", wh::schema::placeholder_value{"", true});
  context.emplace("count", wh::schema::placeholder_value{
                               "abc", false, wh::schema::placeholder_type::number});

  const auto rendered = wh::schema::render_message_template(
      "hello {{ name }}{{ optional_note }}", context);
  REQUIRE(rendered.has_value());
  REQUIRE(rendered.value() == "hello worm-hole");

  const auto rendered_logic = wh::schema::render_message_template(
      "logic ${name}", context, wh::schema::template_format::logic);
  REQUIRE(rendered_logic.has_value());
  REQUIRE(rendered_logic.value() == "logic worm-hole");

  const auto missing =
      wh::schema::render_message_template("{{missing}}", context);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);

  const auto type_mismatch =
      wh::schema::render_message_template("{{count}}", context);
  REQUIRE(type_mismatch.has_error());
  REQUIRE(type_mismatch.error() == wh::core::errc::type_mismatch);

  const auto script_forbidden = wh::schema::render_message_template(
      "{% include 'x' %}", context, wh::schema::template_format::script);
  REQUIRE(script_forbidden.has_error());
  REQUIRE(script_forbidden.error() == wh::core::errc::contract_violation);
}

TEST_CASE("message upsert keeps role consistency",
          "[core][l2][message][branch]") {
  std::vector<wh::schema::message> timeline{};
  std::vector<wh::schema::message_update_audit_entry> audit{};

  wh::schema::message initial{};
  initial.message_id = "id-1";
  initial.role = wh::schema::message_role::assistant;
  initial.parts.emplace_back(wh::schema::text_part{"v1"});
  auto inserted = wh::schema::apply_message_update(
      timeline, initial, wh::schema::message_update_mode::upsert_by_id, &audit);
  REQUIRE(inserted.has_value());
  REQUIRE(timeline.size() == 1U);
  REQUIRE(audit.size() == 1U);
  REQUIRE(audit.back().action == wh::schema::message_update_action::inserted);

  wh::schema::message updated = initial;
  updated.parts.clear();
  updated.parts.emplace_back(wh::schema::text_part{"v2"});
  auto upserted = wh::schema::apply_message_update(
      timeline, updated, wh::schema::message_update_mode::upsert_by_id, &audit);
  REQUIRE(upserted.has_value());
  REQUIRE(
      std::get<wh::schema::text_part>(timeline.front().parts.front()).text ==
      "v2");
  REQUIRE(audit.back().action == wh::schema::message_update_action::replaced);

  wh::schema::message cross_role = updated;
  cross_role.role = wh::schema::message_role::tool;
  const auto rejected = wh::schema::apply_message_update(
      timeline, cross_role, wh::schema::message_update_mode::upsert_by_id,
      &audit);
  REQUIRE(rejected.has_error());
  REQUIRE(rejected.error() == wh::core::errc::contract_violation);
  REQUIRE(audit.back().action == wh::schema::message_update_action::rejected);
  REQUIRE(audit.back().error.code() == wh::core::errc::contract_violation);
}

TEST_CASE("document metadata supports typed and fallback access",
          "[core][l2][document][condition]") {
  wh::schema::document doc{"hello"};
  doc.with_score(0.95)
      .with_sub_index("segment-1")
      .with_dsl("lang:cpp")
      .with_extra_info("unit-test")
      .with_dense_vector({1.0, 2.0, 3.0})
      .with_sparse_vector({{0U, 0.4}, {3U, 0.8}});

  REQUIRE(doc.content() == "hello");
  REQUIRE(std::abs(doc.score() - 0.95) < 1e-12);
  REQUIRE(doc.sub_index() == "segment-1");
  REQUIRE(doc.dsl() == "lang:cpp");
  REQUIRE(doc.extra_info() == "unit-test");
  REQUIRE(doc.get_dense_vector().size() == 3U);
  REQUIRE(doc.get_sparse_vector().size() == 2U);

  const auto missing = doc.metadata_or<std::string>("missing", "fallback");
  REQUIRE(missing == "fallback");
  const auto wrong_type = doc.metadata_or<std::int64_t>(
      wh::schema::document_metadata_keys::sub_index, 7);
  REQUIRE(wrong_type == 7);

  const auto sub_index_ref =
      doc.metadata_cref<std::string>(wh::schema::document_metadata_keys::sub_index);
  REQUIRE(sub_index_ref.has_value());
  REQUIRE(sub_index_ref.value().get() == "segment-1");

  const auto missing_ref = doc.metadata_cref<std::string>("missing");
  REQUIRE(missing_ref.has_error());
  REQUIRE(missing_ref.error() == wh::core::errc::not_found);

  const auto mismatch_ref =
      doc.metadata_cref<std::int64_t>(wh::schema::document_metadata_keys::sub_index);
  REQUIRE(mismatch_ref.has_error());
  REQUIRE(mismatch_ref.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("tool schema builder produces stable object schema",
          "[core][l2][tool][condition]") {
  wh::schema::tool_parameter_schema city{};
  city.name = "city";
  city.type = wh::schema::tool_parameter_type::string;
  city.required = true;
  city.description = "City name";

  wh::schema::tool_parameter_schema unit{};
  unit.name = "unit";
  unit.type = wh::schema::tool_parameter_type::string;
  unit.enum_values = {"celsius", "fahrenheit"};

  wh::schema::tool_schema_definition tool{};
  tool.name = "get_weather";
  tool.description = "Fetch weather info";
  tool.parameters = {unit, city};
  tool.choice =
      wh::schema::tool_choice{wh::schema::tool_call_mode::force, "get_weather"};

  const auto schema = wh::schema::build_tool_json_schema(tool);
  REQUIRE(schema.has_value());
  const auto function = schema.value().FindMember("function");
  REQUIRE(function != schema.value().MemberEnd());
  REQUIRE(function->value.IsObject());

  const auto parameters = function->value.FindMember("parameters");
  REQUIRE(parameters != function->value.MemberEnd());
  REQUIRE(parameters->value.IsObject());
  const auto properties = parameters->value.FindMember("properties");
  REQUIRE(properties != parameters->value.MemberEnd());
  REQUIRE(properties->value.IsObject());
  REQUIRE(properties->value.MemberCount() == 2U);

  const auto required = parameters->value.FindMember("required");
  REQUIRE(required != parameters->value.MemberEnd());
  REQUIRE(required->value.IsArray());
  REQUIRE(required->value.Size() == 1U);
  REQUIRE(std::string_view(required->value[0].GetString(),
                           required->value[0].GetStringLength()) == "city");
}

TEST_CASE("tool schema supports raw schema and validates choice",
          "[core][l2][tool][boundary]") {
  wh::schema::tool_schema_definition raw{};
  raw.name = "raw_tool";
  raw.description = "raw";
  raw.raw_parameters_json_schema = R"({"type":"object","properties":{}})";

  const auto raw_schema = wh::schema::build_tool_json_schema(raw);
  REQUIRE(raw_schema.has_value());

  wh::schema::tool_schema_definition invalid{};
  invalid.name = "broken";
  invalid.choice =
      wh::schema::tool_choice{wh::schema::tool_call_mode::force, ""};
  const auto invalid_schema = wh::schema::build_tool_json_schema(invalid);
  REQUIRE(invalid_schema.has_error());
  REQUIRE(invalid_schema.error() == wh::core::errc::invalid_argument);
}
