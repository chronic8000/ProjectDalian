// Audit static/overgrowth placement height vs terrain (hovering plants, sunk props).
// Usage: placement_audit <bf2_root> [--mod MOD --level LEVEL] [--verbose] [--json out.json]
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "engine/core/level_validator.hpp"
#include "engine/core/placement_audit.hpp"

int main(int argc, char** argv) {
  std::string bf2_root = R"(C:\Program Files (x86)\Battlefield2)";
  std::string filter_mod;
  std::string filter_level;
  std::string json_out;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--mod" && i + 1 < argc)
      filter_mod = argv[++i];
    else if (a == "--level" && i + 1 < argc)
      filter_level = argv[++i];
    else if (a == "--json" && i + 1 < argc)
      json_out = argv[++i];
    else if (a == "--verbose")
      verbose = true;
    else if (a[0] != '-')
      bf2_root = a;
  }

  auto levels = bf2::discover_levels(bf2_root);
  if (!filter_mod.empty() || !filter_level.empty()) {
    std::vector<bf2::ModLevelEntry> filtered;
    for (const auto& e : levels) {
      if (!filter_mod.empty() && e.mod != filter_mod) continue;
      if (!filter_level.empty() && e.level != filter_level) continue;
      filtered.push_back(e);
    }
    levels = std::move(filtered);
  }

  std::cout << "Placement height audit: " << levels.size() << " levels\n";
  std::ostringstream js;
  js << "[\n";
  bool first = true;
  std::size_t total_float = 0, total_embed = 0;

  for (const auto& e : levels) {
    const auto r = bf2::audit_placement_heights(bf2_root, e.mod, e.level);
    total_float += r.float_count;
    total_embed += r.embed_count;
    bf2::log_placement_audit(r, verbose);
    if (!json_out.empty()) {
      if (!first) js << ",\n";
      first = false;
      js << "  {\"mod\":\"" << e.mod << "\",\"level\":\"" << e.level << "\",\"total\":"
         << r.total << ",\"float\":" << r.float_count << ",\"embed\":" << r.embed_count << "}";
    }
  }
  js << "\n]\n";

  std::cout << "\nSummary: float=" << total_float << " embed=" << total_embed << '\n';
  if (!json_out.empty()) {
    std::ofstream out(json_out);
    out << js.str();
    std::cout << "Wrote " << json_out << '\n';
  }
  return 0;
}
