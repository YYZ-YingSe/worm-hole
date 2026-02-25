#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "wh/core/mpmc_queue.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/core/types/mpmc_queue_types.hpp"
#include "wh/core/types/small_vector_types.hpp"

namespace {

template <typename value_t> struct tracking_allocator {
  using value_type = value_t;

  int id{0};

  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  tracking_allocator() = default;

  explicit tracking_allocator(const int identity) : id(identity) {}

  template <typename other_t>
  tracking_allocator(const tracking_allocator<other_t> &other) : id(other.id) {}

  [[nodiscard]] auto allocate(const std::size_t count) -> value_t * {
    return std::allocator<value_t>{}.allocate(count);
  }

  void deallocate(value_t *pointer, const std::size_t count) noexcept {
    std::allocator<value_t>{}.deallocate(pointer, count);
  }

  template <typename other_t>
  [[nodiscard]] auto
  operator==(const tracking_allocator<other_t> &other) const noexcept -> bool {
    return id == other.id;
  }

  template <typename other_t>
  [[nodiscard]] auto
  operator!=(const tracking_allocator<other_t> &other) const noexcept -> bool {
    return !(*this == other);
  }
};

template <typename value_t> struct non_propagating_allocator {
  using value_type = value_t;

  int id{0};

  using propagate_on_container_copy_assignment = std::false_type;
  using propagate_on_container_move_assignment = std::false_type;
  using propagate_on_container_swap = std::false_type;

  non_propagating_allocator() = default;

  explicit non_propagating_allocator(const int identity) : id(identity) {}

  template <typename other_t>
  non_propagating_allocator(const non_propagating_allocator<other_t> &other)
      : id(other.id) {}

  [[nodiscard]] auto allocate(const std::size_t count) -> value_t * {
    return std::allocator<value_t>{}.allocate(count);
  }

  void deallocate(value_t *pointer, const std::size_t count) noexcept {
    std::allocator<value_t>{}.deallocate(pointer, count);
  }

  template <typename other_t>
  [[nodiscard]] auto
  operator==(const non_propagating_allocator<other_t> &other) const noexcept
      -> bool {
    return id == other.id;
  }

  template <typename other_t>
  [[nodiscard]] auto
  operator!=(const non_propagating_allocator<other_t> &other) const noexcept
      -> bool {
    return !(*this == other);
  }
};

struct queue_item {
  std::size_t producer{};
  std::size_t sequence{};
};

struct queue_allocator_counters {
  static inline std::atomic<std::size_t> allocations{0U};
  static inline std::atomic<std::size_t> deallocations{0U};

  static void reset() {
    allocations.store(0U, std::memory_order_relaxed);
    deallocations.store(0U, std::memory_order_relaxed);
  }
};

template <typename value_t> struct queue_tracking_allocator {
  using value_type = value_t;

  int id{0};

  queue_tracking_allocator() = default;
  explicit queue_tracking_allocator(const int allocator_id) : id(allocator_id) {}

  template <typename other_t>
  queue_tracking_allocator(const queue_tracking_allocator<other_t> &other)
      : id(other.id) {}

  [[nodiscard]] auto allocate(const std::size_t count) -> value_t * {
    queue_allocator_counters::allocations.fetch_add(1U,
                                                    std::memory_order_relaxed);
    return std::allocator<value_t>{}.allocate(count);
  }

  void deallocate(value_t *pointer, const std::size_t count) noexcept {
    queue_allocator_counters::deallocations.fetch_add(
        1U, std::memory_order_relaxed);
    std::allocator<value_t>{}.deallocate(pointer, count);
  }

  template <typename other_t>
  [[nodiscard]] auto
  operator==(const queue_tracking_allocator<other_t> &other) const noexcept
      -> bool {
    return id == other.id;
  }

  template <typename other_t>
  [[nodiscard]] auto
  operator!=(const queue_tracking_allocator<other_t> &other) const noexcept
      -> bool {
    return !(*this == other);
  }
};

struct default_only_value {
  int value{0};

  default_only_value() = default;
  explicit default_only_value(const int input) : value(input) {}
  default_only_value(const default_only_value &) = delete;
  auto operator=(const default_only_value &) -> default_only_value & = delete;
  default_only_value(default_only_value &&) noexcept = default;
  auto operator=(default_only_value &&) noexcept
      -> default_only_value & = default;
};

} // namespace

