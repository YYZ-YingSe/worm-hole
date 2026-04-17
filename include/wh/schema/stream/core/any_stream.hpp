// Defines type-erased stream reader/writer handles with stable async sender
// boundaries while preserving the real downstream receiver env.
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/detail/receiver_stop_bridge.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/stream_base.hpp"
#include "wh/schema/stream/core/types.hpp"

namespace wh::schema::stream {

template <typename value_t, std::size_t storage_size = 128U>
class any_stream_reader;
template <typename value_t, std::size_t storage_size = 64U>
class any_stream_writer;
template <typename value_t, std::size_t storage_size = 128U>
class any_stream_read_sender;
template <typename value_t, std::size_t storage_size = 64U>
class any_stream_write_sender;

namespace detail {

template <typename receiver_t>
[[noreturn]] inline auto missing_receiver_scheduler() noexcept
    -> wh::core::detail::any_resume_scheduler_t {
  static_assert(
      wh::core::detail::receiver_with_resume_scheduler<receiver_t>,
      "any_stream async sender requires receiver env to expose scheduler or "
      "completion scheduler");
  std::abort();
}

template <std::size_t storage_size> struct erased_storage {
  alignas(std::max_align_t) std::array<std::byte, storage_size> data{};
};

template <typename model_t, std::size_t storage_size>
inline constexpr bool fits_erased_storage_v =
    sizeof(model_t) <= storage_size &&
    alignof(model_t) <= alignof(erased_storage<storage_size>) &&
    std::is_nothrow_move_constructible_v<model_t>;

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto
storage_ptr(erased_storage<storage_size> &storage) noexcept -> model_t * {
  return std::launder(reinterpret_cast<model_t *>(storage.data.data()));
}

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto
storage_ptr(const erased_storage<storage_size> &storage) noexcept
    -> const model_t * {
  return std::launder(reinterpret_cast<const model_t *>(storage.data.data()));
}

template <typename value_t>
using chunk_result_t = stream_result<stream_chunk<value_t>>;

template <typename value_t>
using chunk_try_result_t = stream_try_result<stream_chunk<value_t>>;

template <typename model_t, std::size_t storage_size>
using erased_storage_model_t =
    std::conditional_t<fits_erased_storage_v<model_t, storage_size>, model_t,
                       std::unique_ptr<model_t>>;

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto
storage_model_ptr(erased_storage<storage_size> &storage) noexcept
    -> erased_storage_model_t<model_t, storage_size> * {
  return storage_ptr<erased_storage_model_t<model_t, storage_size>>(storage);
}

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto
storage_model_ptr(const erased_storage<storage_size> &storage) noexcept
    -> const erased_storage_model_t<model_t, storage_size> * {
  return storage_ptr<erased_storage_model_t<model_t, storage_size>>(storage);
}

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto storage_model_ptr(void *storage) noexcept
    -> erased_storage_model_t<model_t, storage_size> * {
  return std::launder(
      reinterpret_cast<erased_storage_model_t<model_t, storage_size> *>(
          storage));
}

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto storage_model_ptr(const void *storage) noexcept
    -> const erased_storage_model_t<model_t, storage_size> * {
  return std::launder(
      reinterpret_cast<const erased_storage_model_t<model_t, storage_size> *>(
          storage));
}

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto
storage_object(erased_storage<storage_size> &storage) noexcept -> model_t * {
  if constexpr (fits_erased_storage_v<model_t, storage_size>) {
    return storage_ptr<model_t>(storage);
  } else {
    return storage_model_ptr<model_t, storage_size>(storage)->get();
  }
}

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto
storage_object(const erased_storage<storage_size> &storage) noexcept
    -> const model_t * {
  if constexpr (fits_erased_storage_v<model_t, storage_size>) {
    return storage_ptr<model_t>(storage);
  } else {
    return storage_model_ptr<model_t, storage_size>(storage)->get();
  }
}

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto storage_object(void *storage) noexcept -> model_t * {
  if constexpr (fits_erased_storage_v<model_t, storage_size>) {
    return std::launder(reinterpret_cast<model_t *>(storage));
  } else {
    return storage_model_ptr<model_t, storage_size>(storage)->get();
  }
}

template <typename model_t, std::size_t storage_size>
[[nodiscard]] inline auto storage_object(const void *storage) noexcept
    -> const model_t * {
  if constexpr (fits_erased_storage_v<model_t, storage_size>) {
    return std::launder(reinterpret_cast<const model_t *>(storage));
  } else {
    return storage_model_ptr<model_t, storage_size>(storage)->get();
  }
}

template <typename receiver_ref_t> struct stream_op_vtable {
  void (*start)(void *object) noexcept {nullptr};
  void (*destroy)(void *object) noexcept {nullptr};
};

template <typename receiver_ref_t> class stream_op_handle {
public:
  stream_op_handle() = default;
  stream_op_handle(const stream_op_handle &) = delete;
  auto operator=(const stream_op_handle &) -> stream_op_handle & = delete;

  stream_op_handle(stream_op_handle &&other) noexcept
      : object_(std::exchange(other.object_, nullptr)),
        vtable_(std::exchange(other.vtable_, nullptr)) {}

  auto operator=(stream_op_handle &&other) noexcept -> stream_op_handle & {
    if (this != &other) {
      reset();
      object_ = std::exchange(other.object_, nullptr);
      vtable_ = std::exchange(other.vtable_, nullptr);
    }
    return *this;
  }

  ~stream_op_handle() { reset(); }

  auto start() noexcept -> void {
    if (object_ != nullptr) {
      vtable_->start(object_);
    }
  }

  auto reset() noexcept -> void {
    if (object_ != nullptr) {
      vtable_->destroy(object_);
      object_ = nullptr;
      vtable_ = nullptr;
    }
  }

  auto bind(void *object,
            const stream_op_vtable<receiver_ref_t> *vtable) noexcept -> void {
    reset();
    object_ = object;
    vtable_ = vtable;
  }

private:
  void *object_{nullptr};
  const stream_op_vtable<receiver_ref_t> *vtable_{nullptr};
};

template <stdexec::sender sender_t, typename receiver_ref_t>
class stream_op_model {
public:
  using operation_t = stdexec::connect_result_t<sender_t, receiver_ref_t>;

