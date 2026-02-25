#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "wh/core/small_vector.hpp"

namespace {

struct non_trivial_value {
  std::string payload;
  std::uint32_t tag{0U};

  non_trivial_value() = default;

  non_trivial_value(std::string text, const std::uint32_t id)
      : payload(std::move(text)), tag(id) {}
};

[[nodiscard]] auto make_int_input(const std::size_t count) -> std::vector<int> {
  std::vector<int> values(count);
  for (std::size_t index = 0U; index < count; ++index) {
    values[index] = static_cast<int>(index % 251U);
  }
  return values;
}

[[nodiscard]] auto make_payload_input(const std::size_t count)
    -> std::vector<non_trivial_value> {
  std::vector<non_trivial_value> values;
  values.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    values.emplace_back("payload_" + std::to_string(index),
                        static_cast<std::uint32_t>(index));
  }
  return values;
}

template <typename container_t>
[[nodiscard]] auto checksum_int_range(const container_t &values)
    -> std::uint64_t {
  std::uint64_t checksum = 0U;
  for (const auto value : values) {
    checksum = (checksum * 1315423911ULL) ^
               static_cast<std::uint64_t>(static_cast<std::uint32_t>(value));
  }
  return checksum;
}

template <typename container_t>
[[nodiscard]] auto checksum_payload_range(const container_t &values)
    -> std::uint64_t {
  std::uint64_t checksum = 0U;
  for (const auto &value : values) {
    checksum =
        (checksum * 2654435761ULL) ^
        static_cast<std::uint64_t>(
            value.tag + static_cast<std::uint32_t>(value.payload.size()));
  }
  return checksum;
}

void set_items_processed(benchmark::State &state,
                         const std::size_t items_per_iteration) {
  const auto iterations = static_cast<int64_t>(state.iterations());
  const auto elements = static_cast<int64_t>(items_per_iteration);
  state.SetItemsProcessed(iterations * elements);
}