TEST_CASE("small_vector iterators and mutation contracts",
          "[core][small_vector][condition]") {
  wh::core::small_vector<int, 3U> values{1, 2, 3};

  REQUIRE(values.front() == 1);
  REQUIRE(values.back() == 3);
  const auto at_value = values.at(1);
  REQUIRE(at_value.has_value());
  REQUIRE(at_value.value() == 2);

  const auto cbegin = values.cbegin();
  const auto cend = values.cend();
  REQUIRE(std::distance(cbegin, cend) == 3);

  const auto reverse = std::vector<int>(values.rbegin(), values.rend());
  REQUIRE(reverse == std::vector<int>{3, 2, 1});

  const auto inserted = values.insert(values.begin() + 1, 99);
  REQUIRE(inserted.has_value());
  REQUIRE(*inserted.value() == 99);
  REQUIRE(values.to_std_vector() == std::vector<int>{1, 99, 2, 3});

  const auto erased = values.erase(values.begin() + 2);
  REQUIRE(*erased == 3);
  REQUIRE(values.to_std_vector() == std::vector<int>{1, 99, 3});
}

TEST_CASE("small_vector erase range and free erase helpers",
          "[core][small_vector][condition]") {
  wh::core::small_vector<int, 8U> values{1, 2, 3, 2, 4};

  const auto removed = wh::core::erase(values, 2);
  REQUIRE(removed == 2U);
  REQUIRE(values.to_std_vector() == std::vector<int>{1, 3, 4});

  const auto removed_if = wh::core::erase_if(
      values, [](const int value) { return (value % 2) == 1; });
  REQUIRE(removed_if == 2U);
  REQUIRE(values.to_std_vector() == std::vector<int>{4});

  wh::core::small_vector<int, 8U> ranged{1, 2, 3, 4, 5};
  const auto iterator = ranged.erase(ranged.begin() + 1, ranged.begin() + 4);
  REQUIRE(iterator == ranged.begin() + 1);
  REQUIRE(*iterator == 5);
  REQUIRE(ranged.to_std_vector() == std::vector<int>{1, 5});
}

TEST_CASE("small_vector custom allocator and propagation",
          "[core][small_vector][branch]") {
  using vector_t = wh::core::small_vector<int, 2U, tracking_allocator<int>>;

  vector_t base{tracking_allocator<int>{7}};
  REQUIRE(base.push_back(1).has_value());
  REQUIRE(base.push_back(2).has_value());

  vector_t target{tracking_allocator<int>{11}};
  REQUIRE(target.push_back(9).has_value());
  target = base;

  REQUIRE(target.get_allocator().id == 7);
  REQUIRE(target.to_std_vector() == std::vector<int>{1, 2});

  auto moved = std::move(target);
  REQUIRE(moved.get_allocator().id == 7);
  REQUIRE(moved.to_std_vector() == std::vector<int>{1, 2});
}

