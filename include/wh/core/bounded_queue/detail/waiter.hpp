#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <utility>

namespace wh::core::detail {

struct stopped_tag {};

template <typename waiter_t> struct waiter_ops {
  void (*complete)(waiter_t *) noexcept {nullptr};
  bool (*try_complete)(waiter_t *) noexcept {nullptr};
};

template <typename value_t> struct push_waiter_base {
  using value_type = value_t;
  using ops_type = waiter_ops<push_waiter_base>;

  enum class state_type : std::uint8_t {
    empty = 0U,
    source_copy,
    source_move,
    success,
    closed,
    error,
    stopped,
  };

  push_waiter_base *next{nullptr};
  push_waiter_base *prev{nullptr};
  const ops_type *ops{nullptr};

  union payload_storage {
    const value_t *value_ptr;
    std::exception_ptr error;

    payload_storage() noexcept : value_ptr(nullptr) {}
    ~payload_storage() {}
  } payload{};

  state_type state{state_type::empty};

  push_waiter_base() = default;
  push_waiter_base(const push_waiter_base &) = delete;
  auto operator=(const push_waiter_base &) -> push_waiter_base & = delete;
  push_waiter_base(push_waiter_base &&) = delete;
  auto operator=(push_waiter_base &&) -> push_waiter_base & = delete;

  ~push_waiter_base() { destroy_active_payload(); }

  auto set_copy_source(const value_t &value) noexcept -> void {
    destroy_active_payload();
    payload.value_ptr = std::addressof(value);
    state = state_type::source_copy;
  }

  auto set_move_source(value_t &value) noexcept -> void {
    destroy_active_payload();
    payload.value_ptr = std::addressof(value);
    state = state_type::source_move;
  }

  auto store_success() noexcept -> void {
    destroy_active_payload();
    state = state_type::success;
  }

  auto store_closed() noexcept -> void {
    destroy_active_payload();
    state = state_type::closed;
  }

  auto store_stopped() noexcept -> void {
    destroy_active_payload();
    state = state_type::stopped;
  }

  auto store_error(std::exception_ptr error) noexcept -> void {
    if (state == state_type::error) {
      payload.error = std::move(error);
      return;
    }

    destroy_active_payload();
    std::construct_at(std::addressof(payload.error), std::move(error));
    state = state_type::error;
  }

  [[nodiscard]] auto has_copy_source() const noexcept -> bool {
    return state == state_type::source_copy;
  }

  [[nodiscard]] auto has_move_source() const noexcept -> bool {
    return state == state_type::source_move;
  }

  [[nodiscard]] auto source_value() const noexcept -> const value_t & { return *payload.value_ptr; }

  [[nodiscard]] auto movable_source_value() const noexcept -> value_t & {
    return *const_cast<value_t *>(payload.value_ptr);
  }

  [[nodiscard]] auto is_success() const noexcept -> bool { return state == state_type::success; }

  [[nodiscard]] auto is_closed() const noexcept -> bool { return state == state_type::closed; }

  [[nodiscard]] auto is_error() const noexcept -> bool { return state == state_type::error; }

  [[nodiscard]] auto is_stopped() const noexcept -> bool { return state == state_type::stopped; }

  [[nodiscard]] auto error() const noexcept -> const std::exception_ptr & { return payload.error; }

private:
  auto destroy_active_payload() noexcept -> void {
    if (state == state_type::error) {
      payload.error.~exception_ptr();
    }
    state = state_type::empty;
    payload.value_ptr = nullptr;
  }
};

template <typename value_t> struct pop_waiter_base {
  using value_type = value_t;
  using ops_type = waiter_ops<pop_waiter_base>;

  enum class state_type : std::uint8_t {
    empty = 0U,
    value,
    closed,
    error,
    stopped,
  };

  pop_waiter_base *next{nullptr};
  pop_waiter_base *prev{nullptr};
  const ops_type *ops{nullptr};

  union payload_storage {
    value_t value;
    std::exception_ptr error;

    payload_storage() {}
    ~payload_storage() {}
  } payload{};

  state_type state{state_type::empty};

  pop_waiter_base() = default;
  pop_waiter_base(const pop_waiter_base &) = delete;
  auto operator=(const pop_waiter_base &) -> pop_waiter_base & = delete;
  pop_waiter_base(pop_waiter_base &&) = delete;
  auto operator=(pop_waiter_base &&) -> pop_waiter_base & = delete;

  ~pop_waiter_base() { destroy_active_payload(); }

  template <typename... args_t>
    requires std::constructible_from<value_t, args_t &&...>
  auto emplace_value(args_t &&...args) -> void {
    destroy_active_payload();
    std::construct_at(std::addressof(payload.value), std::forward<args_t>(args)...);
    state = state_type::value;
  }

  auto store_closed() noexcept -> void {
    destroy_active_payload();
    state = state_type::closed;
  }

  auto store_stopped() noexcept -> void {
    destroy_active_payload();
    state = state_type::stopped;
  }

  auto store_error(std::exception_ptr error) noexcept -> void {
    if (state == state_type::error) {
      payload.error = std::move(error);
      return;
    }

    destroy_active_payload();
    std::construct_at(std::addressof(payload.error), std::move(error));
    state = state_type::error;
  }

  [[nodiscard]] auto has_value() const noexcept -> bool { return state == state_type::value; }

  [[nodiscard]] auto is_closed() const noexcept -> bool { return state == state_type::closed; }

  [[nodiscard]] auto is_error() const noexcept -> bool { return state == state_type::error; }

  [[nodiscard]] auto is_stopped() const noexcept -> bool { return state == state_type::stopped; }

  [[nodiscard]] auto value() noexcept -> value_t & { return payload.value; }
  [[nodiscard]] auto value() const noexcept -> const value_t & { return payload.value; }

  [[nodiscard]] auto error() const noexcept -> const std::exception_ptr & { return payload.error; }

private:
  auto destroy_active_payload() noexcept -> void {
    if (state == state_type::value) {
      std::destroy_at(std::addressof(payload.value));
    } else if (state == state_type::error) {
      payload.error.~exception_ptr();
    }
    state = state_type::empty;
  }
};

} // namespace wh::core::detail
