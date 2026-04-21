#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

#include "wh/core/compiler.hpp"

namespace wh::core::cursor_reader_detail {

template <typename value_t, typename allocator_t = std::allocator<value_t>>
class retained_ring_storage {
public:
  using value_type = value_t;
  using allocator_type = allocator_t;
  using allocator_traits = std::allocator_traits<allocator_type>;

  explicit retained_ring_storage(const std::size_t capacity,
                                 const allocator_type &allocator = allocator_type{})
      : allocator_(allocator), capacity_(capacity) {
    if (capacity_ != 0U) {
      storage_ = allocator_traits::allocate(allocator_, capacity_);
    }
  }

  retained_ring_storage(const retained_ring_storage &) = delete;
  auto operator=(const retained_ring_storage &) -> retained_ring_storage & = delete;

  retained_ring_storage(retained_ring_storage &&other) noexcept
      : allocator_(std::move(other.allocator_)), capacity_(other.capacity_), size_(other.size_),
        head_(other.head_), head_sequence_(other.head_sequence_), storage_(other.storage_) {
    other.capacity_ = 0U;
    other.size_ = 0U;
    other.head_ = 0U;
    other.head_sequence_ = 0U;
    other.storage_ = nullptr;
  }

  auto operator=(retained_ring_storage &&other) noexcept -> retained_ring_storage & {
    if (this == &other) {
      return *this;
    }

    destroy_all();
    allocator_ = std::move(other.allocator_);
    capacity_ = other.capacity_;
    size_ = other.size_;
    head_ = other.head_;
    head_sequence_ = other.head_sequence_;
    storage_ = other.storage_;

    other.capacity_ = 0U;
    other.size_ = 0U;
    other.head_ = 0U;
    other.head_sequence_ = 0U;
    other.storage_ = nullptr;
    return *this;
  }

  ~retained_ring_storage() { destroy_all(); }

  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0U; }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t { return capacity_; }

  [[nodiscard]] auto front_sequence() const noexcept -> std::uint64_t { return head_sequence_; }

  [[nodiscard]] auto end_sequence() const noexcept -> std::uint64_t {
    return head_sequence_ + size_;
  }

  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type { return allocator_; }

  auto reserve(const std::size_t new_capacity) -> void
    requires std::copy_constructible<value_type>
  {
    if (new_capacity <= capacity_) {
      return;
    }

    value_type *new_storage = allocator_traits::allocate(allocator_, new_capacity);
    std::size_t constructed = 0U;
    try {
      for (std::size_t offset = 0U; offset < size_; ++offset) {
        allocator_traits::construct(allocator_, new_storage + offset, value_at_offset(offset));
        ++constructed;
      }
    } catch (...) {
      for (std::size_t offset = 0U; offset < constructed; ++offset) {
        allocator_traits::destroy(allocator_, new_storage + offset);
      }
      allocator_traits::deallocate(allocator_, new_storage, new_capacity);
      throw;
    }

    const auto preserved_head_sequence = head_sequence_;
    destroy_all();
    storage_ = new_storage;
    capacity_ = new_capacity;
    size_ = constructed;
    head_ = 0U;
    head_sequence_ = preserved_head_sequence;
  }

  template <typename... args_t>
    requires std::constructible_from<value_type, args_t &&...>
  auto emplace_back(args_t &&...args) -> void {
    assert(!full());
    allocator_traits::construct(allocator_, storage_ + tail_index(), std::forward<args_t>(args)...);
    ++size_;
  }

  auto destroy_front() noexcept -> void {
    assert(!empty());
    allocator_traits::destroy(allocator_, storage_ + head_);
    head_ = advance_index(head_);
    --size_;
    ++head_sequence_;
    if (size_ == 0U) {
      head_ = 0U;
    }
  }

  [[nodiscard]] auto value_at_sequence(const std::uint64_t sequence) noexcept -> value_type & {
    assert(contains(sequence));
    return storage_[slot_index(sequence)];
  }

  [[nodiscard]] auto value_at_sequence(const std::uint64_t sequence) const noexcept
      -> const value_type & {
    assert(contains(sequence));
    return storage_[slot_index(sequence)];
  }

private:
  [[nodiscard]] auto full() const noexcept -> bool { return size_ == capacity_; }

  [[nodiscard]] auto contains(const std::uint64_t sequence) const noexcept -> bool {
    return sequence >= head_sequence_ && sequence < end_sequence();
  }

  [[nodiscard]] auto tail_index() const noexcept -> std::size_t {
    if (capacity_ == 0U) {
      return 0U;
    }
    const auto tail = head_ + size_;
    return tail >= capacity_ ? (tail - capacity_) : tail;
  }

  [[nodiscard]] auto advance_index(const std::size_t index) const noexcept -> std::size_t {
    if (capacity_ == 0U) {
      return 0U;
    }
    const auto next = index + 1U;
    return next == capacity_ ? 0U : next;
  }

  [[nodiscard]] auto slot_index(const std::uint64_t sequence) const noexcept -> std::size_t {
    const auto offset = static_cast<std::size_t>(sequence - head_sequence_);
    const auto index = head_ + offset;
    return index >= capacity_ ? (index - capacity_) : index;
  }

  [[nodiscard]] auto value_at_offset(const std::size_t offset) const noexcept
      -> const value_type & {
    assert(offset < size_);
    const auto index = head_ + offset;
    return storage_[index >= capacity_ ? (index - capacity_) : index];
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
    capacity_ = 0U;
    size_ = 0U;
    head_ = 0U;
    head_sequence_ = 0U;
  }

  wh_no_unique_address allocator_type allocator_{};
  std::size_t capacity_{0U};
  std::size_t size_{0U};
  std::size_t head_{0U};
  std::uint64_t head_sequence_{0U};
  value_type *storage_{nullptr};
};

} // namespace wh::core::cursor_reader_detail