TEST_CASE("small_vector constructor family contracts",
          "[core][small_vector][condition][boundary]") {
  wh::core::small_vector<int, 4U> sized_default(3U);
  REQUIRE(sized_default.to_std_vector() == std::vector<int>{0, 0, 0});
  REQUIRE(sized_default.internal_capacity() == 4U);
  REQUIRE(sized_default.is_small());
  REQUIRE(
      sized_default.storage_is_unpropagable(sized_default.internal_storage()));

  wh::core::small_vector<int, 4U> sized_default_init(3U,
                                                     wh::core::default_init);
  wh::core::small_vector_base<int> &sized_default_view = sized_default_init;
  REQUIRE(sized_default_view.size() == 3U);

  wh::core::small_vector<int, 4U, tracking_allocator<int>> sized_default_init_a(
      3U, wh::core::default_init, tracking_allocator<int>{99});
  REQUIRE(sized_default_init_a.size() == 3U);
  REQUIRE(sized_default_init_a.get_allocator().id == 99);

  wh::core::small_vector<int, 4U> sized_value(3U, 5);
  REQUIRE(sized_value.to_std_vector() == std::vector<int>{5, 5, 5});

  const std::array<int, 3U> source{9, 8, 7};
  wh::core::small_vector<int, 4U> ranged(source.begin(), source.end());
  REQUIRE(ranged.to_std_vector() == std::vector<int>{9, 8, 7});

  wh::core::small_vector<int, 4U, tracking_allocator<int>> with_allocator(
      2U, 6, tracking_allocator<int>{44});
  REQUIRE(with_allocator.get_allocator().id == 44);
  REQUIRE(with_allocator.to_std_vector() == std::vector<int>{6, 6});

  using alloc_vector_t =
      wh::core::small_vector<int, 4U, tracking_allocator<int>>;

  alloc_vector_t il_with_allocator({1, 2, 3}, tracking_allocator<int>{55});
  REQUIRE(il_with_allocator.get_allocator().id == 55);
  REQUIRE(il_with_allocator.to_std_vector() == std::vector<int>{1, 2, 3});

  alloc_vector_t copied_with_allocator(il_with_allocator,
                                       tracking_allocator<int>{66});
  REQUIRE(copied_with_allocator.get_allocator().id == 66);
  REQUIRE(copied_with_allocator.to_std_vector() == std::vector<int>{1, 2, 3});

  alloc_vector_t moved_with_same_allocator(std::move(il_with_allocator),
                                           tracking_allocator<int>{55});
  REQUIRE(moved_with_same_allocator.get_allocator().id == 55);
  REQUIRE(moved_with_same_allocator.to_std_vector() ==
          std::vector<int>{1, 2, 3});

  using strict_vector_t =
      wh::core::small_vector<int, 4U, non_propagating_allocator<int>>;
  strict_vector_t strict_source{non_propagating_allocator<int>{7}};
  REQUIRE(strict_source.assign({4, 5, 6}).has_value());

  strict_vector_t moved_with_different_allocator(
      std::move(strict_source), non_propagating_allocator<int>{8});
  REQUIRE(moved_with_different_allocator.get_allocator().id == 8);
  REQUIRE(moved_with_different_allocator.to_std_vector() ==
          std::vector<int>{4, 5, 6});
}

TEST_CASE("small_vector assign resize and swap contracts",
          "[core][small_vector][branch][boundary]") {
  {
    wh::core::small_vector<int, 4U> values{1, 2};

    REQUIRE(values.assign(3U, 7).has_value());
    REQUIRE(values.to_std_vector() == std::vector<int>{7, 7, 7});

    const std::array<int, 4U> input{1, 3, 5, 7};
    REQUIRE(values.assign(input.begin(), input.end()).has_value());
    REQUIRE(values.to_std_vector() == std::vector<int>{1, 3, 5, 7});

    REQUIRE(values.assign({8, 6}).has_value());
    REQUIRE(values.to_std_vector() == std::vector<int>{8, 6});

    REQUIRE(values.assign(values.begin(), values.end()).has_value());
    REQUIRE(values.to_std_vector() == std::vector<int>{8, 6});

    REQUIRE(values.resize(5U, 9).has_value());
    REQUIRE(values.to_std_vector() == std::vector<int>{8, 6, 9, 9, 9});

    REQUIRE(values.resize(7U).has_value());
    REQUIRE(values.to_std_vector() == std::vector<int>{8, 6, 9, 9, 9, 0, 0});

    REQUIRE(values.resize(8U, wh::core::default_init).has_value());
    REQUIRE(values.size() == 8U);

    REQUIRE(values.resize(2U).has_value());
    REQUIRE(values.to_std_vector() == std::vector<int>{8, 6});
  }

  {
    using vector_t = wh::core::small_vector<int, 2U, tracking_allocator<int>>;

    vector_t left{tracking_allocator<int>{10}};
    vector_t right{tracking_allocator<int>{20}};
    REQUIRE(left.assign({1, 2, 3}).has_value());
    REQUIRE(right.assign({7, 8}).has_value());

    REQUIRE(left.swap(right).has_value());
    REQUIRE(left.get_allocator().id == 20);
    REQUIRE(right.get_allocator().id == 10);
    REQUIRE(left.to_std_vector() == std::vector<int>{7, 8});
    REQUIRE(right.to_std_vector() == std::vector<int>{1, 2, 3});
  }

  {
    using strict_vector_t =
        wh::core::small_vector<int, 4U, non_propagating_allocator<int>>;

    strict_vector_t left{non_propagating_allocator<int>{1}};
    strict_vector_t right{non_propagating_allocator<int>{2}};
    REQUIRE(left.assign({1, 2}).has_value());
    REQUIRE(right.assign({3, 4}).has_value());

    const auto denied = left.swap(right);
    REQUIRE(denied.has_error());
    REQUIRE(denied.error() == wh::core::errc::contract_violation);
    REQUIRE(left.to_std_vector() == std::vector<int>{1, 2});
    REQUIRE(right.to_std_vector() == std::vector<int>{3, 4});

    strict_vector_t same_left{non_propagating_allocator<int>{3}};
    strict_vector_t same_right{non_propagating_allocator<int>{3}};
    REQUIRE(same_left.assign({9}).has_value());
    REQUIRE(same_right.assign({11, 12}).has_value());

    REQUIRE(same_left.swap(same_right).has_value());
    REQUIRE(same_left.to_std_vector() == std::vector<int>{11, 12});
    REQUIRE(same_right.to_std_vector() == std::vector<int>{9});
  }
}

