// Defines tool catalog source and cache types used to load tool schemas.
#pragma once

#include <optional>
#include <span>
#include <vector>

#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/tool.hpp"

namespace wh::tool {

/// Callable source used to load one tool catalog snapshot.
struct tool_catalog_source {
  /// Optional readiness probe executed before fetching the catalog.
  wh::core::callback_function<wh::core::result<void>() const> handshake{
      nullptr};
  /// Required catalog fetcher returning the latest schema list.
  wh::core::callback_function<
      wh::core::result<std::vector<wh::schema::tool_schema_definition>>() const>
      fetch_catalog{nullptr};
};

/// Per-load options for one tool catalog cache read.
struct tool_catalog_load_options {
  /// True forces one fresh fetch instead of reusing the cached snapshot.
  bool refresh{false};
};

/// Cache for one tool catalog source with explicit refresh control.
class tool_catalog_cache {
public:
  tool_catalog_cache() = default;
  explicit tool_catalog_cache(const tool_catalog_source &source)
      : source_(source) {}
  explicit tool_catalog_cache(tool_catalog_source &&source) noexcept
      : source_(std::move(source)) {}

  /// Replaces the catalog source and invalidates the current cache snapshot.
  auto set_source(const tool_catalog_source &source) -> void {
    source_ = source;
    cached_.clear();
    cache_valid_ = false;
  }

  /// Replaces the catalog source and invalidates the current cache snapshot.
  auto set_source(tool_catalog_source &&source) noexcept -> void {
    source_ = std::move(source);
    cached_.clear();
    cache_valid_ = false;
  }

  /// Loads the current tool catalog snapshot, refreshing when requested.
  [[nodiscard]] auto load(const tool_catalog_load_options &options =
                              tool_catalog_load_options{}) const
      -> wh::core::result<std::span<const wh::schema::tool_schema_definition>> {
    if (!static_cast<bool>(source_.fetch_catalog)) {
      return wh::core::
          result<std::span<const wh::schema::tool_schema_definition>>::failure(
              wh::core::errc::not_found);
    }
    if (options.refresh) {
      cache_valid_ = false;
    }
    if (!cache_valid_) {
      if (static_cast<bool>(source_.handshake)) {
        auto handshake_result = source_.handshake();
        if (handshake_result.has_error()) {
          return wh::core::result<
              std::span<const wh::schema::tool_schema_definition>>::
              failure(handshake_result.error());
        }
      }

      auto fetched = source_.fetch_catalog();
      if (fetched.has_error()) {
        return wh::core::result<std::span<
            const wh::schema::tool_schema_definition>>::failure(fetched
                                                                    .error());
      }
      cached_ = std::move(fetched).value();
      cache_valid_ = true;
    }
    return std::span<const wh::schema::tool_schema_definition>{cached_.data(),
                                                               cached_.size()};
  }

  /// Invalidates the cached snapshot without changing the source.
  auto invalidate() noexcept -> void { cache_valid_ = false; }

private:
  /// Source used to fetch catalog snapshots.
  tool_catalog_source source_{};
  /// Cached catalog snapshot retained between loads.
  mutable std::vector<wh::schema::tool_schema_definition> cached_{};
  /// True when `cached_` contains a fresh snapshot for the current source.
  mutable bool cache_valid_{false};
};

} // namespace wh::tool
