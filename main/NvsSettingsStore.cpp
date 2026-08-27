#include "NvsSettingsStore.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace snapclient {

namespace {

// FNV-1a 32-bit, hand-rolled and fixed on purpose: std::hash's algorithm
// isn't guaranteed stable across STL versions, and this has to keep
// resolving to the same NVS entries across future firmware rebuilds with
// a possibly different toolchain.
std::string hashKey(const std::string& key) {
  uint32_t h = 2166136261u;
  for (unsigned char c : key) {
    h ^= c;
    h *= 16777619u;
  }
  char buf[10];
  std::snprintf(buf, sizeof(buf), "k%08" PRIx32, h);
  return buf;
}

}  // namespace

NvsSettingsStore::NvsSettingsStore() {
  valid_ = nvs_open("snapclient", NVS_READWRITE, &handle_) == ESP_OK;
}

NvsSettingsStore::~NvsSettingsStore() {
  if (valid_) {
    nvs_close(handle_);
  }
}

std::optional<std::string> NvsSettingsStore::getString(
    const std::string& key) const {
  if (!valid_) {
    return std::nullopt;
  }
  std::string nvsKey = hashKey(key);
  size_t len = 0;
  if (nvs_get_str(handle_, nvsKey.c_str(), nullptr, &len) != ESP_OK) {
    return std::nullopt;
  }
  std::string value(len, '\0');
  if (nvs_get_str(handle_, nvsKey.c_str(), value.data(), &len) != ESP_OK) {
    return std::nullopt;
  }
  value.pop_back();  // drop the null terminator nvs_get_str includes in len
  return value;
}

void NvsSettingsStore::setString(const std::string& key,
                                 const std::string& value) {
  if (!valid_) {
    return;
  }
  nvs_set_str(handle_, hashKey(key).c_str(), value.c_str());
  nvs_commit(handle_);
}

std::optional<int32_t> NvsSettingsStore::getInt(const std::string& key) const {
  if (!valid_) {
    return std::nullopt;
  }
  int32_t value = 0;
  if (nvs_get_i32(handle_, hashKey(key).c_str(), &value) != ESP_OK) {
    return std::nullopt;
  }
  return value;
}

void NvsSettingsStore::setInt(const std::string& key, int32_t value) {
  if (!valid_) {
    return;
  }
  nvs_set_i32(handle_, hashKey(key).c_str(), value);
  nvs_commit(handle_);
}

std::optional<float> NvsSettingsStore::getFloat(const std::string& key) const {
  if (!valid_) {
    return std::nullopt;
  }
  uint32_t bits = 0;
  if (nvs_get_u32(handle_, hashKey(key).c_str(), &bits) != ESP_OK) {
    return std::nullopt;
  }
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void NvsSettingsStore::setFloat(const std::string& key, float value) {
  if (!valid_) {
    return;
  }
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  nvs_set_u32(handle_, hashKey(key).c_str(), bits);
  nvs_commit(handle_);
}

}  // namespace snapclient