TEST_CASE("small_vector insert handles aliased source value",
          "[core][small_vector][branch]") {
  wh::core::small_vector<int, 2U> values{1, 2, 3};
  const auto &aliased = values[1];

  const auto inserted = values.insert(values.begin(), aliased);
  REQUIRE(inserted.has_value());
  REQUIRE(*inserted.value() == 2);
  REQUIRE(values.to_std_vector() == std::vector<int>{2, 1, 2, 3});
}

TEST_CASE("small_vector insert range covers boost-style branches",
          "[core][small_vector][branch][boundary]") {
  {
    wh::core::small_vector<int, 12U> values{1, 2, 3, 4, 5, 6};
    const std::array<int, 2U> inserted{70, 80};

    const auto iterator =
        values.insert(values.begin() + 2, inserted.begin(), inserted.end());
    REQUIRE(iterator.has_value());
    REQUIRE(iterator.value() == values.begin() + 2);
    REQUIRE(values.to_std_vector() ==
            std::vector<int>{1, 2, 70, 80, 3, 4, 5, 6});
  }

  {
    wh::core::small_vector<int, 12U> values{1, 2, 3, 4, 5};
    const std::array<int, 4U> inserted{90, 91, 92, 93};

    const auto iterator =
        values.insert(values.begin() + 4, inserted.begin(), inserted.end());
    REQUIRE(iterator.has_value());
    REQUIRE(iterator.value() == values.begin() + 4);
    REQUIRE(values.to_std_vector() ==
            std::vector<int>{1, 2, 3, 4, 90, 91, 92, 93, 5});
  }

  {
    wh::core::small_vector<int, 2U> values{1, 2};
    const std::array<int, 3U> inserted{10, 11, 12};

    const auto iterator =
        values.insert(values.begin() + 1, inserted.begin(), inserted.end());
    REQUIRE(iterator.has_value());
    REQUIRE(iterator.value() == values.begin() + 1);
    REQUIRE(values.to_std_vector() == std::vector<int>{1, 10, 11, 12, 2});
    REQUIRE(values.capacity() >= values.size());
  }

  {
    wh::core::small_vector<int, 2U> values{3, 4};
    const auto &aliased = values.front();

    const auto iterator = values.insert(values.begin() + 1, 3U, aliased);
    REQUIRE(iterator.has_value());
    REQUIRE(iterator.value() == values.begin() + 1);
    REQUIRE(values.to_std_vector() == std::vector<int>{3, 3, 3, 3, 4});
  }

  {
    wh::core::small_vector<int, 4U> values{1, 4};
    const auto iterator = values.insert(values.begin() + 1, {2, 3});
    REQUIRE(iterator.has_value());
    REQUIRE(iterator.value() == values.begin() + 1);
    REQUIRE(values.to_std_vector() == std::vector<int>{1, 2, 3, 4});
  }
}

TEST_CASE("small_vector shrink_to_fit returns to inline storage",
          "[core][small_vector][boundary]") {
  wh::core::small_vector<int, 2U> values;
  REQUIRE(values.push_back(10).has_value());
  REQUIRE(values.push_back(20).has_value());
  REQUIRE(values.push_back(30).has_value());

  REQUIRE_FALSE(values.using_inline_storage());
  values.pop_back();
  REQUIRE(values.shrink_to_fit().has_value());

  REQUIRE(values.using_inline_storage());
  REQUIRE(values.capacity() == 2U);
  REQUIRE(values.to_std_vector() == std::vector<int>{10, 20});
}

