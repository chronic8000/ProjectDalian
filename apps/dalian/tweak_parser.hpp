#pragma once

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>

namespace dalian {

// Queryable key/value store for Battlefield 2 .tweak and .con scripts.
// Keys are stored exactly as written (e.g. "ObjectTemplate.fire.deviation").
class TweakData {
public:
  void clear();
  void parse(const std::string& text);
  bool has(const std::string& key) const;
  std::string get_string(const std::string& key, const std::string& default_val = {}) const;
  float get_float(const std::string& key, float default_val = 0.f) const;
  int get_int(const std::string& key, int default_val = 0) const;
  glm::vec3 get_vec3(const std::string& key, const glm::vec3& default_val = {}) const;
  const std::unordered_map<std::string, std::string>& entries() const { return entries_; }

private:
  std::unordered_map<std::string, std::string> entries_;
};

// Returns true when all built-in parser checks pass.
bool tweak_parser_self_test();

}  // namespace dalian