void BM_std_vector_push_back_reserved(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto input = make_int_input(element_count);

  for (auto _ : state) {
    std::vector<int> output;
    output.reserve(element_count);
    for (const int value : input) {
      output.push_back(value);
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_small_vector_push_back_reserved(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto input = make_int_input(element_count);

  for (auto _ : state) {
    wh::core::small_vector<int, 64U> output;
    static_cast<void>(
        output.reserve(static_cast<wh::core::small_vector<int, 64U>::size_type>(
            element_count)));

    for (const int value : input) {
      static_cast<void>(output.push_back(value));
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_std_vector_push_back_growth(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto input = make_int_input(element_count);

  for (auto _ : state) {
    std::vector<int> output;
    for (const int value : input) {
      output.push_back(value);
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_small_vector_push_back_growth(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto input = make_int_input(element_count);

  for (auto _ : state) {
    wh::core::small_vector<int, 64U> output;
    for (const int value : input) {
      static_cast<void>(output.push_back(value));
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_std_vector_emplace_back_non_trivial_reserved(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    std::vector<non_trivial_value> output;
    output.reserve(element_count);
    for (std::size_t index = 0U; index < element_count; ++index) {
      output.emplace_back("payload_" + std::to_string(index),
                          static_cast<std::uint32_t>(index));
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_small_vector_emplace_back_non_trivial_reserved(
    benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    wh::core::small_vector<non_trivial_value, 64U> output;
    static_cast<void>(output.reserve(
        static_cast<wh::core::small_vector<non_trivial_value, 64U>::size_type>(
            element_count)));

    for (std::size_t index = 0U; index < element_count; ++index) {
      static_cast<void>(output.emplace_back("payload_" + std::to_string(index),
                                            static_cast<std::uint32_t>(index)));
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_std_vector_insert_fill_non_trivial(benchmark::State &state) {
  const auto base_size = static_cast<std::size_t>(state.range(0));
  const auto insert_count = static_cast<std::size_t>(state.range(1));
  const auto base_data = make_payload_input(base_size);
  const non_trivial_value fill_value{"inserted_payload", 42U};

  for (auto _ : state) {
    auto output = base_data;
    output.insert(output.begin() + static_cast<std::ptrdiff_t>(base_size / 2U),
                  insert_count, fill_value);

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, base_size + insert_count);
}

void BM_small_vector_insert_fill_non_trivial(benchmark::State &state) {
  const auto base_size = static_cast<std::size_t>(state.range(0));
  const auto insert_count = static_cast<std::size_t>(state.range(1));
  const auto base_data = make_payload_input(base_size);
  const non_trivial_value fill_value{"inserted_payload", 42U};

  using container_t = wh::core::small_vector<non_trivial_value, 64U>;

  for (auto _ : state) {
    container_t output(base_data.begin(), base_data.end());
    static_cast<void>(output.insert(
        output.begin() + static_cast<std::ptrdiff_t>(base_size / 2U),
        static_cast<container_t::size_type>(insert_count), fill_value));

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, base_size + insert_count);
}

void BM_std_vector_insert_range_non_trivial(benchmark::State &state) {
  const auto base_size = static_cast<std::size_t>(state.range(0));
  const auto insert_count = static_cast<std::size_t>(state.range(1));
  const auto base_data = make_payload_input(base_size);
  const auto insert_data = make_payload_input(insert_count);

  for (auto _ : state) {
    auto output = base_data;
    output.insert(output.begin() + static_cast<std::ptrdiff_t>(base_size / 2U),
                  insert_data.begin(), insert_data.end());

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, base_size + insert_count);
}

void BM_small_vector_insert_range_non_trivial(benchmark::State &state) {
  const auto base_size = static_cast<std::size_t>(state.range(0));
  const auto insert_count = static_cast<std::size_t>(state.range(1));
  const auto base_data = make_payload_input(base_size);
  const auto insert_data = make_payload_input(insert_count);

  using container_t = wh::core::small_vector<non_trivial_value, 64U>;

  for (auto _ : state) {
    container_t output(base_data.begin(), base_data.end());
    static_cast<void>(output.insert(
        output.begin() + static_cast<std::ptrdiff_t>(base_size / 2U),
        insert_data.begin(), insert_data.end()));

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, base_size + insert_count);
}

void BM_std_vector_insert_range_trivial(benchmark::State &state) {
  const auto base_size = static_cast<std::size_t>(state.range(0));
  const auto insert_count = static_cast<std::size_t>(state.range(1));
  const auto base_data = make_int_input(base_size);
  const auto insert_data = make_int_input(insert_count);

  for (auto _ : state) {
    auto output = base_data;
    output.insert(output.begin() + static_cast<std::ptrdiff_t>(base_size / 2U),
                  insert_data.begin(), insert_data.end());

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, base_size + insert_count);
}

void BM_small_vector_insert_range_trivial(benchmark::State &state) {
  const auto base_size = static_cast<std::size_t>(state.range(0));
  const auto insert_count = static_cast<std::size_t>(state.range(1));
  const auto base_data = make_int_input(base_size);
  const auto insert_data = make_int_input(insert_count);

  using container_t = wh::core::small_vector<int, 64U>;

  for (auto _ : state) {
    container_t output(base_data.begin(), base_data.end());
    static_cast<void>(output.insert(
        output.begin() + static_cast<std::ptrdiff_t>(base_size / 2U),
        insert_data.begin(), insert_data.end()));

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, base_size + insert_count);
}

void BM_std_vector_assign_range_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto input = make_int_input(element_count);

  for (auto _ : state) {
    std::vector<int> output;
    output.assign(input.begin(), input.end());

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_small_vector_assign_range_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto input = make_int_input(element_count);

  for (auto _ : state) {
    wh::core::small_vector<int, 64U> output;
    static_cast<void>(output.assign(input.begin(), input.end()));

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_std_vector_erase_middle_non_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto base_data = make_payload_input(element_count);

  for (auto _ : state) {
    auto output = base_data;
    const auto first =
        output.begin() + static_cast<std::ptrdiff_t>(element_count / 4U);
    const auto last =
        output.begin() + static_cast<std::ptrdiff_t>((element_count * 3U) / 4U);
    output.erase(first, last);

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_small_vector_erase_middle_non_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto base_data = make_payload_input(element_count);

  using container_t = wh::core::small_vector<non_trivial_value, 64U>;

  for (auto _ : state) {
    container_t output(base_data.begin(), base_data.end());
    const auto first =
        output.begin() + static_cast<std::ptrdiff_t>(element_count / 4U);
    const auto last =
        output.begin() + static_cast<std::ptrdiff_t>((element_count * 3U) / 4U);
    output.erase(first, last);

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_std_vector_copy_construct_non_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto source = make_payload_input(element_count);

  for (auto _ : state) {
    std::vector<non_trivial_value> output(source);

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_small_vector_copy_construct_non_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto source_values = make_payload_input(element_count);
  wh::core::small_vector<non_trivial_value, 64U> source(source_values.begin(),
                                                        source_values.end());

  for (auto _ : state) {
    wh::core::small_vector<non_trivial_value, 64U> output(source);

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_std_vector_copy_assign_non_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto source = make_payload_input(element_count);

  for (auto _ : state) {
    std::vector<non_trivial_value> output;
    output = source;

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_small_vector_copy_assign_non_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto source_values = make_payload_input(element_count);
  wh::core::small_vector<non_trivial_value, 64U> source(source_values.begin(),
                                                        source_values.end());

  for (auto _ : state) {
    wh::core::small_vector<non_trivial_value, 64U> output;
    output = source;

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_payload_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_std_vector_resize_grow_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    std::vector<int> output;
    output.resize(element_count);

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_small_vector_resize_grow_trivial(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    wh::core::small_vector<int, 64U> output;
    static_cast<void>(
        output.resize(static_cast<wh::core::small_vector<int, 64U>::size_type>(
            element_count)));

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_std_vector_clear_reuse_push_back(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto input = make_int_input(element_count);
  std::vector<int> output;
  output.reserve(element_count);

  for (auto _ : state) {
    output.clear();
    for (const int value : input) {
      output.push_back(value);
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

void BM_small_vector_clear_reuse_push_back(benchmark::State &state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const auto input = make_int_input(element_count);
  wh::core::small_vector<int, 64U> output;
  static_cast<void>(output.reserve(
      static_cast<wh::core::small_vector<int, 64U>::size_type>(element_count)));

  for (auto _ : state) {
    output.clear();
    for (const int value : input) {
      static_cast<void>(output.push_back(value));
    }

    benchmark::DoNotOptimize(output.data());
    benchmark::DoNotOptimize(checksum_int_range(output));
  }

  set_items_processed(state, element_count);
}

} // namespace

BENCHMARK(BM_std_vector_push_back_reserved)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384);
BENCHMARK(BM_small_vector_push_back_reserved)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384);

BENCHMARK(BM_std_vector_push_back_growth)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384);
BENCHMARK(BM_small_vector_push_back_growth)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384);

BENCHMARK(BM_std_vector_emplace_back_non_trivial_reserved)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Arg(512)
    ->Arg(2048)
    ->Arg(8192);
BENCHMARK(BM_small_vector_emplace_back_non_trivial_reserved)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Arg(512)
    ->Arg(2048)
    ->Arg(8192);

BENCHMARK(BM_std_vector_insert_fill_non_trivial)
    ->Args({512, 16})
    ->Args({2048, 32})
    ->Args({8192, 64});
BENCHMARK(BM_small_vector_insert_fill_non_trivial)
    ->Args({512, 16})
    ->Args({2048, 32})
    ->Args({8192, 64});

BENCHMARK(BM_std_vector_insert_range_non_trivial)
    ->Args({512, 16})
    ->Args({2048, 32})
    ->Args({8192, 64});
BENCHMARK(BM_small_vector_insert_range_non_trivial)
    ->Args({512, 16})
    ->Args({2048, 32})
    ->Args({8192, 64});

BENCHMARK(BM_std_vector_insert_range_trivial)
    ->Args({512, 16})
    ->Args({2048, 32})
    ->Args({8192, 64});
BENCHMARK(BM_small_vector_insert_range_trivial)
    ->Args({512, 16})
    ->Args({2048, 32})
    ->Args({8192, 64});

BENCHMARK(BM_std_vector_assign_range_trivial)->Arg(1024)->Arg(4096)->Arg(16384);
BENCHMARK(BM_small_vector_assign_range_trivial)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384);

BENCHMARK(BM_std_vector_erase_middle_non_trivial)
    ->Arg(512)
    ->Arg(2048)
    ->Arg(8192);
BENCHMARK(BM_small_vector_erase_middle_non_trivial)
    ->Arg(512)
    ->Arg(2048)
    ->Arg(8192);

BENCHMARK(BM_std_vector_copy_construct_non_trivial)
    ->Arg(512)
    ->Arg(2048)
    ->Arg(8192);
BENCHMARK(BM_small_vector_copy_construct_non_trivial)
    ->Arg(512)
    ->Arg(2048)
    ->Arg(8192);

BENCHMARK(BM_std_vector_copy_assign_non_trivial)
    ->Arg(512)
    ->Arg(2048)
    ->Arg(8192);
BENCHMARK(BM_small_vector_copy_assign_non_trivial)
    ->Arg(512)
    ->Arg(2048)
    ->Arg(8192);

BENCHMARK(BM_std_vector_resize_grow_trivial)->Arg(1024)->Arg(4096)->Arg(16384);
BENCHMARK(BM_small_vector_resize_grow_trivial)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384);

BENCHMARK(BM_std_vector_clear_reuse_push_back)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384);
BENCHMARK(BM_small_vector_clear_reuse_push_back)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384);

BENCHMARK_MAIN();