TEST_CASE("small_vector custom options and no-heap boundary",
          "[core][small_vector][extreme]") {
  using no_heap_options =
      wh::core::small_vector_options<3U, 2U, 0U, false, true>;
  using no_heap_vector =
      wh::core::small_vector<int, 2U, std::allocator<int>, no_heap_options>;

  no_heap_vector values;
  REQUIRE(values.push_back(10).has_value());
  REQUIRE(values.push_back(20).has_value());
  REQUIRE(values.capacity() == 2U);

  const auto overflow = values.push_back(30);
  REQUIRE(overflow.has_error());
  REQUIRE(overflow.error() == wh::core::errc::resource_exhausted);

  const auto assign_overflow = values.assign(3U, 99);
  REQUIRE(assign_overflow.has_error());
  REQUIRE(assign_overflow.error() == wh::core::errc::resource_exhausted);

  const auto resize_overflow = values.resize(3U, 99);
  REQUIRE(resize_overflow.has_error());
  REQUIRE(resize_overflow.error() == wh::core::errc::resource_exhausted);

  const auto policy = wh::core::describe_growth_policy(values);
  REQUIRE_FALSE(policy.heap_enabled);
  REQUIRE(policy.growth_multiplier_num == 3U);
  REQUIRE(policy.growth_multiplier_den == 2U);

  const auto contract = wh::core::describe_contract(values);
  REQUIRE(contract.supports_custom_options);
  REQUIRE(contract.supports_custom_allocator);
}

TEST_CASE("small_vector default-count constructor handles move-only default "
          "types",
          "[core][small_vector][boundary]") {
  wh::core::small_vector<default_only_value, 4U> values(3U);
  REQUIRE(values.size() == 3U);
  REQUIRE(values.capacity() >= 3U);

  REQUIRE(values.resize(5U).has_value());
  REQUIRE(values.size() == 5U);
}

TEST_CASE("small_vector growth saturates cleanly at size_type limit",
          "[core][small_vector][extreme][boundary]") {
  using tiny_options =
      wh::core::small_vector_options<3U, 2U, 0U, true, true, std::uint8_t>;
  using tiny_vector_t =
      wh::core::small_vector<int, 1U, std::allocator<int>, tiny_options>;

  tiny_vector_t values;
  bool hit_limit = false;
  for (std::size_t index = 0U; index < 1024U; ++index) {
    const auto pushed = values.push_back(static_cast<int>(index));
    if (pushed.has_error()) {
      REQUIRE(pushed.error() == wh::core::errc::resource_exhausted);
      hit_limit = true;
      break;
    }
  }

  REQUIRE(hit_limit);
  REQUIRE(values.size() <= values.max_size());
  REQUIRE(values.capacity() <= values.max_size());
  REQUIRE(values.max_size() <= static_cast<tiny_vector_t::size_type>(
                                   (std::numeric_limits<std::uint8_t>::max)()));
}

TEST_CASE("mpmc_queue bounded semantics and metrics",
          "[core][mpmc_queue][condition]") {
  wh::core::mpmc_queue<int> queue(4U);

  REQUIRE(queue.try_push(1).has_value());
  REQUIRE(queue.try_push(2).has_value());
  REQUIRE(queue.try_push(3).has_value());
  REQUIRE(queue.try_push(4).has_value());
  REQUIRE(queue.is_full());
  REQUIRE_FALSE(queue.is_empty());

  auto full = queue.try_push(5);
  REQUIRE(full.has_error());
  REQUIRE(full.error() == wh::core::errc::queue_full);

  auto first = queue.try_pop();
  REQUIRE(first.has_value());
  REQUIRE(first.value() == 1);

  auto second = queue.try_pop();
  REQUIRE(second.has_value());
  REQUIRE(second.value() == 2);

  REQUIRE(queue.try_pop().value() == 3);
  REQUIRE(queue.try_pop().value() == 4);
  REQUIRE(queue.try_pop().error() == wh::core::errc::queue_empty);
  REQUIRE(queue.is_empty());
  REQUIRE_FALSE(queue.is_full());
  REQUIRE(queue.size_guess() == 0);

  const auto contract = wh::core::describe_contract(queue);
  REQUIRE(contract.multi_producer);
  REQUIRE(contract.multi_consumer);
  REQUIRE_FALSE(contract.boost_dummy_node_pattern);
  REQUIRE(contract.folly_ring_sequence_aba_guard);
  REQUIRE(contract.bounded_ring_avoids_reclamation_aba);

  const auto metrics = wh::core::describe_metrics(queue);
  REQUIRE(metrics.push_count == 4U);
  REQUIRE(metrics.pop_count == 4U);
  REQUIRE(metrics.approximate_depth == 0U);
}