  stream_op_model(sender_t sender, receiver_ref_t receiver)
      : operation_(stdexec::connect(std::move(sender), std::move(receiver))) {}

  static auto destroy(void *object) noexcept -> void {
    delete static_cast<stream_op_model *>(object);
  }

  static auto start(void *object) noexcept -> void {
    stdexec::start(static_cast<stream_op_model *>(object)->operation_);
  }

  static inline constexpr stream_op_vtable<receiver_ref_t> vtable{
      .start = &stream_op_model::start,
      .destroy = &stream_op_model::destroy,
  };

private:
  operation_t operation_;
};

using erased_resume_scheduler = wh::core::detail::any_resume_scheduler_t;

struct receiver_env_vtable {
  erased_resume_scheduler (*get_scheduler)(const void *) noexcept {nullptr};
  erased_resume_scheduler (*get_delegation_scheduler)(const void *) noexcept {
      nullptr};
  erased_resume_scheduler (*get_value_scheduler)(const void *) noexcept {
      nullptr};
  erased_resume_scheduler (*get_error_scheduler)(const void *) noexcept {
      nullptr};
  erased_resume_scheduler (*get_stopped_scheduler)(const void *) noexcept {
      nullptr};
  stdexec::inplace_stop_token (*get_stop_token)(const void *) noexcept {
      nullptr};
};

class receiver_env_ref {
public:
  receiver_env_ref() = default;

  receiver_env_ref(const void *object,
                   const receiver_env_vtable *vtable) noexcept
      : object_(object), vtable_(vtable) {}

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
      -> erased_resume_scheduler {
    return vtable_->get_scheduler(object_);
  }

