#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>
#include <numeric>
#include <span>
#include <string>
#include <typeindex>

#include "wh/compose/stream_concat.hpp"
#include "wh/compose/values_merge.hpp"
#include "wh/internal/concat.hpp"
#include "wh/internal/merge.hpp"

namespace {

struct unresolved_value {
  int payload{};
};

struct copy_probe {
  static inline int copy_count = 0;
  static inline int move_count = 0;

  int payload{};

  copy_probe() = default;
  explicit copy_probe(const int value) : payload(value) {}

  copy_probe(const copy_probe &other) : payload(other.payload) {
    ++copy_count;
  }
  auto operator=(const copy_probe &other) -> copy_probe & {
    payload = other.payload;
    ++copy_count;
    return *this;
  }

  copy_probe(copy_probe &&other) noexcept : payload(other.payload) {
    ++move_count;
  }
  auto operator=(copy_probe &&other) noexcept -> copy_probe & {
    payload = other.payload;
    ++move_count;
    return *this;
  }
};

namespace static_concat_ns {

struct static_concat_value {
  int sum{};
};

auto wh_stream_concat(std::span<const static_concat_value> values)
    -> wh::core::result<static_concat_value> {
  int total = 0;
  for (const auto &value : values) {
    total += value.sum;
  }
  return static_concat_value{total};
}

} // namespace static_concat_ns

} // namespace

TEST_CASE("values merge registry registration and default strategies",
          "[core][merge][condition]") {
  wh::internal::values_merge_registry registry;

  auto register_status = registry.register_merge<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        return std::accumulate(values.begin(), values.end(), 0);
      });
  REQUIRE(register_status.has_value());
  REQUIRE(registry.size() == 1U);

  const std::array<int, 3U> numeric_values{1, 2, 3};
  const auto merged_numeric = wh::compose::values_merge(
      registry, std::span<const int>{numeric_values});
  REQUIRE(merged_numeric.has_value());
  REQUIRE(merged_numeric.value() == 6);

  const auto duplicate_register = registry.register_merge<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        return values.empty() ? 0 : values.front();
      });
  REQUIRE(duplicate_register.has_error());
  REQUIRE(duplicate_register.error() == wh::core::errc::already_exists);

  const std::array<std::string, 3U> string_values{"wh", "-", "core"};
  const auto merged_string = registry.merge_as<std::string>(string_values);
  REQUIRE(merged_string.has_value());
  REQUIRE(merged_string.value() == "wh-core");

  const std::array<double, 3U> float_values{1.5, 2.5, 9.5};
  const auto merged_float = registry.merge_as<double>(float_values);
  REQUIRE(merged_float.has_value());
  REQUIRE(merged_float.value() == 9.5);

  using map_t = std::map<std::string, int>;
  const std::array<map_t, 2U> map_values{
      map_t{{"k", 1}},
      map_t{{"k", 2}},
  };
  const auto merged_map = registry.merge_as<map_t>(map_values);
  REQUIRE(merged_map.has_error());
  REQUIRE(merged_map.error() == wh::core::errc::already_exists);
}

TEST_CASE("values merge registry mismatch and fallback failures",
          "[core][merge][boundary]") {
  wh::internal::values_merge_registry registry;
  auto register_status = registry.register_merge<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        return values.empty() ? 0 : values.back();
      });
  REQUIRE(register_status.has_value());

  const std::array<wh::internal::dynamic_merge_value, 2U> mismatch_values{
      std::string{"x"},
      std::string{"y"},
  };
  const auto dynamic_merged =
      wh::compose::values_merge(registry, std::type_index(typeid(int)),
                                mismatch_values);
  REQUIRE(dynamic_merged.has_error());
  REQUIRE(dynamic_merged.error() == wh::core::errc::type_mismatch);

  const std::array<unresolved_value, 2U> unresolved_values{
      unresolved_value{1},
      unresolved_value{2},
  };
  const auto unresolved_result = registry.merge_as<unresolved_value>(
      unresolved_values);
  REQUIRE(unresolved_result.has_error());
  REQUIRE(unresolved_result.error() == wh::core::errc::contract_violation);
}

