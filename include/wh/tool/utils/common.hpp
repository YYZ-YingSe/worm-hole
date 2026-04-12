// Provides declarations and utilities for `wh/tool/utils/common.hpp`.
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace wh::tool::utils {

/// Converts snake/kebab/spaced words to CamelCase.
[[nodiscard]] inline auto to_camel_case(std::string_view input) -> std::string {
  std::string output{};
  output.reserve(input.size());

  bool capitalize_next = true;
  for (const auto ch : input) {
    if (ch == '_' || ch == '-' || ch == ' ') {
      capitalize_next = true;
      continue;
    }

    if (capitalize_next) {
      output.push_back(
          static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
      capitalize_next = false;
    } else {
      output.push_back(ch);
    }
  }
  return output;
}

} // namespace wh::tool::utils
