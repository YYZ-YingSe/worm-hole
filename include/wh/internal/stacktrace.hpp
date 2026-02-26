#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

#include "wh/core/compiler.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <execinfo.h>
#endif

namespace wh::internal {

[[nodiscard]] inline auto capture_call_stack() -> std::string {
#if defined(_WIN32)
  std::array<void *, 64U> frames{};
  const auto captured = static_cast<std::size_t>(
      ::CaptureStackBackTrace(0U, static_cast<DWORD>(frames.size()),
                              frames.data(), nullptr));
  if (captured == 0U) {
    return "stack-unavailable";
  }

  std::ostringstream stack_stream{};
  for (std::size_t index = 0U; index < captured; ++index) {
    if (index != 0U) {
      stack_stream << '\n';
    }
    stack_stream << index << ": 0x" << std::hex
                 << reinterpret_cast<std::uintptr_t>(frames[index]) << std::dec;
  }
  auto stack = stack_stream.str();
  if (stack.empty()) {
    return "stack-unavailable";
  }
  return stack;
#elif defined(__unix__) || defined(__APPLE__)
  std::array<void *, 64U> frames{};
  const auto captured =
      ::backtrace(frames.data(), static_cast<int>(frames.size()));
  if (captured <= 0) {
    return "stack-unavailable";
  }

  char **symbols = ::backtrace_symbols(frames.data(), captured);
  if (symbols == nullptr) {
    return "stack-unavailable";
  }

  std::string stack{};
  for (int index = 0; index < captured; ++index) {
    if (index != 0) {
      stack.push_back('\n');
    }
    stack += symbols[index];
  }
  std::free(symbols);

  if (stack.empty()) {
    return "stack-unavailable";
  }
  return stack;
#else
  return "stack-unavailable";
#endif
}

} // namespace wh::internal
