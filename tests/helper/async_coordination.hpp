#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

namespace wh::testing::helper {

class async_event {
  struct waiter {
    virtual ~waiter() = default;
    virtual auto resume() noexcept -> void = 0;
  };

  struct state {
    std::mutex mutex{};
    bool signaled{false};
    std::vector<waiter *> waiters{};
  };

public:
  async_event() : state_(std::make_shared<state>()) {}

  auto signal() const noexcept -> void {
    std::vector<waiter *> ready{};
    {
      std::lock_guard lock{state_->mutex};
      if (state_->signaled) {
        return;
      }
      state_->signaled = true;
      ready.swap(state_->waiters);
    }

    for (auto *entry : ready) {
      if (entry != nullptr) {
        entry->resume();
      }
    }
  }

  class sender {
  public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

    explicit sender(std::shared_ptr<state> state) : state_(std::move(state)) {}

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    class operation final : private waiter {
    public:
      using operation_state_concept = stdexec::operation_state_t;

      operation(std::shared_ptr<state> state, receiver_t receiver)
          : state_(std::move(state)), receiver_(std::move(receiver)) {}

      operation(const operation &) = delete;
      auto operator=(const operation &) -> operation & = delete;

      operation(operation &&) = delete;
      auto operator=(operation &&) -> operation & = delete;

      ~operation() { detach(); }

      auto start() & noexcept -> void {
        bool ready = false;
        {
          std::lock_guard lock{state_->mutex};
          if (state_->signaled) {
            ready = true;
          } else {
            state_->waiters.push_back(this);
            registered_ = true;
          }
        }

        if (ready) {
          resume();
        }
      }

    private:
      auto resume() noexcept -> void override {
        if (completed_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        {
          std::lock_guard lock{state_->mutex};
          registered_ = false;
        }
        stdexec::set_value(std::move(receiver_));
      }

      auto detach() noexcept -> void {
        if (completed_.load(std::memory_order_acquire)) {
          return;
        }
        std::lock_guard lock{state_->mutex};
        if (!registered_) {
          return;
        }
        std::erase(state_->waiters, static_cast<waiter *>(this));
        registered_ = false;
      }

      std::shared_ptr<state> state_{};
      receiver_t receiver_{};
      std::atomic<bool> completed_{false};
      bool registered_{false};
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    auto connect(receiver_t receiver) const -> operation<std::remove_cvref_t<receiver_t>> {
      using operation_t = operation<std::remove_cvref_t<receiver_t>>;
      return operation_t{state_, std::move(receiver)};
    }

    template <typename self_t, typename... env_t>
      requires std::same_as<std::remove_cvref_t<self_t>, sender> && (sizeof...(env_t) >= 1U)
    static consteval auto get_completion_signatures() {
      return completion_signatures{};
    }

  private:
    std::shared_ptr<state> state_{};
  };

  [[nodiscard]] auto wait() const -> sender { return sender{state_}; }

private:
  std::shared_ptr<state> state_{};
};

class async_pair_gate_registry {
  struct waiter {
    virtual ~waiter() = default;
    virtual auto resume() noexcept -> void = 0;
    virtual auto clear_registration_locked() noexcept -> void = 0;
  };

  struct gate_entry {
    waiter *first{nullptr};
  };

  struct state {
    std::mutex mutex{};
    std::unordered_map<int, gate_entry> gates{};
  };

public:
  async_pair_gate_registry() : state_(std::make_shared<state>()) {}

  class sender {
  public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

    sender(std::shared_ptr<state> state, int key) : state_(std::move(state)), key_(key) {}

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    class operation final : private waiter {
    public:
      using operation_state_concept = stdexec::operation_state_t;

      operation(std::shared_ptr<state> state, int key, receiver_t receiver)
          : state_(std::move(state)), key_(key), receiver_(std::move(receiver)) {}

      operation(const operation &) = delete;
      auto operator=(const operation &) -> operation & = delete;

      operation(operation &&) = delete;
      auto operator=(operation &&) -> operation & = delete;

      ~operation() { detach(); }

      auto start() & noexcept -> void {
        waiter *peer = nullptr;
        {
          std::lock_guard lock{state_->mutex};
          auto &entry = state_->gates[key_];
          if (entry.first == nullptr) {
            entry.first = this;
            registered_ = true;
            return;
          }

          peer = entry.first;
          peer->clear_registration_locked();
          state_->gates.erase(key_);
        }

        peer->resume();
        resume();
      }

    private:
      auto resume() noexcept -> void override {
        if (completed_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        {
          std::lock_guard lock{state_->mutex};
          registered_ = false;
        }
        stdexec::set_value(std::move(receiver_));
      }

      auto clear_registration_locked() noexcept -> void override { registered_ = false; }

      auto detach() noexcept -> void {
        if (completed_.load(std::memory_order_acquire)) {
          return;
        }
        std::lock_guard lock{state_->mutex};
        if (!registered_) {
          return;
        }
        if (auto iter = state_->gates.find(key_);
            iter != state_->gates.end() && iter->second.first == static_cast<waiter *>(this)) {
          state_->gates.erase(iter);
        }
        registered_ = false;
      }

      std::shared_ptr<state> state_{};
      int key_{0};
      receiver_t receiver_{};
      std::atomic<bool> completed_{false};
      bool registered_{false};
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    auto connect(receiver_t receiver) const -> operation<std::remove_cvref_t<receiver_t>> {
      using operation_t = operation<std::remove_cvref_t<receiver_t>>;
      return operation_t{state_, key_, std::move(receiver)};
    }

    template <typename self_t, typename... env_t>
      requires std::same_as<std::remove_cvref_t<self_t>, sender> && (sizeof...(env_t) >= 1U)
    static consteval auto get_completion_signatures() {
      return completion_signatures{};
    }

  private:
    std::shared_ptr<state> state_{};
    int key_{0};
  };

  [[nodiscard]] auto arrive(const int key) const -> sender { return sender{state_, key}; }

private:
  std::shared_ptr<state> state_{};
};

} // namespace wh::testing::helper