TEST_CASE("values merge typed lookup cache invalidates after registration",
          "[core][merge][condition]") {
  wh::internal::values_merge_registry registry;

  REQUIRE(registry.find_merge(std::type_index(typeid(int))) == nullptr);

  auto register_status = registry.register_merge<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        return std::accumulate(values.begin(), values.end(), 0);
      });
  REQUIRE(register_status.has_value());

  const auto *function = registry.find_merge(std::type_index(typeid(int)));
  REQUIRE(function != nullptr);

  const std::array<wh::internal::dynamic_merge_value, 3U> values{
      1,
      2,
      3,
  };
  const auto merged = registry.merge(std::type_index(typeid(int)), values);
  REQUIRE(merged.has_value());
  const auto *typed = std::any_cast<int>(&merged.value());
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == 6);
}

TEST_CASE("values merge freeze blocks late registration but keeps reads",
          "[core][merge][boundary]") {
  wh::internal::values_merge_registry registry;

  auto register_status = registry.register_merge<int>(
      [](const std::span<const int> input) -> wh::core::result<int> {
        return input.empty() ? 0 : input.back();
      });
  REQUIRE(register_status.has_value());

  registry.freeze();
  REQUIRE(registry.is_frozen());

  const auto late_register = registry.register_merge<std::string>(
      [](const std::span<const std::string> input)
          -> wh::core::result<std::string> {
        return input.empty() ? std::string{} : input.front();
      });
  REQUIRE(late_register.has_error());
  REQUIRE(late_register.error() == wh::core::errc::contract_violation);

  const std::array<int, 3U> values{1, 2, 3};
  const auto merged = registry.merge_as<int>(values);
  REQUIRE(merged.has_value());
  REQUIRE(merged.value() == 3);
}

TEST_CASE("values merge pointer registration reduces dynamic input copies",
          "[core][merge][condition]") {
  copy_probe::copy_count = 0;
  copy_probe::move_count = 0;

  wh::internal::values_merge_registry registry;
  auto register_status = registry.register_merge_from_ptrs<copy_probe>(
      [](const std::span<const copy_probe *> input)
          -> wh::core::result<copy_probe> {
        int total = 0;
        for (const auto *value : input) {
          total += value->payload;
        }
        return copy_probe{total};
      });
  REQUIRE(register_status.has_value());

  const std::array<wh::internal::dynamic_merge_value, 3U> dynamic_values{
      copy_probe{1},
      copy_probe{2},
      copy_probe{3},
  };

  const auto copy_before = copy_probe::copy_count;
  const auto merged_dynamic =
      registry.merge(std::type_index(typeid(copy_probe)), dynamic_values);
  REQUIRE(merged_dynamic.has_value());
  const auto *dynamic_typed = std::any_cast<copy_probe>(&merged_dynamic.value());
  REQUIRE(dynamic_typed != nullptr);
  REQUIRE(dynamic_typed->payload == 6);
  const auto copy_after = copy_probe::copy_count;
  REQUIRE(copy_after - copy_before <= 1);
}

TEST_CASE("values merge pointer registration handles large dynamic batches",
          "[core][merge][condition]") {
  wh::internal::values_merge_registry registry;
  auto register_status = registry.register_merge_from_ptrs<int>(
      [](const std::span<const int *> input) -> wh::core::result<int> {
        int total = 0;
        for (const auto *value : input) {
          total += *value;
        }
        return total;
      });
  REQUIRE(register_status.has_value());

  const std::array<wh::internal::dynamic_merge_value, 12U> values{
      1,  2,  3, 4,  5,  6,
      7,  8,  9, 10, 11, 12,
  };
  const auto merged = registry.merge(std::type_index(typeid(int)), values);
  REQUIRE(merged.has_value());
  const auto *typed = std::any_cast<int>(&merged.value());
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == 78);
}

