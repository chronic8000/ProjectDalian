#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {

struct OvergrowthTypeDef {
  std::string name;
  std::string geometry;
  float density = 0.f;
  float normal_scale = 0.f;
  float rotation_scale = 0.f;
  float min_radius_same = 0.f;
  float min_radius_others = 0.f;
};

struct OvergrowthMaterialDef {
  std::string name;
  int id = 0;
  std::vector<std::string> type_names;
};

// Parses Overgrowth/Overgrowth.con (tree/bush material definitions).
// Instance positions live in the binary Overgrowth.raw (not parsed here yet).
class OvergrowthParser {
public:
  void parse(const std::string& overgrowth_con_script);
  const std::vector<OvergrowthMaterialDef>& materials() const { return materials_; }
  const std::unordered_map<std::string, OvergrowthTypeDef>& types() const { return types_; }
  float view_distance() const { return view_distance_; }

private:
  std::vector<OvergrowthMaterialDef> materials_;
  std::unordered_map<std::string, OvergrowthTypeDef> types_;
  std::string active_material_;
  std::string active_type_;
  float view_distance_ = 500.f;
};

bool overgrowth_parser_self_test();

}  // namespace bf2
