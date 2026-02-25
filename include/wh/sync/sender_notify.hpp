#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "wh/core/compiler.hpp"

namespace wh::core {

class sender_notify {
public:
  static constexpr std::uint16_t invalid_channel_index =
      std::numeric_limits<std::uint16_t>::max();

  struct wait_registration {
    std::atomic<std::uint64_t> *turn_ptr{nullptr};
    std::uint64_t expected_turn{0U};
    std::uint16_t channel_hint{invalid_channel_index};
  };

  struct waiter {
    std::atomic<std::uint64_t> *turn_ptr{nullptr};
    std::uint64_t expected_turn{0U};
    waiter *next{nullptr};
    waiter *prev{nullptr};
    void *owner{nullptr};
    void (*notify)(void *, waiter *) noexcept {nullptr};
    std::atomic<bool> armed{false};
    std::atomic<bool> linked{false};
    std::atomic<bool> notifying{false};
    std::uint16_t channel_hint{invalid_channel_index};
    std::atomic<std::uint16_t> channel_index{invalid_channel_index};
  };

  [[nodiscard]] auto has_waiters() const noexcept -> bool {
    return occupied_channel_count_.load(std::memory_order_relaxed) != 0U;
  }

  [[nodiscard]] static auto
  suggest_channel_index(std::atomic<std::uint64_t> *turn_ptr,
                        const std::uint64_t expected_turn) noexcept
      -> std::uint16_t {
    return static_cast<std::uint16_t>(hash_key(turn_ptr, expected_turn));
  }

  [[nodiscard]] auto arm(waiter &waiter_state) noexcept -> bool {
    auto *turn_ptr = waiter_state.turn_ptr;
    wh_precondition(turn_ptr != nullptr);

    if (turn_reached(turn_ptr->load(std::memory_order_acquire),
                     waiter_state.expected_turn)) {
      return false;
    }

    const auto key_tag = mix_key(turn_ptr, waiter_state.expected_turn);
    std::size_t channel_index = 0U;
    auto *channel = find_or_reserve_channel(waiter_state, turn_ptr,
                                            waiter_state.expected_turn, key_tag,
                                            channel_index);
    if (channel == nullptr) {
      return false;
    }

    if (turn_reached(turn_ptr->load(std::memory_order_acquire),
                     waiter_state.expected_turn)) {
      clear_channel_if_empty(*channel);
      unlock_channel(*channel);
      return false;
    }

    waiter_state.notifying.store(false, std::memory_order_release);
    waiter_state.armed.store(true, std::memory_order_release);
    waiter_state.linked.store(true, std::memory_order_relaxed);
    waiter_state.channel_index.store(static_cast<std::uint16_t>(channel_index),
                                     std::memory_order_relaxed);
    waiter_state.prev = nullptr;
    waiter_state.next = channel->head;
    if (channel->head != nullptr) {
      channel->head->prev = &waiter_state;
    }
    const bool was_empty = (channel->size == 0U);
    channel->head = &waiter_state;
    ++channel->size;
    if (was_empty) {
      mark_channel_occupied(channel_index);
    }

    if (turn_reached(turn_ptr->load(std::memory_order_acquire),
                     waiter_state.expected_turn)) {
      waiter_state.armed.store(false, std::memory_order_release);
      remove_waiter_from_channel(*channel, channel_index, waiter_state);
      unlock_channel(*channel);
      return false;
    }

    unlock_channel(*channel);
    return true;
  }

  void disarm(waiter &waiter_state) noexcept {
    waiter_state.armed.store(false, std::memory_order_release);

    const auto channel_index =
        waiter_state.channel_index.load(std::memory_order_relaxed);
    if (channel_index != invalid_channel_index) {
      auto &channel = channels_[channel_index];
      lock_channel(channel);
      if (waiter_state.linked.load(std::memory_order_relaxed)) {
        remove_waiter_from_channel(channel, channel_index, waiter_state);
      }
      unlock_channel(channel);
    }

    while (waiter_state.notifying.load(std::memory_order_acquire)) {
      spin_pause();
    }
  }

  wh_noinline void notify(std::atomic<std::uint64_t> *turn_ptr,
                          const std::uint64_t turn_value) noexcept {
    wh_precondition(turn_ptr != nullptr);

    std::size_t channel_index = 0U;
    auto *channel = lock_existing_channel(turn_ptr, turn_value, channel_index);
    if (channel == nullptr) {
      return;
    }

    const auto detached_count = channel->size;
    auto *list = channel->head;
    channel->head = nullptr;
    channel->size = 0U;
    channel->turn_ptr = nullptr;
    channel->expected_turn = 0U;
    channel->key_tag.store(0U, std::memory_order_relaxed);
    if (detached_count != 0U) {
      mark_channel_empty(channel_index);
    }

    waiter *ready = nullptr;
    while (list != nullptr) {
      auto *current = list;
      list = current->next;
      current->next = nullptr;
      current->prev = nullptr;
      current->linked.store(false, std::memory_order_relaxed);
      current->channel_index.store(invalid_channel_index,
                                   std::memory_order_relaxed);

      if (current->armed.exchange(false, std::memory_order_acq_rel)) {
        current->notifying.store(true, std::memory_order_release);
        current->next = ready;
        ready = current;
      }
    }

    unlock_channel(*channel);

    while (ready != nullptr) {
      auto *w = ready;
      ready = ready->next;
      w->next = nullptr;
      w->notify(w->owner, w);
      w->notifying.store(false, std::memory_order_release);
    }
  }

private:
  static constexpr std::size_t wait_channel_count_ = 1024U;
  static constexpr std::size_t min_probe_window_ = 8U;
  static constexpr std::size_t max_probe_window_ = 256U;
  static_assert((wait_channel_count_ & (wait_channel_count_ - 1U)) == 0U);
  static_assert(wait_channel_count_ <=
                std::numeric_limits<std::uint16_t>::max());

