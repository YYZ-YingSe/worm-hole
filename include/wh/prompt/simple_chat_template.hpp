// Implements template-driven prompt rendering that expands template values into
// ordered chat messages with strict/non-strict missing-variable modes.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/prompt/chat_template.hpp"

namespace wh::prompt {

struct prompt_message_template {
  wh::schema::message_role role{wh::schema::message_role::user};
  std::string text{};
  std::string name{};
};

namespace detail {

class simple_chat_template_impl {
public:
  simple_chat_template_impl() = default;
  explicit simple_chat_template_impl(
      const std::vector<prompt_message_template> &templates)
      : templates_(templates) {}
  explicit simple_chat_template_impl(
      std::vector<prompt_message_template> &&templates)
      : templates_(std::move(templates)) {}

  [[nodiscard]] auto render(const prompt_render_request &request,
                            prompt_callback_event &event) const
      -> prompt_result {
    return render_common(request, event);
  }

  [[nodiscard]] auto render(prompt_render_request &&request,
                            prompt_callback_event &event) const
      -> prompt_result {
    return render_common(std::move(request), event);
  }

  auto add_template(const prompt_message_template &value)
      -> simple_chat_template_impl & {
    templates_.push_back(value);
    return *this;
  }

  auto add_template(prompt_message_template &&value)
      -> simple_chat_template_impl & {
    templates_.push_back(std::move(value));
    return *this;
  }

  [[nodiscard]] auto templates() const noexcept
      -> const std::vector<prompt_message_template> & {
    return templates_;
  }

private:
  template <typename request_t>
  [[nodiscard]] auto render_common(request_t &&request,
                                   prompt_callback_event &event) const
      -> prompt_result {
    const auto extract_missing_variable =
        [](const std::string &text) -> std::string {
      const auto open = text.find("{{");
      if (open == std::string::npos) {
        return {};
      }
      const auto close = text.find("}}", open + 2U);
      if (close == std::string::npos || close <= open + 2U) {
        return {};
      }
      auto token = std::string{text.substr(open + 2U, close - (open + 2U))};
      const auto first = token.find_first_not_of(" \t\r\n");
      if (first == std::string::npos) {
        return {};
      }
      const auto last = token.find_last_not_of(" \t\r\n");
      return token.substr(first, last - first + 1U);
    };

    const auto options = request.options.resolve_view();
    std::vector<wh::schema::message> output{};
    output.reserve(templates_.size());

    for (const auto &entry : templates_) {
      auto rendered = wh::prompt::render_text_template(
          entry.text, request.context, options.syntax);
      if (rendered.has_error()) {
        if (rendered.error() == wh::core::errc::not_found &&
            !options.strict_missing_variables) {
          rendered = std::string{entry.text};
        } else {
          event.rendered_message_count = output.size();
          event.failed_template = entry.name.empty()
                                      ? std::string{options.template_name}
                                      : entry.name;
          event.failed_variable = extract_missing_variable(entry.text);
          return prompt_result::failure(rendered.error());
        }
      }

      wh::schema::message message{};
      message.role = entry.role;
      message.name = entry.name;
      message.parts.emplace_back(
          wh::schema::text_part{std::move(rendered).value()});
      output.push_back(std::move(message));
    }

    return output;
  }

  std::vector<prompt_message_template> templates_{};
};

} // namespace detail

class simple_chat_template final
    : public chat_template<detail::simple_chat_template_impl> {
  using base_t = chat_template<detail::simple_chat_template_impl>;

public:
  simple_chat_template() : base_t(detail::simple_chat_template_impl{}) {}
  explicit simple_chat_template(
      const std::vector<prompt_message_template> &templates)
      : base_t(detail::simple_chat_template_impl{templates}) {}
  explicit simple_chat_template(
      std::vector<prompt_message_template> &&templates)
      : base_t(detail::simple_chat_template_impl{std::move(templates)}) {}

  auto add_template(const prompt_message_template &value)
      -> simple_chat_template & {
    this->impl().add_template(value);
    return *this;
  }

  auto add_template(prompt_message_template &&value) -> simple_chat_template & {
    this->impl().add_template(std::move(value));
    return *this;
  }

  [[nodiscard]] auto templates() const noexcept
      -> const std::vector<prompt_message_template> & {
    return this->impl().templates();
  }
};

} // namespace wh::prompt
