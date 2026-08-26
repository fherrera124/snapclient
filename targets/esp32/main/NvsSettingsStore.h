#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nvs.h>

#include "snapclient/SettingsStore.h"

namespace snapclient {

// NVS keys are capped at 15 usable characters; this codebase's dotted
// setting keys can run longer, so each key is stored under a short
// deterministic hash.
class NvsSettingsStore : public SettingsStore {
 public:
  NvsSettingsStore();
  ~NvsSettingsStore() override;

  std::optional<std::string> getString(const std::string& key) const override;
  void setString(const std::string& key, const std::string& value) override;

  std::optional<int32_t> getInt(const std::string& key) const override;
  void setInt(const std::string& key, int32_t value) override;

  std::optional<float> getFloat(const std::string& key) const override;
  void setFloat(const std::string& key, float value) override;

 private:
  nvs_handle_t handle_{};
  bool valid_ = false;
};

}  // namespace snapclient