TEST_CASE("mpmc_queue bounded custom allocator parity with folly",
          "[core][mpmc_queue][allocator]") {
  using allocator_t = queue_tracking_allocator<int>;
  using queue_t = wh::core::mpmc_queue<int, false, allocator_t>;

  queue_allocator_counters::reset();
  {
    queue_t queue(4U, allocator_t{77});
    REQUIRE(queue.get_allocator().id == 77);
    REQUIRE(queue.allocated_capacity() == 4U);

    REQUIRE(queue.try_push(10).has_value());
    REQUIRE(queue.try_push(11).has_value());
    REQUIRE(queue.write_count() == 2U);
    REQUIRE(queue.read_count() == 0U);

    auto first = queue.try_pop();
    REQUIRE(first.has_value());
    REQUIRE(first.value() == 10);
    REQUIRE(queue.read_count() == 1U);
  }

  REQUIRE(queue_allocator_counters::allocations.load(std::memory_order_relaxed) >
          0U);
  REQUIRE(queue_allocator_counters::allocations.load(std::memory_order_relaxed) ==
          queue_allocator_counters::deallocations.load(
              std::memory_order_relaxed));
}

TEST_CASE("mpmc_queue multi-producer multi-consumer uniqueness",
          "[core][mpmc_queue][concurrency]") {
  constexpr std::size_t producer_count = 4U;
  constexpr std::size_t consumer_count = 3U;
  constexpr std::size_t per_producer = 128U;
  constexpr std::size_t expected_total = producer_count * per_producer;

  wh::core::mpmc_queue<queue_item> queue(256U);

  std::array<std::thread, producer_count> producers{};
  for (std::size_t producer = 0U; producer < producer_count; ++producer) {
    producers[producer] = std::thread([producer, &queue] {
      for (std::size_t index = 0U; index < per_producer; ++index) {
        auto pushed = queue.try_push(queue_item{producer, index});
        while (!pushed.has_value()) {
          pushed = queue.try_push(queue_item{producer, index});
        }
      }
    });
  }

  std::array<std::thread, consumer_count> consumers{};
  std::vector<bool> seen(expected_total, false);
  std::mutex seen_mutex;
  std::atomic<std::size_t> consumed{0U};

  for (std::size_t index = 0U; index < consumer_count; ++index) {
    consumers[index] = std::thread([&] {
      while (consumed.load(std::memory_order_relaxed) < expected_total) {
        auto popped = queue.try_pop();
        if (!popped.has_value()) {
          continue;
        }

        const auto value = popped.value();
        const auto encoded = (value.producer * per_producer) + value.sequence;

        {
          std::lock_guard<std::mutex> guard(seen_mutex);
          REQUIRE(encoded < expected_total);
          REQUIRE_FALSE(seen[encoded]);
          seen[encoded] = true;
        }

        consumed.fetch_add(1U, std::memory_order_relaxed);
      }
    });
  }

  for (auto &producer : producers) {
    producer.join();
  }

  while (consumed.load(std::memory_order_relaxed) < expected_total) {
    std::this_thread::yield();
  }

  for (auto &consumer : consumers) {
    consumer.join();
  }

  REQUIRE(queue.empty());

  const auto metrics = wh::core::describe_metrics(queue);
  REQUIRE(metrics.push_count == expected_total);
  REQUIRE(metrics.pop_count == expected_total);
  REQUIRE(metrics.approximate_depth == 0U);
}

TEST_CASE("mpmc_queue wraparound and contract", "[core][mpmc_queue][branch]") {
  wh::core::mpmc_queue<std::size_t> queue(2U);

  constexpr std::size_t rounds = 512U;
  std::vector<std::size_t> popped;
  popped.reserve(rounds);

  for (std::size_t value = 0U; value < rounds; ++value) {
    auto pushed = queue.try_push(value);
    while (!pushed.has_value()) {
      auto drained = queue.try_pop();
      REQUIRE(drained.has_value());
      popped.push_back(drained.value());
      pushed = queue.try_push(value);
    }
  }

  while (popped.size() < rounds) {
    auto drained = queue.try_pop();
    if (!drained.has_value()) {
      continue;
    }
    popped.push_back(drained.value());
  }

  REQUIRE(popped.size() == rounds);
  REQUIRE(std::is_sorted(popped.begin(), popped.end()));
  REQUIRE(popped.front() == 0U);
  REQUIRE(popped.back() == rounds - 1U);

  const auto contract = wh::core::describe_contract(queue);
  REQUIRE(contract.folly_ring_sequence_aba_guard);
  REQUIRE(contract.bounded_ring_avoids_reclamation_aba);
}