TEST_CASE("stream concat registry registration and recursive map strategy",
          "[core][concat][condition]") {
  wh::internal::stream_concat_registry registry;

  auto register_status = registry.register_concat<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        return std::accumulate(values.begin(), values.end(), 0);
      });
  REQUIRE(register_status.has_value());
  REQUIRE(registry.size() == 1U);

  const std::array<int, 3U> numeric_values{1, 2, 3};
  const auto concat_numeric = wh::compose::stream_concat(
      registry, std::span<const int>{numeric_values});
  REQUIRE(concat_numeric.has_value());
  REQUIRE(concat_numeric.value() == 6);

  const auto duplicate_register = registry.register_concat<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        return values.empty() ? 0 : values.front();
      });
  REQUIRE(duplicate_register.has_error());
  REQUIRE(duplicate_register.error() == wh::core::errc::already_exists);

  const std::array<std::string, 3U> string_values{"a", "b", "c"};
  const auto concat_string = registry.concat_as<std::string>(string_values);
  REQUIRE(concat_string.has_value());
  REQUIRE(concat_string.value() == "abc");

  using nested_map_t = std::map<std::string, std::map<std::string, std::string>>;
  const std::array<nested_map_t, 2U> nested_values{
      nested_map_t{{"root", {{"leaf", "left"}}}},
      nested_map_t{{"root", {{"leaf", "-right"}, {"other", "x"}}}},
  };
  const auto concat_nested = registry.concat_as<nested_map_t>(nested_values);
  REQUIRE(concat_nested.has_value());
  REQUIRE(concat_nested.value().at("root").at("leaf") == "left-right");
  REQUIRE(concat_nested.value().at("root").at("other") == "x");
}

TEST_CASE("stream concat registry mismatch and unresolved recursive conflicts",
          "[core][concat][boundary]") {
  wh::internal::stream_concat_registry registry;

  auto register_status = registry.register_concat<int>(
      [](const std::span<const int> values) -> wh::core::result<int> {
        return values.empty() ? 0 : values.back();
      });
  REQUIRE(register_status.has_value());

  const std::array<wh::internal::dynamic_stream_chunk, 2U> mismatch_values{
      std::string{"x"},
      std::string{"y"},
  };
  const auto dynamic_merged =
      wh::compose::stream_concat(registry, std::type_index(typeid(int)),
                                 mismatch_values);
  REQUIRE(dynamic_merged.has_error());
  REQUIRE(dynamic_merged.error() == wh::core::errc::type_mismatch);

  using unresolved_map_t = std::map<std::string, unresolved_value>;
  const std::array<unresolved_map_t, 2U> unresolved_values{
      unresolved_map_t{{"same", unresolved_value{1}}},
      unresolved_map_t{{"same", unresolved_value{2}}},
  };
  const auto unresolved_concat =
      registry.concat_as<unresolved_map_t>(unresolved_values);
  REQUIRE(unresolved_concat.has_error());
  REQUIRE(unresolved_concat.error() == wh::core::errc::contract_violation);
}

TEST_CASE("stream concat supports compile-time specialization before registry",
          "[core][concat][condition]") {
  wh::internal::stream_concat_registry registry;

  const std::array<static_concat_ns::static_concat_value, 3U> values{
      static_concat_ns::static_concat_value{1},
      static_concat_ns::static_concat_value{2},
      static_concat_ns::static_concat_value{3},
  };
  const auto merged = registry.concat_as<static_concat_ns::static_concat_value>(
      std::span<const static_concat_ns::static_concat_value>{values});
  REQUIRE(merged.has_value());
  REQUIRE(merged.value().sum == 6);
  REQUIRE(registry.size() == 0U);
}

TEST_CASE("stream concat typed lookup cache remains correct after registration",
          "[core][concat][condition]") {
  wh::internal::stream_concat_registry registry;

  const std::array<int, 3U> values{1, 2, 3};
  const auto fallback_result = registry.concat_as<int>(values);
  REQUIRE(fallback_result.has_value());
  REQUIRE(fallback_result.value() == 3);

  auto register_status = registry.register_concat<int>(
      [](const std::span<const int> input) -> wh::core::result<int> {
        return std::accumulate(input.begin(), input.end(), 0);
      });
  REQUIRE(register_status.has_value());

  const auto registered_result = registry.concat_as<int>(values);
  REQUIRE(registered_result.has_value());
  REQUIRE(registered_result.value() == 6);
}

