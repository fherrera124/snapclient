#include "snapclient/JsonFileSettingsStore.h"

#include <fstream>
#include <sstream>

#include <tao/json.hpp>

namespace snapclient {

JsonFileSettingsStore::JsonFileSettingsStore(std::string path)
    : path_(std::move(path)) {
  load();
}

void JsonFileSettingsStore::load() {
  std::ifstream in(path_);
  if (!in) {
    return;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string content = buffer.str();
  if (content.empty()) {
    return;
  }

  tao::json::value obj;
  try {
    obj = tao::json::from_string(content);
  } catch (const std::exception&) {
    return;
  }
  if (!obj.is_object()) {
    return;
  }
  for (const auto& [key, value] : obj.get_object()) {
    if (value.is_string()) {
      values_[key] = value.get_string();
    }
  }
}

void JsonFileSettingsStore::save() const {
  tao::json::value obj;
  for (const auto& [key, value] : values_) {
    obj[key] = value;
  }
  std::ofstream out(path_, std::ios::trunc);
  out << tao::json::to_string(obj);
}

std::optional<std::string> JsonFileSettingsStore::getString(
    const std::string& key) const {
  auto it = values_.find(key);
  if (it == values_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void JsonFileSettingsStore::setString(const std::string& key,
                                      const std::string& value) {
  values_[key] = value;
  save();
}

std::optional<int32_t> JsonFileSettingsStore::getInt(
    const std::string& key) const {
  auto s = getString(key);
  if (!s) {
    return std::nullopt;
  }
  try {
    return static_cast<int32_t>(std::stol(*s));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void JsonFileSettingsStore::setInt(const std::string& key, int32_t value) {
  setString(key, std::to_string(value));
}

std::optional<float> JsonFileSettingsStore::getFloat(
    const std::string& key) const {
  auto s = getString(key);
  if (!s) {
    return std::nullopt;
  }
  try {
    return std::stof(*s);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void JsonFileSettingsStore::setFloat(const std::string& key, float value) {
  setString(key, std::to_string(value));
}

}  // namespace snapclient