  [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept
      -> erased_resume_scheduler {
    return vtable_->get_delegation_scheduler(object_);
  }

  [[nodiscard]] auto query(
      stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
      -> erased_resume_scheduler {
    return vtable_->get_value_scheduler(object_);
  }

  [[nodiscard]] auto query(
      stdexec::get_completion_scheduler_t<stdexec::set_error_t>) const noexcept
      -> erased_resume_scheduler {
    return vtable_->get_error_scheduler(object_);
  }

  [[nodiscard]] auto
  query(stdexec::get_completion_scheduler_t<stdexec::set_stopped_t>)
      const noexcept -> erased_resume_scheduler {
    return vtable_->get_stopped_scheduler(object_);
  }

  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
      -> stdexec::inplace_stop_token {
    return vtable_->get_stop_token(object_);
  }

private:
  const void *object_{nullptr};
  const receiver_env_vtable *vtable_{nullptr};
};

template <typename receiver_t> struct receiver_env_model {
  template <typename cpo_t>
  [[nodiscard]] static auto
  select_completion_scheduler(const void *object) noexcept
      -> erased_resume_scheduler {
    const auto &receiver = *static_cast<const receiver_t *>(object);
    const auto &env = stdexec::get_env(receiver);
    if constexpr (requires {
                    wh::core::detail::select_resume_scheduler<cpo_t>(env);
                  }) {
      return wh::core::detail::erase_resume_scheduler(
          wh::core::detail::select_resume_scheduler<cpo_t>(env));
    } else if constexpr (requires {
                           wh::core::detail::select_resume_scheduler<
                               stdexec::set_value_t>(env);
                         }) {
      return wh::core::detail::erase_resume_scheduler(
          wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(env));
    } else {
      return missing_receiver_scheduler<receiver_t>();
    }
  }

  [[nodiscard]] static auto get_scheduler(const void *object) noexcept
      -> erased_resume_scheduler {
    const auto &receiver = *static_cast<const receiver_t *>(object);
    const auto &env = stdexec::get_env(receiver);
    if constexpr (requires {
                    wh::core::detail::select_launch_scheduler(env);
                  }) {
      return wh::core::detail::erase_resume_scheduler(
          wh::core::detail::select_launch_scheduler(env));
    } else {
      return missing_receiver_scheduler<receiver_t>();
    }
  }

  [[nodiscard]] static auto
  get_delegation_scheduler(const void *object) noexcept
      -> erased_resume_scheduler {
    const auto &receiver = *static_cast<const receiver_t *>(object);
    const auto &env = stdexec::get_env(receiver);
    if constexpr (requires { stdexec::get_delegation_scheduler(env); }) {
      return wh::core::detail::erase_resume_scheduler(
          stdexec::get_delegation_scheduler(env));
    } else {
      return get_scheduler(object);
    }
  }

  [[nodiscard]] static auto get_stop_token(const void *object) noexcept
      -> stdexec::inplace_stop_token {
    const auto &receiver = *static_cast<const receiver_t *>(object);
    const auto &env = stdexec::get_env(receiver);
    if constexpr (requires {
                    {
                      stdexec::get_stop_token(env)
                    } -> std::same_as<stdexec::inplace_stop_token>;
                  }) {
      return stdexec::get_stop_token(env);
    } else {
      return {};
    }
  }

  static inline constexpr receiver_env_vtable vtable{
      .get_scheduler = &receiver_env_model::get_scheduler,
      .get_delegation_scheduler = &receiver_env_model::get_delegation_scheduler,
      .get_value_scheduler =
          &receiver_env_model::template select_completion_scheduler<
              stdexec::set_value_t>,
      .get_error_scheduler =
          &receiver_env_model::template select_completion_scheduler<
              stdexec::set_error_t>,
      .get_stopped_scheduler =
          &receiver_env_model::template select_completion_scheduler<
              stdexec::set_stopped_t>,
      .get_stop_token = &receiver_env_model::get_stop_token,
  };
};

template <typename... value_ts> struct receiver_ref_vtable {
  void (*set_value)(void *, value_ts...) noexcept {nullptr};
  void (*set_error)(void *, std::exception_ptr) noexcept {nullptr};
  void (*set_stopped)(void *) noexcept {nullptr};
  receiver_env_ref (*get_env)(const void *) noexcept {nullptr};
};

template <typename receiver_t, typename... value_ts> struct receiver_ref_model {
  static auto set_value(void *object, value_ts... values) noexcept -> void {
    stdexec::set_value(std::move(*static_cast<receiver_t *>(object)),
                       std::move(values)...);
  }

  static auto set_error(void *object, std::exception_ptr error) noexcept
      -> void {
    stdexec::set_error(std::move(*static_cast<receiver_t *>(object)),
                       std::move(error));
  }

  static auto set_stopped(void *object) noexcept -> void {
    stdexec::set_stopped(std::move(*static_cast<receiver_t *>(object)));
  }

  [[nodiscard]] static auto get_env(const void *object) noexcept
      -> receiver_env_ref {
    return receiver_env_ref{object, &receiver_env_model<receiver_t>::vtable};
  }

  static inline constexpr receiver_ref_vtable<value_ts...> vtable{
      .set_value = &receiver_ref_model::set_value,
      .set_error = &receiver_ref_model::set_error,
      .set_stopped = &receiver_ref_model::set_stopped,
      .get_env = &receiver_ref_model::get_env,
  };
};

template <typename... value_ts> class basic_receiver_ref {
public:
  using receiver_concept = stdexec::receiver_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(value_ts...),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;

  basic_receiver_ref(const basic_receiver_ref &) noexcept = default;
  basic_receiver_ref(basic_receiver_ref &&) noexcept = default;
  auto operator=(const basic_receiver_ref &) noexcept
      -> basic_receiver_ref & = default;
  auto operator=(basic_receiver_ref &&) noexcept
      -> basic_receiver_ref & = default;

  template <typename receiver_t>
    requires(!std::same_as<std::remove_cvref_t<receiver_t>,
                           basic_receiver_ref> &&
             stdexec::receiver_of<std::remove_cvref_t<receiver_t>,
                                  completion_signatures> &&
             wh::core::detail::receiver_with_resume_scheduler<
                 std::remove_cvref_t<receiver_t>>)
  basic_receiver_ref(receiver_t &receiver) noexcept
      : object_(std::addressof(receiver)),
        vtable_(&receiver_ref_model<std::remove_cvref_t<receiver_t>,
                                    value_ts...>::vtable) {}

  auto set_value(value_ts... values) noexcept -> void {
    vtable_->set_value(object_, std::move(values)...);
  }

  auto set_error(std::exception_ptr error) noexcept -> void {
    vtable_->set_error(object_, std::move(error));
  }

  auto set_stopped() noexcept -> void { vtable_->set_stopped(object_); }

  [[nodiscard]] auto get_env() const noexcept -> receiver_env_ref {
    return vtable_->get_env(object_);
  }

private:
  void *object_{nullptr};
  const receiver_ref_vtable<value_ts...> *vtable_{nullptr};
};

template <typename value_t>
using receiver_ref = basic_receiver_ref<chunk_result_t<value_t>>;

template <typename value_t>
using write_receiver_ref = basic_receiver_ref<wh::core::result<void>>;

template <typename value_t, std::size_t storage_size>
struct any_stream_reader_vtable {
  using chunk_result_type = chunk_result_t<value_t>;
  using chunk_try_result_type = chunk_try_result_t<value_t>;
  using receiver_ref_t = receiver_ref<value_t>;
  using op_handle_t = stream_op_handle<receiver_ref_t>;

  void (*destroy)(void *) noexcept {nullptr};
  void (*move_construct)(void *, void *) noexcept {nullptr};
  chunk_result_type (*read)(void *){nullptr};
  chunk_try_result_type (*try_read)(void *){nullptr};
  wh::core::result<void> (*close)(void *){nullptr};
  bool (*is_closed)(const void *) noexcept {nullptr};
  bool (*is_source_closed)(const void *) noexcept {nullptr};
  void (*set_automatic_close)(void *,
                              const auto_close_options &) noexcept {nullptr};
  op_handle_t (*connect_read_async)(void *, receiver_ref_t){nullptr};
};

template <typename value_t, std::size_t storage_size>
struct any_stream_writer_vtable {
  using receiver_ref_t = write_receiver_ref<value_t>;
  using op_handle_t = stream_op_handle<receiver_ref_t>;

  void (*destroy)(void *) noexcept {nullptr};
  void (*move_construct)(void *, void *) noexcept {nullptr};
  wh::core::result<void> (*try_write_copy)(void *, const value_t &){nullptr};
  wh::core::result<void> (*try_write_move)(void *, value_t &&){nullptr};
  wh::core::result<void> (*close)(void *){nullptr};
  bool (*is_closed)(const void *) noexcept {nullptr};
  op_handle_t (*connect_write_async)(void *, value_t, receiver_ref_t){nullptr};
};

template <typename value_t, std::size_t storage_size, typename reader_t>
  requires async_stream_reader<reader_t>
struct any_stream_reader_model {
  using vtable_t = any_stream_reader_vtable<value_t, storage_size>;
  using receiver_ref_t = typename vtable_t::receiver_ref_t;
  using op_handle_t = typename vtable_t::op_handle_t;
  using storage_model_t = erased_storage_model_t<reader_t, storage_size>;

  static auto destroy(void *object) noexcept -> void {
    static_cast<storage_model_t *>(object)->~storage_model_t();
  }

  static auto move_construct(void *source, void *target) noexcept -> void {
    auto *typed_source = static_cast<storage_model_t *>(source);
    ::new (target) storage_model_t(std::move(*typed_source));
    typed_source->~storage_model_t();
  }

  static auto read(void *object) -> chunk_result_t<value_t> {
    return storage_object<reader_t, storage_size>(object)->read();
  }

  static auto try_read(void *object) -> chunk_try_result_t<value_t> {
    return storage_object<reader_t, storage_size>(object)->try_read();
  }

  static auto close(void *object) -> wh::core::result<void> {
    return storage_object<reader_t, storage_size>(object)->close();
  }

  static auto is_closed(const void *object) noexcept -> bool {
    return storage_object<reader_t, storage_size>(object)->is_closed();
  }

  static auto is_source_closed(const void *object) noexcept -> bool {
    const auto *reader = storage_object<reader_t, storage_size>(object);
    if constexpr (requires(const reader_t &candidate) {
                    { candidate.is_source_closed() } -> std::same_as<bool>;
                  }) {
      return reader->is_source_closed();
    }
    return reader->is_closed();
  }

  static auto set_automatic_close(void *object,
                                  const auto_close_options &options) noexcept
      -> void {
    auto *reader = storage_object<reader_t, storage_size>(object);
    if constexpr (requires(reader_t &candidate,
                           const auto_close_options &value) {
                    candidate.set_automatic_close(value);
                  }) {
      reader->set_automatic_close(options);
    } else {
      (void)object;
      (void)options;
    }
  }

  static auto connect_read_async(void *object, receiver_ref_t receiver)
      -> op_handle_t {
    auto *reader = storage_object<reader_t, storage_size>(object);
    auto sender = reader->read_async();
    using sender_t = std::remove_cvref_t<decltype(sender)>;
    using model_t = stream_op_model<sender_t, receiver_ref_t>;
    auto *model = new model_t(std::move(sender), std::move(receiver));
    op_handle_t handle{};
    handle.bind(model, &model_t::vtable);
    return handle;
  }

  static inline constexpr vtable_t vtable{
      .destroy = &any_stream_reader_model::destroy,
      .move_construct = &any_stream_reader_model::move_construct,
      .read = &any_stream_reader_model::read,
      .try_read = &any_stream_reader_model::try_read,
      .close = &any_stream_reader_model::close,
      .is_closed = &any_stream_reader_model::is_closed,
      .is_source_closed = &any_stream_reader_model::is_source_closed,
      .set_automatic_close = &any_stream_reader_model::set_automatic_close,
      .connect_read_async = &any_stream_reader_model::connect_read_async,
  };
};

template <typename value_t, std::size_t storage_size, typename writer_t>
struct any_stream_writer_model {
  using vtable_t = any_stream_writer_vtable<value_t, storage_size>;
  using receiver_ref_t = typename vtable_t::receiver_ref_t;
  using op_handle_t = typename vtable_t::op_handle_t;
  using storage_model_t = erased_storage_model_t<writer_t, storage_size>;

  static auto destroy(void *object) noexcept -> void {
    static_cast<storage_model_t *>(object)->~storage_model_t();
  }

  static auto move_construct(void *source, void *target) noexcept -> void {
    auto *typed_source = static_cast<storage_model_t *>(source);
    ::new (target) storage_model_t(std::move(*typed_source));
    typed_source->~storage_model_t();
  }

  static auto try_write_copy(void *object, const value_t &value)
      -> wh::core::result<void> {
    auto *typed = storage_object<writer_t, storage_size>(object);
    if constexpr (requires(writer_t &writer, const value_t &input) {
                    writer.try_write(input);
                  }) {
      return typed->try_write(value);
    } else if constexpr (std::copy_constructible<value_t>) {
      wh::core::result<value_t> copied{};
      try {
        copied = value_t{value};
      } catch (...) {
        return wh::core::result<void>::failure(
            wh::core::map_current_exception());
      }
      if (copied.has_error()) {
        return wh::core::result<void>::failure(copied.error());
      }
      return try_write_move(object, std::move(copied).value());
    } else {
      (void)value;
      return wh::core::result<void>::failure(wh::core::errc::not_supported);
    }
  }

  static auto try_write_move(void *object, value_t &&value)
      -> wh::core::result<void> {
    auto *typed = storage_object<writer_t, storage_size>(object);
    if constexpr (requires(writer_t &writer, value_t &&input) {
                    writer.try_write(std::move(input));
                  }) {
      return typed->try_write(std::move(value));
    } else {
      return typed->try_write(value);
    }
  }

  static auto close(void *object) -> wh::core::result<void> {
    return storage_object<writer_t, storage_size>(object)->close();
  }

  static auto is_closed(const void *object) noexcept -> bool {
    return storage_object<writer_t, storage_size>(object)->is_closed();
  }

  static auto connect_write_async(void *object, value_t value,
                                  receiver_ref_t receiver) -> op_handle_t {
    auto *typed = storage_object<writer_t, storage_size>(object);
    auto make_sender = [&]() {
      if constexpr (requires(const writer_t &writer, value_t &&input) {
                      writer.write_async(std::move(input));
                    }) {
        return typed->write_async(std::move(value));
      } else {
        return typed->write_async(value);
      }
    };
    using sender_t = std::remove_cvref_t<decltype(make_sender())>;
    using model_t = stream_op_model<sender_t, receiver_ref_t>;
    auto *model = new model_t(make_sender(), std::move(receiver));
    op_handle_t handle{};
    handle.bind(model, &model_t::vtable);
    return handle;
  }

  static inline constexpr vtable_t vtable{
      .destroy = &any_stream_writer_model::destroy,
      .move_construct = &any_stream_writer_model::move_construct,
      .try_write_copy = &any_stream_writer_model::try_write_copy,
      .try_write_move = &any_stream_writer_model::try_write_move,
      .close = &any_stream_writer_model::close,
      .is_closed = &any_stream_writer_model::is_closed,
      .connect_write_async = &any_stream_writer_model::connect_write_async,
  };
};

template <typename value_t>
[[nodiscard]] inline auto make_reader_not_found() -> chunk_result_t<value_t> {
  return chunk_result_t<value_t>::failure(wh::core::errc::not_found);
}

} // namespace detail

/// Type-erased stream reader handle used to unify pipe/copy/merge readers.
template <typename value_t, std::size_t storage_size>
class any_stream_reader final
    : public stream_base<any_stream_reader<value_t, storage_size>, value_t> {
public:
  using value_type = value_t;
  using chunk_type = stream_chunk<value_t>;
  using read_sender_type = any_stream_read_sender<value_t, storage_size>;

  /// Creates an empty reader handle.
  any_stream_reader() = default;

  /// Wraps one concrete async-capable reader backend.
  template <typename reader_t>
    requires(!std::same_as<std::remove_cvref_t<reader_t>, any_stream_reader>) &&
            detail::async_stream_reader<std::remove_cvref_t<reader_t>> &&
            std::same_as<typename std::remove_cvref_t<reader_t>::value_type,
                         value_t> &&
            std::constructible_from<std::remove_cvref_t<reader_t>, reader_t>
  any_stream_reader(reader_t &&reader) {
    emplace<std::remove_cvref_t<reader_t>>(std::forward<reader_t>(reader));
  }

  any_stream_reader(const any_stream_reader &) = delete;
  auto operator=(const any_stream_reader &) -> any_stream_reader & = delete;

  /// Moves the concrete backend without exposing its concrete type.
  any_stream_reader(any_stream_reader &&other) noexcept {
    move_from(std::move(other));
  }

  /// Replaces current backend with the moved backend from `other`.
  auto operator=(any_stream_reader &&other) noexcept -> any_stream_reader & {
    if (this == &other) {
      return *this;
    }
    reset();
    move_from(std::move(other));
    return *this;
  }

  /// Destroys the current backend if present.
  ~any_stream_reader() { reset(); }

  /// Reads the next owned chunk via the erased backend.
  [[nodiscard]] auto read_impl() -> detail::chunk_result_t<value_t> {
    if (!has_value()) {
      return detail::make_reader_not_found<value_t>();
    }
    return vtable_->read(object_ptr());
  }

  /// Tries to read the next owned chunk via the erased backend.
  [[nodiscard]] auto try_read_impl() -> detail::chunk_try_result_t<value_t> {
    if (!has_value()) {
      return detail::make_reader_not_found<value_t>();
    }
    return vtable_->try_read(object_ptr());
  }

  /// Starts one sender-based async read using the erased backend.
  [[nodiscard]] auto read_async() & -> read_sender_type {
    return read_sender_type{*this};
  }

  /// Starts one sender-based async read, transferring reader ownership into the
  /// sender.
  [[nodiscard]] auto read_async() && -> read_sender_type {
    return read_sender_type{std::move(*this)};
  }

  /// Closes the erased backend.
  auto close_impl() -> wh::core::result<void> {
    if (!has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return vtable_->close(object_ptr());
  }

  /// Reports whether the erased backend is closed.
  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return !has_value() || vtable_->is_closed(object_ptr());
  }

  /// Reports whether the upstream source side is closed.
  [[nodiscard]] auto is_source_closed() const noexcept -> bool {
    return !has_value() || vtable_->is_source_closed(object_ptr());
  }

  /// Forwards auto-close policy when the backend supports it.
  auto set_automatic_close(const auto_close_options &options) noexcept -> void {
    if (has_value()) {
      vtable_->set_automatic_close(object_ptr(), options);
    }
  }

  /// Returns the concrete backend pointer when `reader_t` is active.
  template <typename reader_t>
  [[nodiscard]] auto target_if() noexcept -> reader_t * {
    if (vtable_ == &detail::any_stream_reader_model<value_t, storage_size,
                                                    reader_t>::vtable) {
      return detail::storage_object<reader_t>(storage_);
    }
    return nullptr;
  }

  /// Returns the concrete backend pointer when `reader_t` is active.
  template <typename reader_t>
  [[nodiscard]] auto target_if() const noexcept -> const reader_t * {
    if (vtable_ == &detail::any_stream_reader_model<value_t, storage_size,
                                                    reader_t>::vtable) {
      return detail::storage_object<reader_t>(storage_);
    }
    return nullptr;
  }

private:
  template <typename value_u, std::size_t storage_u>
  friend class any_stream_read_sender;

  [[nodiscard]] auto has_value() const noexcept -> bool {
    return vtable_ != nullptr;
  }

  template <typename reader_t, typename... arg_ts>
  auto emplace(arg_ts &&...args) -> void {
    using model_t = std::remove_cvref_t<reader_t>;
    using storage_model_t =
        detail::erased_storage_model_t<model_t, storage_size>;
    if constexpr (detail::fits_erased_storage_v<model_t, storage_size>) {
      ::new (storage_.data.data())
          storage_model_t(std::forward<arg_ts>(args)...);
    } else {
      ::new (storage_.data.data()) storage_model_t(
          std::make_unique<model_t>(std::forward<arg_ts>(args)...));
    }
    vtable_ = &detail::any_stream_reader_model<value_t, storage_size,
                                               model_t>::vtable;
  }

  auto reset() noexcept -> void {
    if (has_value()) {
      vtable_->destroy(object_ptr());
      vtable_ = nullptr;
    }
  }

  auto move_from(any_stream_reader &&other) noexcept -> void {
    if (!other.has_value()) {
      return;
    }
    other.vtable_->move_construct(other.object_ptr(), storage_.data.data());
    vtable_ = std::exchange(other.vtable_, nullptr);
  }

  [[nodiscard]] auto object_ptr() noexcept -> void * {
    return storage_.data.data();
  }
  [[nodiscard]] auto object_ptr() const noexcept -> const void * {
    return storage_.data.data();
  }

  detail::erased_storage<storage_size> storage_{};
  const detail::any_stream_reader_vtable<value_t, storage_size> *vtable_{
      nullptr};
};

/// Stable async sender returned by `any_stream_reader::read_async()`.
template <typename value_t, std::size_t storage_size>
class any_stream_read_sender final {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<
      stdexec::set_value_t(detail::chunk_result_t<value_t>),
      stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