TEST_CASE("stream concat freeze blocks late registration but keeps reads",
          "[core][concat][boundary]") {
  wh::internal::stream_concat_registry registry;

  auto register_status = registry.register_concat<int>(
      [](const std::span<const int> input) -> wh::core::result<int> {
        return input.empty() ? 0 : input.back();
      });
  REQUIRE(register_status.has_value());

  registry.freeze();
  REQUIRE(registry.is_frozen());

  const auto late_register = registry.register_concat<std::string>(
      [](const std::span<const std::string> input)
          -> wh::core::result<std::string> {
        return input.empty() ? std::string{} : input.front();
      });
  REQUIRE(late_register.has_error());
  REQUIRE(late_register.error() == wh::core::errc::contract_violation);

  const std::array<int, 3U> values{1, 2, 3};
  const auto merged = registry.concat_as<int>(values);
  REQUIRE(merged.has_value());
  REQUIRE(merged.value() == 3);
}

TEST_CASE("stream concat pointer registration reduces dynamic input copies",
          "[core][concat][condition]") {
  copy_probe::copy_count = 0;
  copy_probe::move_count = 0;

  wh::internal::stream_concat_registry registry;
  auto register_status = registry.register_concat_from_ptrs<copy_probe>(
      [](const std::span<const copy_probe *> input)
          -> wh::core::result<copy_probe> {
        int total = 0;
        for (const auto *value : input) {
          total += value->payload;
        }
        return copy_probe{total};
      });
  REQUIRE(register_status.has_value());

  const std::array<wh::internal::dynamic_stream_chunk, 3U> dynamic_values{
      copy_probe{1},
      copy_probe{2},
      copy_probe{3},
  };

  const auto copy_before = copy_probe::copy_count;
  const auto merged_dynamic = wh::compose::stream_concat(
      registry, std::type_index(typeid(copy_probe)), dynamic_values);
  REQUIRE(merged_dynamic.has_value());
  const auto *dynamic_typed = std::any_cast<copy_probe>(&merged_dynamic.value());
  REQUIRE(dynamic_typed != nullptr);
  REQUIRE(dynamic_typed->payload == 6);
  const auto copy_after = copy_probe::copy_count;
  REQUIRE(copy_after - copy_before <= 1);
}

TEST_CASE("stream concat dynamic lookup cache invalidates after registration",
          "[core][concat][condition]") {
  wh::internal::stream_concat_registry registry;

  REQUIRE(registry.find_concat(std::type_index(typeid(int))) == nullptr);

  auto register_status = registry.register_concat<int>(
      [](const std::span<const int> input) -> wh::core::result<int> {
        return std::accumulate(input.begin(), input.end(), 0);
      });
  REQUIRE(register_status.has_value());

  const auto *function = registry.find_concat(std::type_index(typeid(int)));
  REQUIRE(function != nullptr);

  const std::array<wh::internal::dynamic_stream_chunk, 3U> values{
      1,
      2,
      3,
  };
  const auto merged = registry.concat(std::type_index(typeid(int)), values);
  REQUIRE(merged.has_value());
  const auto *typed = std::any_cast<int>(&merged.value());
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == 6);
}

TEST_CASE("stream concat pointer registration handles large dynamic batches",
          "[core][concat][condition]") {
  wh::internal::stream_concat_registry registry;
  auto register_status = registry.register_concat_from_ptrs<int>(
      [](const std::span<const int *> input) -> wh::core::result<int> {
        int total = 0;
        for (const auto *value : input) {
          total += *value;
        }
        return total;
      });
  REQUIRE(register_status.has_value());

  const std::array<wh::internal::dynamic_stream_chunk, 12U> values{
      1,  2,  3, 4,  5,  6,
      7,  8,  9, 10, 11, 12,
  };
  const auto merged = registry.concat(std::type_index(typeid(int)), values);
  REQUIRE(merged.has_value());
  const auto *typed = std::any_cast<int>(&merged.value());
  REQUIRE(typed != nullptr);
  REQUIRE(*typed == 78);
}