TEST_CASE("mpmc_queue supports bounded and dynamic growth modes",
          "[core][mpmc_queue][condition][boundary]") {
  {
    wh::core::mpmc_queue<int> bounded(2U);
    REQUIRE(bounded.try_push(1).has_value());
    REQUIRE(bounded.try_push(2).has_value());
    auto overflow = bounded.try_push(3);
    REQUIRE(overflow.has_error());
    REQUIRE(overflow.error() == wh::core::errc::queue_full);
    REQUIRE(bounded.capacity() == 2U);
    REQUIRE(bounded.max_capacity() == 2U);
    REQUIRE_FALSE(bounded.dynamic_growth_enabled());

    const auto contract = wh::core::describe_contract(bounded);
    REQUIRE(contract.bounded_capacity);
  }

  {
    wh::core::mpmc_queue<int, true> dynamic_default(64U);
    REQUIRE(dynamic_default.max_capacity() == 64U);
    REQUIRE(dynamic_default.capacity() == 10U);

    wh::core::mpmc_queue<int, true> dynamic_ctor(64U, 16U, 4U);
    REQUIRE(dynamic_ctor.max_capacity() == 64U);
    REQUIRE(dynamic_ctor.capacity() == 16U);
  }

  {
    wh::core::mpmc_dynamic_options options{};
    options.max_capacity = 8U;
    options.growth_factor = 2U;

    wh::core::mpmc_queue<int, true> dynamic(2U, options);
    std::size_t forced_progress_pops = 0U;
    std::vector<bool> seen(8U, false);
    for (int value = 0; value < 8; ++value) {
      auto pushed = dynamic.try_push(value);
      while (!pushed.has_value()) {
        REQUIRE(pushed.error() == wh::core::errc::queue_full);
        auto progressed = dynamic.try_pop();
        REQUIRE(progressed.has_value());
        REQUIRE(progressed.value() >= 0);
        REQUIRE(progressed.value() < 8);
        REQUIRE_FALSE(seen[static_cast<std::size_t>(progressed.value())]);
        seen[static_cast<std::size_t>(progressed.value())] = true;
        ++forced_progress_pops;
        pushed = dynamic.try_push(value);
      }
      REQUIRE(pushed.has_value());
    }

    REQUIRE(forced_progress_pops > 0U);
    REQUIRE(dynamic.capacity() == 8U);
    REQUIRE(dynamic.allocated_capacity() == 8U);
    REQUIRE(dynamic.max_capacity() == 8U);
    REQUIRE(dynamic.dynamic_growth_enabled());
    REQUIRE_FALSE(dynamic.is_empty());
    REQUIRE(dynamic.size_guess() >= 0);

    const auto contract = wh::core::describe_contract(dynamic);
    REQUIRE_FALSE(contract.bounded_capacity);

    while (true) {
      auto popped = dynamic.try_pop();
      if (!popped.has_value()) {
        REQUIRE(popped.error() == wh::core::errc::queue_empty);
        break;
      }
      REQUIRE(popped.value() >= 0);
      REQUIRE(popped.value() < 8);
      REQUIRE_FALSE(seen[static_cast<std::size_t>(popped.value())]);
      seen[static_cast<std::size_t>(popped.value())] = true;
    }

    for (bool value_seen : seen) {
      REQUIRE(value_seen);
    }
    REQUIRE(dynamic.write_count() == 8U);
    REQUIRE(dynamic.read_count() == 8U);
  }
}

TEST_CASE("mpmc queue boundary result code strings",
          "[core][mpmc_queue][boundary]") {
  REQUIRE(wh::core::to_string(wh::core::errc::queue_empty) == "queue_empty");
  REQUIRE(wh::core::to_string(wh::core::errc::queue_full) == "queue_full");
}