  explicit any_stream_read_sender(
      any_stream_reader<value_t, storage_size> &reader) noexcept
      : borrowed_(std::addressof(reader)) {}

  explicit any_stream_read_sender(
      any_stream_reader<value_t, storage_size> &&reader) noexcept
      : owned_(std::move(reader)) {}

  any_stream_read_sender(const any_stream_read_sender &) = delete;
  auto operator=(const any_stream_read_sender &)
      -> any_stream_read_sender & = delete;
  any_stream_read_sender(any_stream_read_sender &&) noexcept = default;
  auto operator=(any_stream_read_sender &&) noexcept
      -> any_stream_read_sender & = default;

  template <stdexec::receiver_of<completion_signatures> receiver_t>
  class operation {
    using receiver_ref_t = detail::receiver_ref<value_t>;
    using bridge_t = wh::core::detail::receiver_stop_bridge<receiver_t>;

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      bridge_t *bridge{nullptr};

      auto set_value(detail::chunk_result_t<value_t> status) && noexcept
          -> void {
        bridge->set_value(std::move(status));
      }

      auto set_error(std::exception_ptr error) && noexcept -> void {
        bridge->set_error(std::move(error));
      }

      auto set_stopped() && noexcept -> void { bridge->set_stopped(); }

      [[nodiscard]] auto get_env() const noexcept ->
          typename bridge_t::stop_env_t {
        return bridge->env();
      }
    };