  struct alignas(default_cacheline_size) wait_channel {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> key_tag{0U};
    std::atomic<std::uint64_t> *turn_ptr{nullptr};
    std::uint64_t expected_turn{0U};
    waiter *head{nullptr};
    std::size_t size{0U};
  };

  [[nodiscard]] static auto
  turn_reached(const std::uint64_t current_turn,
               const std::uint64_t expected_turn) noexcept -> bool {
    return static_cast<std::int64_t>(current_turn - expected_turn) >= 0;
  }

  [[nodiscard]] static auto mix_key(std::atomic<std::uint64_t> *turn_ptr,
                                    const std::uint64_t expected_turn) noexcept
      -> std::uint64_t {
    auto mixed = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(turn_ptr) >> 6U);
    mixed ^=
        expected_turn + 0x9e3779b97f4a7c15ULL + (mixed << 6U) + (mixed >> 2U);
    mixed ^= mixed >> 30U;
    mixed *= 0xbf58476d1ce4e5b9ULL;
    mixed ^= mixed >> 27U;
    mixed *= 0x94d049bb133111ebULL;
    mixed ^= mixed >> 31U;
    return mixed | 1ULL;
  }

  [[nodiscard]] static auto hash_key(std::atomic<std::uint64_t> *turn_ptr,
                                     const std::uint64_t expected_turn) noexcept
      -> std::size_t {
    return static_cast<std::size_t>(mix_key(turn_ptr, expected_turn)) &
           (wait_channel_count_ - 1U);
  }

  [[nodiscard]] auto probe_window() const noexcept -> std::size_t {
    const auto value = probe_window_.load(std::memory_order_relaxed);
    return std::clamp<std::size_t>(value, min_probe_window_, max_probe_window_);
  }

  void maybe_grow_probe_window(const std::size_t current) noexcept {
    if (current >= max_probe_window_) {
      return;
    }
    const auto target = std::min(max_probe_window_, current * 2U);
    auto expected = current;
    (void)probe_window_.compare_exchange_weak(
        expected, target, std::memory_order_relaxed, std::memory_order_relaxed);
  }

  [[nodiscard]] auto lock_matching_channel(std::atomic<std::uint64_t> *turn_ptr,
                                           const std::uint64_t expected_turn,
                                           const std::uint64_t key_tag,
                                           const std::size_t start,
                                           const std::size_t span,
                                           std::size_t &channel_index) noexcept
      -> wait_channel * {
    for (std::size_t offset = 0U; offset < span; ++offset) {
      const auto index = (start + offset) & (wait_channel_count_ - 1U);
      auto &channel = channels_[index];
      if (channel.key_tag.load(std::memory_order_relaxed) != key_tag) {
        continue;
      }
      lock_channel(channel);
      if (channel.turn_ptr == turn_ptr &&
          channel.expected_turn == expected_turn) {
        channel_index = index;
        return &channel;
      }
      unlock_channel(channel);
    }
    return nullptr;
  }

  [[nodiscard]] auto lock_empty_channel(const std::size_t start,
                                        const std::size_t span,
                                        std::size_t &channel_index) noexcept
      -> wait_channel * {
    for (std::size_t offset = 0U; offset < span; ++offset) {
      const auto index = (start + offset) & (wait_channel_count_ - 1U);
      auto &channel = channels_[index];
      if (channel.key_tag.load(std::memory_order_relaxed) != 0U) {
        continue;
      }
      lock_channel(channel);
      if (channel.size == 0U &&
          channel.key_tag.load(std::memory_order_relaxed) == 0U) {
        channel_index = index;
        return &channel;
      }
      unlock_channel(channel);
    }
    return nullptr;
  }

  [[nodiscard]] auto lock_channel_by_hint(const std::uint16_t hint,
                                          const std::uint64_t key_tag,
                                          std::size_t &channel_index) noexcept
      -> wait_channel * {
    if (hint == invalid_channel_index) {
      return nullptr;
    }
    channel_index = hint & (wait_channel_count_ - 1U);
    auto &channel = channels_[channel_index];
    const auto observed_tag = channel.key_tag.load(std::memory_order_relaxed);
    if (observed_tag != 0U && observed_tag != key_tag) {
      return nullptr;
    }
    lock_channel(channel);
    return &channel;
  }

