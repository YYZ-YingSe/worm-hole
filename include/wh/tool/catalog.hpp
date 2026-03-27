// Defines tool catalog provider and cache types used to load tool schemas.
#pragma once

#include <memory>
#include <vector>

#include "wh/core/result.hpp"
#include "wh/schema/tool.hpp"

namespace wh::tool {

struct tool_catalog_load_options {
  bool refresh{false};
};

class tool_catalog_provider {
public:
  virtual ~tool_catalog_provider() = default;
  [[nodiscard]] virtual auto handshake() const -> wh::core::result<void> = 0;
  [[nodiscard]] virtual auto fetch_catalog() const
      -> wh::core::result<std::vector<wh::schema::tool_schema_definition>> = 0;
};

class tool_catalog_cache {
public:
  tool_catalog_cache() = default;
  explicit tool_catalog_cache(
      const std::shared_ptr<tool_catalog_provider> &provider)
      : provider_(provider) {}
  explicit tool_catalog_cache(std::shared_ptr<tool_catalog_provider> &&provider)
      : provider_(std::move(provider)) {}

  auto set_provider(const std::shared_ptr<tool_catalog_provider> &provider)
      -> void {
    provider_ = provider;
    cached_.clear();
    cache_valid_ = false;
  }

  auto set_provider(std::shared_ptr<tool_catalog_provider> &&provider) -> void {
    provider_ = std::move(provider);
    cached_.clear();
    cache_valid_ = false;
  }

  [[nodiscard]] auto load(
      const tool_catalog_load_options &options = tool_catalog_load_options{})
      -> wh::core::result<const std::vector<wh::schema::tool_schema_definition> *> {
    if (!provider_) {
      return wh::core::result<
          const std::vector<wh::schema::tool_schema_definition> *>::failure(
          wh::core::errc::not_found);
    }
    if (options.refresh) {
      cache_valid_ = false;
    }
    if (!cache_valid_) {
      auto handshake_result = provider_->handshake();
      if (handshake_result.has_error()) {
        return wh::core::result<
            const std::vector<wh::schema::tool_schema_definition> *>::failure(
            handshake_result.error());
      }
      auto fetched = provider_->fetch_catalog();
      if (fetched.has_error()) {
        return wh::core::result<
            const std::vector<wh::schema::tool_schema_definition> *>::failure(
            fetched.error());
      }
      cached_ = std::move(fetched).value();
      cache_valid_ = true;
    }
    return &cached_;
  }

  auto invalidate() noexcept -> void { cache_valid_ = false; }

private:
  std::shared_ptr<tool_catalog_provider> provider_{};
  std::vector<wh::schema::tool_schema_definition> cached_{};
  bool cache_valid_{false};
};

} // namespace wh::tool