  public:
    using operation_state_concept = stdexec::operation_state_t;

    operation(any_stream_reader<value_t, storage_size> *reader,
              receiver_t receiver) noexcept
        : borrowed_(reader), bridge_(std::move(receiver)),
          child_receiver_{std::addressof(bridge_)} {}

    operation(any_stream_reader<value_t, storage_size> &&reader,
              receiver_t receiver) noexcept
        : owned_(std::move(reader)), bridge_(std::move(receiver)),
          child_receiver_{std::addressof(bridge_)} {}

    auto start() & noexcept -> void {
      auto *reader = active_reader();
      if (reader == nullptr || !reader->has_value()) {
        bridge_.set_value(detail::make_reader_not_found<value_t>());
        return;
      }
      if (!bridge_.bind_outer_stop()) {
        bridge_.set_stopped();
        return;
      }
      try {
        auto child = reader->vtable_->connect_read_async(
            reader->object_ptr(), receiver_ref_t{child_receiver_});
        child_ = std::move(child);
        child_.start();
      } catch (...) {
        bridge_.set_error(std::current_exception());
      }
    }

  private:
    [[nodiscard]] auto active_reader() noexcept
        -> any_stream_reader<value_t, storage_size> * {
      if (owned_.has_value()) {
        return std::addressof(*owned_);
      }
      return borrowed_;
    }

