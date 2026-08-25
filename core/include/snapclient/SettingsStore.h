#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace snapclient {

// Flat key-value persistence, no JSON/schema knowledge. Implementations
// decide their own storage and key-length constraints.
class SettingsStore {
 public:
  virtual ~SettingsStore() = default;

  virtual std::optional<std::string> getString(const std::string& key) const = 0;
  virtual void setString(const std::string& key, const std::string& value) = 0;

  virtual std::optional<int32_t> getInt(const std::string& key) const = 0;
  virtual void setInt(const std::string& key, int32_t value) = 0;

  virtual std::optional<float> getFloat(const std::string& key) const = 0;
  virtual void setFloat(const std::string& key, float value) = 0;
};

}  // namespace snapclient
