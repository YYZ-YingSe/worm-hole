#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

#include "wh/core/bounded_queue/bounded_queue.hpp"

namespace wh::core {

template <typename value_t, typename allocator_t = std::allocator<value_t>>
class bounded_queue_producer {
public:
  using queue_type = bounded_queue<value_t, allocator_t>;
  using value_type = typename queue_type::value_type;
  using allocator_type = typename queue_type::allocator_type;
  using status_type = bounded_queue_status;

  explicit bounded_queue_producer(queue_type &queue) noexcept
      : queue_(&queue) {}

  auto push(const value_type &value) -> bool
    requires std::copy_constructible<value_type>
  {
    return queue_->push(value);
  }

  auto push(value_type &&value) -> bool
    requires std::move_constructible<value_type>
  {
    return queue_->push(std::move(value));
  }

  template <typename... args_t>
    requires std::constructible_from<value_type, args_t &&...>
  auto emplace(args_t &&...args) -> bool {
    return queue_->emplace(std::forward<args_t>(args)...);
  }

  [[nodiscard]] auto try_push(const value_type &value) noexcept(
      noexcept(std::declval<queue_type &>().try_push(value))) -> status_type
    requires std::copy_constructible<value_type>
  {
    return queue_->try_push(value);
  }

  [[nodiscard]] auto try_push(value_type &&value) noexcept(noexcept(
      std::declval<queue_type &>().try_push(std::move(value)))) -> status_type
    requires std::move_constructible<value_type>
  {
    return queue_->try_push(std::move(value));
  }

  template <typename... args_t>
    requires std::constructible_from<value_type, args_t &&...>
  [[nodiscard]] auto try_emplace(args_t &&...args) noexcept(noexcept(
      std::declval<queue_type &>().try_emplace(std::forward<args_t>(args)...)))
      -> status_type {
    return queue_->try_emplace(std::forward<args_t>(args)...);
  }

  [[nodiscard]] auto async_push(const value_type &value) noexcept(
      noexcept(std::declval<queue_type &>().async_push(value))) ->
      typename queue_type::push_sender
    requires std::copy_constructible<value_type>
  {
    return queue_->async_push(value);
  }

  [[nodiscard]] auto async_push(value_type &&value) noexcept(
      noexcept(std::declval<queue_type &>().async_push(std::move(value)))) ->
      typename queue_type::push_sender
    requires std::move_constructible<value_type>
  {
    return queue_->async_push(std::move(value));
  }

  template <typename... args_t>
    requires std::constructible_from<value_type, args_t &&...> &&
             std::move_constructible<value_type>
  [[nodiscard]] auto async_emplace(args_t &&...args) noexcept(
      noexcept(std::declval<queue_type &>().async_emplace(
          std::forward<args_t>(args)...))) -> typename queue_type::push_sender {
    return queue_->async_emplace(std::forward<args_t>(args)...);
  }

  auto close() noexcept -> void { queue_->close(); }

  [[nodiscard]] auto is_closed() const noexcept -> bool {
    return queue_->is_closed();
  }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return queue_->capacity();
  }

  [[nodiscard]] auto size_hint() const noexcept -> std::size_t {
    return queue_->size_hint();
  }

  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
    return queue_->get_allocator();
  }

private:
  queue_type *queue_{nullptr};
};

template <typename value_t, typename allocator_t = std::allocator<value_t>>
class bounded_queue_consumer {
public:
  using queue_type = bounded_queue<value_t, allocator_t>;
  using value_type = typename queue_type::value_type;
  using allocator_type = typename queue_type::allocator_type;
  using try_pop_result = typename queue_type::try_pop_result;

  explicit bounded_queue_consumer(queue_type &queue) noexcept
      : queue_(&queue) {}

  [[nodiscard]] auto pop() -> std::optional<value_type> {
    return queue_->pop();
  }

  [[nodiscard]] auto
  try_pop() noexcept(noexcept(std::declval<queue_type &>().try_pop()))
      -> try_pop_result {
    return queue_->try_pop();
  }

  [[nodiscard]] auto async_pop() noexcept -> typename queue_type::pop_sender {
    return queue_->async_pop();
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool {
    return queue_->is_closed();
  }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return queue_->capacity();
  }

  [[nodiscard]] auto size_hint() const noexcept -> std::size_t {
    return queue_->size_hint();
  }

  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
    return queue_->get_allocator();
  }

private:
  queue_type *queue_{nullptr};
};

template <typename value_t, typename allocator_t>
[[nodiscard]] inline auto
make_producer(bounded_queue<value_t, allocator_t> &queue) noexcept
    -> bounded_queue_producer<value_t, allocator_t> {
  return bounded_queue_producer<value_t, allocator_t>{queue};
}

template <typename value_t, typename allocator_t>
[[nodiscard]] inline auto
make_consumer(bounded_queue<value_t, allocator_t> &queue) noexcept
    -> bounded_queue_consumer<value_t, allocator_t> {
  return bounded_queue_consumer<value_t, allocator_t>{queue};
}

template <typename value_t, typename allocator_t>
[[nodiscard]] inline auto
split_endpoints(bounded_queue<value_t, allocator_t> &queue) noexcept
    -> std::pair<bounded_queue_producer<value_t, allocator_t>,
                 bounded_queue_consumer<value_t, allocator_t>> {
  return {make_producer(queue), make_consumer(queue)};
}

} // namespace wh::core
