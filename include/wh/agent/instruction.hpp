// Defines agent instruction composition primitives with deterministic append
// and replace precedence.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wh::agent {

/// How one instruction fragment participates in final rendering.
enum class instruction_mode {
  /// Concatenate this fragment after the current base instruction.
  append = 0U,
  /// Replace the current base instruction at the given priority.
  replace,
};

/// One authored instruction fragment with explicit precedence.
struct instruction_entry {
  /// Raw instruction text carried by this fragment.
  std::string text{};
  /// Higher values win over lower values.
  std::int32_t priority{0};
  /// Append or replace behavior for this fragment.
  instruction_mode mode{instruction_mode::append};
  /// Stable insertion sequence used for deterministic tie-breaking.
  std::size_t sequence{0U};
};

/// Mutable instruction builder with predictable append/replace precedence.
class instruction {
public:
  /// Appends one fragment at `priority`.
  auto append(std::string text, const std::int32_t priority = 0) -> void {
    entries_.push_back(instruction_entry{
        .text = std::move(text),
        .priority = priority,
        .mode = instruction_mode::append,
        .sequence = next_sequence_++,
    });
  }

  /// Replaces the current base instruction at `priority`.
  auto replace(std::string text, const std::int32_t priority = 0) -> void {
    entries_.push_back(instruction_entry{
        .text = std::move(text),
        .priority = priority,
        .mode = instruction_mode::replace,
        .sequence = next_sequence_++,
    });
  }

  /// Returns true when no fragments have been registered.
  [[nodiscard]] auto empty() const noexcept -> bool { return entries_.empty(); }

  /// Exposes the raw fragment list for diagnostics and tests.
  [[nodiscard]] auto entries() const noexcept
      -> std::span<const instruction_entry> {
    return {entries_.data(), entries_.size()};
  }

  /// Renders the final instruction string using deterministic precedence:
  /// highest-priority replace becomes the base, then append fragments at the
  /// same or higher priority are concatenated in stable order.
  [[nodiscard]] auto render(const std::string_view separator = "\n") const
      -> std::string {
    if (entries_.empty()) {
      return {};
    }

    const instruction_entry *base_replace = nullptr;
    for (const auto &entry : entries_) {
      if (entry.mode != instruction_mode::replace) {
        continue;
      }
      if (base_replace == nullptr || entry.priority > base_replace->priority ||
          (entry.priority == base_replace->priority &&
           entry.sequence > base_replace->sequence)) {
        base_replace = std::addressof(entry);
      }
    }

    std::vector<const instruction_entry *> append_entries{};
    append_entries.reserve(entries_.size());
    const auto minimum_priority =
        base_replace == nullptr ? std::int32_t{} : base_replace->priority;
    for (const auto &entry : entries_) {
      if (entry.mode != instruction_mode::append) {
        continue;
      }
      if (base_replace != nullptr && entry.priority < minimum_priority) {
        continue;
      }
      append_entries.push_back(std::addressof(entry));
    }

    std::stable_sort(
        append_entries.begin(), append_entries.end(),
        [](const instruction_entry *left, const instruction_entry *right) {
          if (left->priority != right->priority) {
            return left->priority < right->priority;
          }
          return left->sequence < right->sequence;
        });

    std::string rendered{};
    auto append_fragment = [&](const std::string &fragment) -> void {
      if (fragment.empty()) {
        return;
      }
      if (!rendered.empty()) {
        rendered.append(separator);
      }
      rendered.append(fragment);
    };

    if (base_replace != nullptr) {
      append_fragment(base_replace->text);
    }
    for (const auto *entry : append_entries) {
      append_fragment(entry->text);
    }
    return rendered;
  }

private:
  /// Stable fragment storage in insertion order.
  std::vector<instruction_entry> entries_{};
  /// Monotonic insertion sequence for deterministic ties.
  std::size_t next_sequence_{0U};
};

} // namespace wh::agent
