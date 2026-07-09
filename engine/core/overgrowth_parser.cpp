#include "overgrowth_parser.hpp"

#include <sstream>

namespace bf2 {

void OvergrowthParser::parse(const std::string& overgrowth_con_script) {
  materials_.clear();
  types_.clear();
  active_material_.clear();
  active_type_.clear();
  view_distance_ = 500.f;

  std::istringstream in(overgrowth_con_script);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    ls >> cmd;
    if (cmd == "Overgrowth.viewDistance") {
      ls >> view_distance_;
    } else if (cmd == "Overgrowth.addMaterial") {
      OvergrowthMaterialDef mat;
      ls >> mat.name >> mat.id;
      materials_.push_back(std::move(mat));
    } else if (cmd == "Overgrowth.setActiveMaterial") {
      ls >> active_material_;
    } else if (cmd == "Overgrowth.addType") {
      std::string type_name;
      ls >> type_name;
      for (auto& mat : materials_) {
        if (mat.name == active_material_) mat.type_names.push_back(type_name);
      }
      types_[type_name] = OvergrowthTypeDef{type_name, {}, 0.f, 0.f, 0.f, 0.f, 0.f};
      active_type_ = type_name;
    } else if (cmd == "Overgrowth.setActiveType") {
      ls >> active_type_;
    } else if (cmd == "OvergrowthType.geometry" && !active_type_.empty()) {
      std::string geom;
      ls >> geom;
      types_[active_type_].geometry = geom;
    } else if (cmd == "OvergrowthType.density" && !active_type_.empty()) {
      ls >> types_[active_type_].density;
    } else if (cmd == "OvergrowthType.normalScale" && !active_type_.empty()) {
      ls >> types_[active_type_].normal_scale;
    } else if (cmd == "OvergrowthType.rotationScale" && !active_type_.empty()) {
      ls >> types_[active_type_].rotation_scale;
    } else if (cmd == "OvergrowthType.minRadiusToSame" && !active_type_.empty()) {
      ls >> types_[active_type_].min_radius_same;
    } else if (cmd == "OvergrowthType.minRadiusToOthers" && !active_type_.empty()) {
      ls >> types_[active_type_].min_radius_others;
    }
  }
}

bool overgrowth_parser_self_test() {
  static const char* kSample = R"(
Overgrowth.viewDistance 500
Overgrowth.addMaterial bjorkcluster 3
Overgrowth.setActiveMaterial bjorkcluster
Overgrowth.addType bjork1
Overgrowth.setActiveType bjork1
OvergrowthType.geometry nc_birch_cluster01
OvergrowthType.density 5
)";

  OvergrowthParser parser;
  parser.parse(kSample);
  if (parser.materials().size() != 1) return false;
  if (parser.types().size() != 1) return false;
  const auto it = parser.types().find("bjork1");
  if (it == parser.types().end()) return false;
  if (it->second.geometry != "nc_birch_cluster01") return false;
  if (it->second.density != 5.f) return false;
  return true;
}

}  // namespace bf2
