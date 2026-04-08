#pragma once

#include <cassert>
#include <concepts>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace wh::core::detail {

template <typename value_t, typename allocator_t = std::allocator<value_t>>
class ring_storage {
public:
  using value_type = value_t;
  using allocator_type = allocator_t;
  using allocator_traits = std::allocator_traits<allocator_type>;

  explicit ring_storage(const std::size_t capacity,
                        const allocator_type &allocator = allocator_type{})
      : allocator_(allocator), capacity_(capacity) {
    if (capacity_ != 0U) {
      storage_ = allocator_traits::allocate(allocator_, capacity_);
    }
  }

  ring_storage(const ring_storage &) = delete;
  auto operator=(const ring_storage &) -> ring_storage & = delete;

  ring_storage(ring_storage &&other) noexcept
      : allocator_(std::move(other.allocator_)), capacity_(other.capacity_),
        size_(other.size_), head_(other.head_), storage_(other.storage_) {
    other.capacity_ = 0U;
    other.size_ = 0U;
    other.head_ = 0U;
    other.storage_ = nullptr;
  }

  auto operator=(ring_storage &&other) noexcept -> ring_storage & {
    if (this == &other) {
      return *this;
    }
    destroy_all();

    allocator_ = std::move(other.allocator_);
    capacity_ = other.capacity_;
    size_ = other.size_;
    head_ = other.head_;
    storage_ = other.storage_;

    other.capacity_ = 0U;
    other.size_ = 0U;
    other.head_ = 0U;
    other.storage_ = nullptr;
    return *this;
  }

  ~ring_storage() { destroy_all(); }

  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0U; }
  [[nodiscard]] auto full() const noexcept -> bool {
    return size_ == capacity_;
  }
  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return capacity_;
  }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
    return allocator_;
  }

  template <typename... args_t>
    requires std::constructible_from<value_t, args_t &&...>
  auto emplace_back(args_t &&...args) -> void {
    assert(!full());
    allocator_traits::construct(allocator_, storage_ + tail_index(),
                                std::forward<args_t>(args)...);
    ++size_;
  }

  auto push_back(const value_t &value) -> void
    requires std::copy_constructible<value_t>
  {
    emplace_back(value);
  }

  auto push_back(value_t &&value) -> void
    requires std::move_constructible<value_t>
  {
    emplace_back(std::move(value));
  }

  [[nodiscard]] auto pop_front() -> value_t {
    assert(!empty());
    std::optional<value_t> result{};
    consume_front([&](value_t &&value) { result.emplace(std::move(value)); });
    return std::move(*result);
  }

  template <typename sink_t> auto consume_front(sink_t &&sink) -> void {
    assert(!empty());
    auto *slot = storage_ + head_;
    head_ = advance_index(head_);
    --size_;

    try {
      std::forward<sink_t>(sink)(std::move(*slot));
      allocator_traits::destroy(allocator_, slot);
    } catch (...) {
      allocator_traits::destroy(allocator_, slot);
      throw;
    }
  }

private:
  [[nodiscard]] auto tail_index() const noexcept -> std::size_t {
    if (capacity_ == 0U) {
      return 0U;
    }
    const auto tail = head_ + size_;
    return tail >= capacity_ ? (tail - capacity_) : tail;
  }

  [[nodiscard]] auto advance_index(const std::size_t index) const noexcept
      -> std::size_t {
    if (capacity_ == 0U) {
      return 0U;
    }
    const auto next = index + 1U;
    return next == capacity_ ? 0U : next;
  }

  auto destroy_all() noexcept -> void {
    if (storage_ == nullptr) {
      return;
    }

    auto index = head_;
    for (std::size_t remaining = size_; remaining > 0U; --remaining) {
      allocator_traits::destroy(allocator_, storage_ + index);
      index = advance_index(index);
    }
    allocator_traits::deallocate(allocator_, storage_, capacity_);
    storage_ = nullptr;
    size_ = 0U;
    head_ = 0U;
    capacity_ = 0U;
  }

  [[no_unique_address]] allocator_type allocator_{};
  std::size_t capacity_{0U};
  std::size_t size_{0U};
  std::size_t head_{0U};
  value_t *storage_{nullptr};
};

} // namespace wh::core::detail