  [[nodiscard]] auto lock_existing_channel(std::atomic<std::uint64_t> *turn_ptr,
                                           const std::uint64_t expected_turn,
                                           std::size_t &channel_index) noexcept
      -> wait_channel * {
    const auto key_tag = mix_key(turn_ptr, expected_turn);
    const auto start = hash_key(turn_ptr, expected_turn);
    const auto span = probe_window();

    if (auto *channel = lock_matching_channel(turn_ptr, expected_turn, key_tag,
                                              start, span, channel_index);
        channel != nullptr) {
      return channel;
    }
    return lock_matching_channel(turn_ptr, expected_turn, key_tag, start,
                                 wait_channel_count_, channel_index);
  }

  [[nodiscard]] auto find_or_reserve_channel(
      waiter &waiter_state, std::atomic<std::uint64_t> *turn_ptr,
      const std::uint64_t expected_turn, const std::uint64_t key_tag,
      std::size_t &channel_index) noexcept -> wait_channel * {
    if (auto *hinted = lock_channel_by_hint(waiter_state.channel_hint, key_tag,
                                            channel_index);
        hinted != nullptr) {
      if (hinted->turn_ptr == turn_ptr &&
          hinted->expected_turn == expected_turn) {
        return hinted;
      }
      if (hinted->size == 0U) {
        hinted->turn_ptr = turn_ptr;
        hinted->expected_turn = expected_turn;
        hinted->key_tag.store(key_tag, std::memory_order_relaxed);
        return hinted;
      }
      unlock_channel(*hinted);
    }

    const auto start = hash_key(turn_ptr, expected_turn);
    for (std::size_t attempt = 0U; attempt < 3U; ++attempt) {
      const auto span = probe_window();

      if (auto *channel = lock_matching_channel(
              turn_ptr, expected_turn, key_tag, start, span, channel_index);
          channel != nullptr) {
        return channel;
      }
      if (auto *channel = lock_empty_channel(start, span, channel_index);
          channel != nullptr) {
        channel->turn_ptr = turn_ptr;
        channel->expected_turn = expected_turn;
        channel->key_tag.store(key_tag, std::memory_order_relaxed);
        return channel;
      }
      maybe_grow_probe_window(span);
    }

    if (auto *channel =
            lock_matching_channel(turn_ptr, expected_turn, key_tag, start,
                                  wait_channel_count_, channel_index);
        channel != nullptr) {
      return channel;
    }
    if (auto *channel =
            lock_empty_channel(start, wait_channel_count_, channel_index);
        channel != nullptr) {
      channel->turn_ptr = turn_ptr;
      channel->expected_turn = expected_turn;
      channel->key_tag.store(key_tag, std::memory_order_relaxed);
      return channel;
    }
    return nullptr;
  }

  static void lock_channel(wait_channel &channel) noexcept {
    while (channel.lock.test_and_set(std::memory_order_acquire)) {
      spin_pause();
    }
  }

  static void unlock_channel(wait_channel &channel) noexcept {
    channel.lock.clear(std::memory_order_release);
  }

  static void clear_channel_if_empty(wait_channel &channel) noexcept {
    if (channel.size == 0U) {
      channel.head = nullptr;
      channel.turn_ptr = nullptr;
      channel.expected_turn = 0U;
      channel.key_tag.store(0U, std::memory_order_relaxed);
    }
  }

  void remove_waiter_from_channel(wait_channel &channel,
                                  const std::size_t channel_index,
                                  waiter &waiter_state) noexcept {
    if (!waiter_state.linked.load(std::memory_order_relaxed)) {
      return;
    }

    auto *prev = waiter_state.prev;
    auto *next = waiter_state.next;

    if (prev != nullptr) {
      prev->next = next;
    } else {
      channel.head = next;
    }
    if (next != nullptr) {
      next->prev = prev;
    }

    waiter_state.next = nullptr;
    waiter_state.prev = nullptr;
    waiter_state.linked.store(false, std::memory_order_relaxed);
    waiter_state.channel_index.store(invalid_channel_index,
                                     std::memory_order_relaxed);
    if (channel.size > 0U) {
      --channel.size;
      if (channel.size == 0U) {
        mark_channel_empty(channel_index);
      }
    }
    clear_channel_if_empty(channel);
  }

  void mark_channel_occupied(const std::size_t channel_index) noexcept {
    (void)channel_index;
    occupied_channel_count_.fetch_add(1U, std::memory_order_relaxed);
  }

  void mark_channel_empty(const std::size_t channel_index) noexcept {
    (void)channel_index;
    occupied_channel_count_.fetch_sub(1U, std::memory_order_relaxed);
  }

  std::atomic<std::uint32_t> occupied_channel_count_{0U};
  std::atomic<std::size_t> probe_window_{16U};
  std::array<wait_channel, wait_channel_count_> channels_{};
};

} // namespace wh::core