    any_stream_reader<value_t, storage_size> *borrowed_{nullptr};
    std::optional<any_stream_reader<value_t, storage_size>> owned_{};
    bridge_t bridge_;
    child_receiver child_receiver_{};
    typename detail::any_stream_reader_vtable<
        value_t, storage_size>::op_handle_t child_{};
  };

  template <stdexec::receiver_of<completion_signatures> receiver_t>
    requires wh::core::detail::receiver_with_resume_scheduler<
        std::remove_cvref_t<receiver_t>>
  [[nodiscard]] auto connect(receiver_t receiver) && -> operation<receiver_t> {
    if (owned_.has_value()) {
      return operation<receiver_t>{std::move(*owned_), std::move(receiver)};
    }
    return operation<receiver_t>{borrowed_, std::move(receiver)};
  }

  [[nodiscard]] auto get_env() const noexcept
      -> wh::core::detail::async_completion_env {
    return {};
  }

private:
  any_stream_reader<value_t, storage_size> *borrowed_{nullptr};
  std::optional<any_stream_reader<value_t, storage_size>> owned_{};
};

/// Type-erased stream writer handle used to unify writer-facing stream APIs.
template <typename value_t, std::size_t storage_size>
class any_stream_writer final {
public:
  using value_type = value_t;
  using write_sender_type = any_stream_write_sender<value_t, storage_size>;

