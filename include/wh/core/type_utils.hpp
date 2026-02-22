#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if __has_include(<stdexec/execution.hpp>)
#include <stdexec/execution.hpp>
#define wh_has_stdexec 1
#else
#define wh_has_stdexec 0
#endif

#include "wh/core/compiler.hpp"
#include "wh/internal/type_name.hpp"

namespace wh::core {

template <typename value_t, typename error_t>
class result;

template <typename t>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<t>>;

template <typename t>
struct type_tag {
  using type = t;
};

template <typename t>
using type_of_t = typename type_tag<remove_cvref_t<t>>::type;

template <typename t>
[[nodiscard]] consteval auto type_of() noexcept -> type_tag<remove_cvref_t<t>> {
  return {};
}

template <typename t>
concept default_initializable_object = std::default_initializable<remove_cvref_t<t>>;

template <typename t>
concept container_like = requires(remove_cvref_t<t> container) {
  typename remove_cvref_t<t>::value_type;
  container.begin();
  container.end();
  container.size();
};

template <typename t>
concept pair_like = requires(remove_cvref_t<t> value) {
  typename remove_cvref_t<t>::first_type;
  typename remove_cvref_t<t>::second_type;
  value.first;
  value.second;
};

template <typename t>
struct is_optional : std::false_type {};

template <typename value_t>
struct is_optional<std::optional<value_t>> : std::true_type {};

template <typename t>
inline constexpr bool is_optional_v = is_optional<remove_cvref_t<t>>::value;

template <typename t>
struct is_result : std::false_type {};

template <typename value_t, typename error_t>
struct is_result<result<value_t, error_t>> : std::true_type {};

template <typename t>
inline constexpr bool is_result_v = is_result<remove_cvref_t<t>>::value;

template <typename t>
concept expected_like = requires(remove_cvref_t<t> value) {
  { value.has_value() } -> std::convertible_to<bool>;
  value.value();
  value.error();
};

template <typename t>
struct is_sender : std::false_type {};

#if wh_has_stdexec
template <typename t>
  requires stdexec::sender<remove_cvref_t<t>>
struct is_sender<t> : std::true_type {};
#endif

template <typename t>
inline constexpr bool is_sender_v = is_sender<remove_cvref_t<t>>::value;

template <typename callable_t, typename... args_t>
concept callable_with = std::invocable<callable_t, args_t...>;

template <typename callable_t, typename... args_t>
using callable_result_t = std::invoke_result_t<callable_t, args_t...>;

template <typename... ts>
struct type_list {};

template <typename list_t>
struct type_list_size;

template <typename... ts>
struct type_list_size<type_list<ts...>>
    : std::integral_constant<std::size_t, sizeof...(ts)> {};

template <std::size_t index, typename list_t>
struct type_list_at;

template <std::size_t index, typename head_t, typename... tail_t>
struct type_list_at<index, type_list<head_t, tail_t...>>
    : type_list_at<index - 1U, type_list<tail_t...>> {};

template <typename head_t, typename... tail_t>
struct type_list_at<0U, type_list<head_t, tail_t...>> {
  using type = head_t;
};

template <typename list_t>
struct type_list_reverse;

template <>
struct type_list_reverse<type_list<>> {
  using type = type_list<>;
};

template <typename head_t, typename... tail_t>
struct type_list_reverse<type_list<head_t, tail_t...>> {
 private:
  using tail_reversed = typename type_list_reverse<type_list<tail_t...>>::type;

  template <typename reversed_t>
  struct append_head;

  template <typename... reversed_items_t>
  struct append_head<type_list<reversed_items_t...>> {
    using type = type_list<reversed_items_t..., head_t>;
  };

 public:
  using type = typename append_head<tail_reversed>::type;
};

template <typename t>
struct function_traits;

template <typename return_t, typename... args_t>
struct function_traits<return_t(args_t...)> {
  using return_type = return_t;
  using argument_types = type_list<args_t...>;
};

template <typename return_t, typename... args_t>
struct function_traits<return_t (*)(args_t...)> : function_traits<return_t(args_t...)> {};

template <typename class_t, typename return_t, typename... args_t>
struct function_traits<return_t (class_t::*)(args_t...)>
    : function_traits<return_t(args_t...)> {};

template <typename class_t, typename return_t, typename... args_t>
struct function_traits<return_t (class_t::*)(args_t...) const>
    : function_traits<return_t(args_t...)> {};

template <typename callable_t>
  requires requires {
    &remove_cvref_t<callable_t>::operator();
  }
struct function_traits<callable_t>
    : function_traits<decltype(&remove_cvref_t<callable_t>::operator())> {};

template <typename t>
using function_argument_types_t = typename function_traits<remove_cvref_t<t>>::argument_types;

template <typename t>
using function_return_t = typename function_traits<remove_cvref_t<t>>::return_type;

template <typename t>
struct default_instance_factory {
  [[nodiscard]] static auto make() -> remove_cvref_t<t>
    requires default_initializable_object<t>
  {
    return remove_cvref_t<t>{};
  }
};

template <typename t>
struct default_instance_factory<t*> {
  [[nodiscard]] static auto make() -> t* {
    using pointee_t = std::remove_cv_t<t>;
    auto* value = new pointee_t(default_instance_factory<pointee_t>::make());
    return value;
  }
};

template <typename t>
[[nodiscard]] auto default_instance() -> remove_cvref_t<t> {
  return default_instance_factory<remove_cvref_t<t>>::make();
}

template <typename value_t>
[[nodiscard]] auto wrap_unique(value_t&& value)
    -> std::unique_ptr<remove_cvref_t<value_t>> {
  using normalized_t = remove_cvref_t<value_t>;
  return std::make_unique<normalized_t>(std::forward<value_t>(value));
}

template <typename first_t, typename second_t>
using pair_type = std::pair<first_t, second_t>;

template <typename sequence_t>
[[nodiscard]] auto reverse_copy(const sequence_t& sequence)
    -> std::vector<typename sequence_t::value_type> {
  std::vector<typename sequence_t::value_type> output;
  output.reserve(sequence.size());
  for (auto iter = sequence.rbegin(); iter != sequence.rend(); ++iter) {
    output.push_back(*iter);
  }
  return output;
}

template <typename map_out_t, typename map_in_t>
[[nodiscard]] auto map_copy_as(const map_in_t& input) -> map_out_t {
  map_out_t output;
  for (const auto& [key, value] : input) {
    output.insert_or_assign(key, value);
  }
  return output;
}

template <typename t>
[[nodiscard]] constexpr std::string_view stable_type_token() noexcept {
  return ::wh::internal::persistent_type_alias<t>();
}

template <typename t>
[[nodiscard]] constexpr std::string_view diagnostic_type_token() noexcept {
  return ::wh::internal::diagnostic_type_alias<t>();
}

}  // namespace wh::core
