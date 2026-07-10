#include "compiled_roads_parser.hpp"

#include <cctype>
#include <sstream>

namespace bf2 {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string normalize_mesh_path(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  return lower(path);
}

bool parse_triple(const std::string& triple, float out[3]) {
  const auto s1 = triple.find('/');
  const auto s2 = triple.find('/', s1 + 1);
  if (s1 == std::string::npos || s2 == std::string::npos) return false;
  try {
    out[0] = std::stof(triple.substr(0, s1));
    out[1] = std::stof(triple.substr(s1 + 1, s2 - s1 - 1));
    out[2] = std::stof(triple.substr(s2 + 1));
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

std::vector<CompiledRoadPlacement> parse_compiled_roads(const std::string& script) {
  std::vector<CompiledRoadPlacement> out;
  std::istringstream in(script);
  std::string line;
  CompiledRoadPlacement cur;
  bool have = false;

  auto flush = [&]() {
    if (have && !cur.mesh_vpath.empty()) out.push_back(cur);
    have = false;
    cur = {};
  };

  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string cmd;
    if (!(ls >> cmd)) continue;
    const std::string lc = lower(cmd);

    if (lc == "object.create") {
      flush();
      have = true;
      std::string name;
      if (ls >> name) cur.template_name = name;
    } else if (have && lc == "object.geometry.loadmesh") {
      std::string path;
      ls >> path;
      cur.mesh_vpath = normalize_mesh_path(path);
    } else if (have && lc == "object.absoluteposition") {
      std::string triple;
      ls >> triple;
      parse_triple(triple, cur.position);
    }
  }
  flush();
  return out;
}

bool compiled_roads_parser_self_test() {
  static const char* kSample = R"(
object.create highway
object.geometry.loadMesh Levels\Test/Roads/main_compiled.mesh
object.absoluteposition 10/20/30
object.create dirt
object.geometry.loadMesh Levels\Test/Roads/side_compiled.mesh
object.absoluteposition -5/15/2
)";
  const auto roads = parse_compiled_roads(kSample);
  if (roads.size() != 2) return false;
  if (roads[0].template_name != "highway") return false;
  if (roads[0].mesh_vpath != "levels/test/roads/main_compiled.mesh") return false;
  if (roads[0].position[1] != 20.f) return false;
  if (roads[1].position[0] != -5.f) return false;
  return true;
}

}  // namespace bf2