  /// Creates an empty writer handle.
  any_stream_writer() = default;

  /// Wraps one concrete writer backend.
  template <typename writer_t>
    requires(!std::same_as<std::remove_cvref_t<writer_t>, any_stream_writer>) &&
            std::constructible_from<std::remove_cvref_t<writer_t>, writer_t>
  any_stream_writer(writer_t &&writer) {
    emplace<std::remove_cvref_t<writer_t>>(std::forward<writer_t>(writer));
  }

  any_stream_writer(const any_stream_writer &) = delete;
  auto operator=(const any_stream_writer &) -> any_stream_writer & = delete;

  /// Moves the concrete backend without exposing its concrete type.
  any_stream_writer(any_stream_writer &&other) noexcept {
    move_from(std::move(other));
  }

  /// Replaces current backend with the moved backend from `other`.
  auto operator=(any_stream_writer &&other) noexcept -> any_stream_writer & {
    if (this == &other) {
      return *this;
    }
    reset();
    move_from(std::move(other));
    return *this;
  }

  /// Destroys the current backend if present.
  ~any_stream_writer() { reset(); }

  /// Tries to write one copied value chunk.
  auto try_write(const value_t &value) -> wh::core::result<void> {
    if (!has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return vtable_->try_write_copy(object_ptr(), value);
  }

  /// Tries to write one movable value chunk.
  auto try_write(value_t &&value) -> wh::core::result<void> {
    if (!has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return vtable_->try_write_move(object_ptr(), std::move(value));
  }

  /// Starts one sender-based async write.
  [[nodiscard]] auto write_async(const value_t &value) & -> write_sender_type {
    wh::core::result<value_t> prepared{};
    if constexpr (std::copy_constructible<value_t>) {
      try {
        prepared = value_t{value};
      } catch (...) {
        prepared = wh::core::result<value_t>::failure(
            wh::core::map_current_exception());
      }
    } else {
      (void)value;
      prepared =
          wh::core::result<value_t>::failure(wh::core::errc::not_supported);
    }
    return write_sender_type{*this, std::move(prepared)};
  }

  /// Starts one sender-based async write.
  [[nodiscard]] auto write_async(value_t &&value) & -> write_sender_type {
    wh::core::result<value_t> prepared{};
    try {
      prepared = std::move(value);
    } catch (...) {
      prepared =
          wh::core::result<value_t>::failure(wh::core::map_current_exception());
    }
    return write_sender_type{*this, std::move(prepared)};
  }

  /// Starts one sender-based async write, transferring writer ownership into
  /// the sender.
  [[nodiscard]] auto write_async(const value_t &value) && -> write_sender_type {
    wh::core::result<value_t> prepared{};
    if constexpr (std::copy_constructible<value_t>) {
      try {
        prepared = value_t{value};
      } catch (...) {
        prepared = wh::core::result<value_t>::failure(
            wh::core::map_current_exception());
      }
    } else {
      (void)value;
      prepared =
          wh::core::result<value_t>::failure(wh::core::errc::not_supported);
    }
    return write_sender_type{std::move(*this), std::move(prepared)};
  }

  /// Starts one sender-based async write, transferring writer ownership into
  /// the sender.
  [[nodiscard]] auto write_async(value_t &&value) && -> write_sender_type {
    wh::core::result<value_t> prepared{};
    try {
      prepared = std::move(value);
    } catch (...) {
      prepared =
          wh::core::result<value_t>::failure(wh::core::map_current_exception());
    }
    return write_sender_type{std::move(*this), std::move(prepared)};
  }

  /// Closes the erased backend.
  auto close() -> wh::core::result<void> {
    if (!has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return vtable_->close(object_ptr());
  }

  /// Reports whether the erased backend is closed.
  [[nodiscard]] auto is_closed() const noexcept -> bool {
    return !has_value() || vtable_->is_closed(object_ptr());
  }

private:
  template <typename value_u, std::size_t storage_u>
  friend class any_stream_write_sender;

  [[nodiscard]] auto has_value() const noexcept -> bool {
    return vtable_ != nullptr;
  }

  template <typename writer_t, typename... arg_ts>
  auto emplace(arg_ts &&...args) -> void {
    using model_t = std::remove_cvref_t<writer_t>;
    using storage_model_t =
        detail::erased_storage_model_t<model_t, storage_size>;
    if constexpr (detail::fits_erased_storage_v<model_t, storage_size>) {
      ::new (storage_.data.data())
          storage_model_t(std::forward<arg_ts>(args)...);
    } else {
      ::new (storage_.data.data()) storage_model_t(
          std::make_unique<model_t>(std::forward<arg_ts>(args)...));
    }
    vtable_ = &detail::any_stream_writer_model<value_t, storage_size,
                                               model_t>::vtable;
  }

  auto reset() noexcept -> void {
    if (has_value()) {
      vtable_->destroy(object_ptr());
      vtable_ = nullptr;
    }
  }

  auto move_from(any_stream_writer &&other) noexcept -> void {
    if (!other.has_value()) {
      return;
    }
    other.vtable_->move_construct(other.object_ptr(), storage_.data.data());
    vtable_ = std::exchange(other.vtable_, nullptr);
  }

  [[nodiscard]] auto object_ptr() noexcept -> void * {
    return storage_.data.data();
  }
  [[nodiscard]] auto object_ptr() const noexcept -> const void * {
    return storage_.data.data();
  }

  detail::erased_storage<storage_size> storage_{};
  const detail::any_stream_writer_vtable<value_t, storage_size> *vtable_{
      nullptr};
};

/// Stable async sender returned by `any_stream_writer::write_async()`.
template <typename value_t, std::size_t storage_size>
class any_stream_write_sender final {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<
      stdexec::set_value_t(wh::core::result<void>),
      stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

  any_stream_write_sender(any_stream_writer<value_t, storage_size> &&writer,
                          wh::core::result<value_t> prepared)
      : owned_(std::move(writer)), prepared_(std::move(prepared)) {}

  any_stream_write_sender(any_stream_writer<value_t, storage_size> &writer,
                          wh::core::result<value_t> prepared) noexcept
      : borrowed_(std::addressof(writer)), prepared_(std::move(prepared)) {}

  any_stream_write_sender(const any_stream_write_sender &) = delete;
  auto operator=(const any_stream_write_sender &)
      -> any_stream_write_sender & = delete;
  any_stream_write_sender(any_stream_write_sender &&) noexcept = default;
  auto operator=(any_stream_write_sender &&) noexcept
      -> any_stream_write_sender & = default;

  template <stdexec::receiver_of<completion_signatures> receiver_t>
  class operation {
    using receiver_ref_t = detail::write_receiver_ref<value_t>;
    using bridge_t = wh::core::detail::receiver_stop_bridge<receiver_t>;

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      bridge_t *bridge{nullptr};

      auto set_value(wh::core::result<void> status) && noexcept -> void {
        bridge->set_value(std::move(status));
      }

      auto set_error(std::exception_ptr error) && noexcept -> void {
        bridge->set_error(std::move(error));
      }

      auto set_stopped() && noexcept -> void { bridge->set_stopped(); }

      [[nodiscard]] auto get_env() const noexcept ->
          typename bridge_t::stop_env_t {
        return bridge->env();
      }
    };

  public:
    using operation_state_concept = stdexec::operation_state_t;

    operation(any_stream_writer<value_t, storage_size> *writer,
              wh::core::result<value_t> prepared, receiver_t receiver) noexcept
        : borrowed_(writer), prepared_(std::move(prepared)),
          bridge_(std::move(receiver)),
          child_receiver_{std::addressof(bridge_)} {}

    operation(any_stream_writer<value_t, storage_size> &&writer,
              wh::core::result<value_t> prepared, receiver_t receiver)
        : owned_(std::move(writer)), prepared_(std::move(prepared)),
          bridge_(std::move(receiver)),
          child_receiver_{std::addressof(bridge_)} {}

    auto start() & noexcept -> void {
      auto *writer = active_writer();
      if (writer == nullptr || !writer->has_value()) {
        bridge_.set_value(
            wh::core::result<void>::failure(wh::core::errc::not_found));
        return;
      }
      if (prepared_.has_error()) {
        bridge_.set_value(wh::core::result<void>::failure(prepared_.error()));
        return;
      }
      if (!bridge_.bind_outer_stop()) {
        bridge_.set_stopped();
        return;
      }
      try {
        auto child = writer->vtable_->connect_write_async(
            writer->object_ptr(), std::move(prepared_).value(),
            receiver_ref_t{child_receiver_});
        child_ = std::move(child);
        child_.start();
      } catch (...) {
        bridge_.set_error(std::current_exception());
      }
    }

  private:
    [[nodiscard]] auto active_writer() noexcept
        -> any_stream_writer<value_t, storage_size> * {
      if (owned_.has_value()) {
        return std::addressof(*owned_);
      }
      return borrowed_;
    }

    any_stream_writer<value_t, storage_size> *borrowed_{nullptr};
    std::optional<any_stream_writer<value_t, storage_size>> owned_{};
    wh::core::result<value_t> prepared_{};
    bridge_t bridge_;
    child_receiver child_receiver_{};
    typename detail::any_stream_writer_vtable<
        value_t, storage_size>::op_handle_t child_{};
  };

  template <stdexec::receiver_of<completion_signatures> receiver_t>
    requires wh::core::detail::receiver_with_resume_scheduler<
        std::remove_cvref_t<receiver_t>>
  [[nodiscard]] auto connect(receiver_t receiver) && -> operation<receiver_t> {
    if (owned_.has_value()) {
      return operation<receiver_t>{std::move(*owned_), std::move(prepared_),
                                   std::move(receiver)};
    }
    return operation<receiver_t>{borrowed_, std::move(prepared_),
                                 std::move(receiver)};
  }

  [[nodiscard]] auto get_env() const noexcept
      -> wh::core::detail::async_completion_env {
    return {};
  }

private:
  any_stream_writer<value_t, storage_size> *borrowed_{nullptr};
  std::optional<any_stream_writer<value_t, storage_size>> owned_{};
  wh::core::result<value_t> prepared_{};
};

} // namespace wh::schema::stream
