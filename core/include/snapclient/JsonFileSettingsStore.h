#pragma once

#include <string>
#include <unordered_map>

#include "snapclient/SettingsStore.h"

namespace snapclient {

// SettingsStore backed by a single JSON file: loaded fully at construction,
// rewritten in full on every set* call.
class JsonFileSettingsStore : public SettingsStore {
 public:
  explicit JsonFileSettingsStore(std::string path);

  std::optional<std::string> getString(const std::string& key) const override;
  void setString(const std::string& key, const std::string& value) override;

  std::optional<int32_t> getInt(const std::string& key) const override;
  void setInt(const std::string& key, int32_t value) override;

  std::optional<float> getFloat(const std::string& key) const override;
  void setFloat(const std::string& key, float value) override;

 private:
  std::string path_;
  std::unordered_map<std::string, std::string> values_;

  void load();
  void save() const;
};

}  // namespace snapclient
