#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "wh/core/mpmc_queue.hpp"
#include "wh/core/types/small_vector_types.hpp"

namespace wh::core {

enum class mpmc_backpressure_policy {
  drop,
  fail,
  defer,
};

struct mpmc_memory_order_contract {
  std::string_view producer_ticket_claim{"acq_rel"};
  std::string_view producer_publish{"release"};
  std::string_view consumer_ticket_claim{"acq_rel"};
  std::string_view consumer_observe{"acquire"};
};

struct mpmc_queue_contract {
  bool multi_producer{true};
  bool multi_consumer{true};
  bool bounded_capacity{true};
  bool producer_path_lock_free{true};
  bool consumer_path_lock_free{true};
  bool boost_dummy_node_pattern{true};
  bool bounded_ring_avoids_reclamation_aba{false};
  bool folly_ring_sequence_aba_guard{false};
  mpmc_backpressure_policy backpressure_when_full{
      mpmc_backpressure_policy::fail};
  complexity_class push_complexity{complexity_class::constant};
  complexity_class pop_complexity{complexity_class::constant};
};

struct mpmc_queue_metrics_snapshot {
  std::uint64_t push_count{0U};
  std::uint64_t pop_count{0U};
  std::size_t approximate_depth{0U};
  std::size_t capacity{0U};
  bool lock_free{false};
};

template <typename value_t> using default_mpmc_queue = mpmc_queue<value_t>;

template <typename value_t, bool dynamic_growth, typename allocator_t>
[[nodiscard]] auto
describe_contract(const mpmc_queue<value_t, dynamic_growth, allocator_t> &queue)
    -> mpmc_queue_contract {
  mpmc_queue_contract contract{};
  contract.bounded_capacity = !dynamic_growth;
  contract.producer_path_lock_free = queue.lock_free();
  contract.consumer_path_lock_free = queue.lock_free();
  contract.boost_dummy_node_pattern = false;
  contract.bounded_ring_avoids_reclamation_aba = true;
  contract.folly_ring_sequence_aba_guard = true;
  return contract;
}

template <typename value_t, bool dynamic_growth, typename allocator_t>
[[nodiscard]] constexpr auto memory_order_contract(
    const mpmc_queue<value_t, dynamic_growth, allocator_t> &) noexcept
    -> mpmc_memory_order_contract {
  return mpmc_memory_order_contract{};
}

template <typename queue_t>
[[nodiscard]] auto describe_metrics(const queue_t &queue)
    -> mpmc_queue_metrics_snapshot {
  return mpmc_queue_metrics_snapshot{queue.push_count(), queue.pop_count(),
                                     queue.approximate_depth(),
                                     queue.capacity(), queue.lock_free()};
}

} // namespace wh::core
