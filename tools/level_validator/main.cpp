// Headless validator: scan every BF2 level for load + asset resolution gaps.
// Usage: level_validator <bf2_root> [--json out.json] [--fail-on-miss] [--verbose]
//        [--mod MOD --level LEVEL]  (validate one level, dump unresolved/texture gaps)
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "engine/core/asset_audit.hpp"
#include "engine/core/level_validator.hpp"

int main(int argc, char** argv) {
  std::string bf2_root = R"(C:\Program Files (x86)\Battlefield2)";
  std::string json_out;
  std::string filter_mod;
  std::string filter_level;
  bool fail_on_miss = false;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--json" && i + 1 < argc) {
      json_out = argv[++i];
    } else if (a == "--fail-on-miss") {
      fail_on_miss = true;
    } else if (a == "--verbose") {
      verbose = true;
    } else if (a == "--mod" && i + 1 < argc) {
      filter_mod = argv[++i];
    } else if (a == "--level" && i + 1 < argc) {
      filter_level = argv[++i];
    } else if (a[0] != '-') {
      bf2_root = a;
    }
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

  std::cout << "Validating " << levels.size() << " levels under " << bf2_root << '\n';

  int ok = 0, tex_miss = 0, unresolved = 0, failed = 0;
  std::ostringstream js;
  js << "[\n";
  bool first = true;
  for (const auto& e : levels) {
    const auto r = bf2::validate_level(bf2_root, e.mod, e.level);
    if (!r.load_ok) ++failed;
    else ++ok;
    tex_miss += static_cast<int>(r.audit.texture_misses);
    unresolved += static_cast<int>(r.audit.unresolved_templates);

    std::cout << (r.load_ok ? "OK " : "FAIL ") << e.mod << '/' << e.level << " placements="
              << r.placements << " tex_miss=" << r.audit.texture_misses
              << " unresolved=" << r.audit.unresolved_templates << " nav=" << r.has_nav
              << " foliage=" << (r.has_overgrowth || r.has_undergrowth) << '\n';
    if (!r.error.empty() && !r.load_ok) std::cout << "  error: " << r.error << '\n';
    if (verbose && r.load_ok) bf2::log_asset_audit(r.audit, true);

    if (!json_out.empty()) {
      if (!first) js << ",\n";
      first = false;
      js << "  {\"mod\":\"" << e.mod << "\",\"level\":\"" << e.level << "\",\"ok\":"
         << (r.load_ok ? "true" : "false") << ",\"placements\":" << r.placements
         << ",\"texture_misses\":" << r.audit.texture_misses << ",\"unresolved\":"
         << r.audit.unresolved_templates << ",\"has_nav\":" << (r.has_nav ? "true" : "false")
         << ",\"has_sky\":" << (r.has_sky ? "true" : "false") << "}";
    }
  }
  js << "\n]\n";

  std::cout << "\nSummary: ok=" << ok << " failed=" << failed << " texture_misses=" << tex_miss
            << " unresolved_templates=" << unresolved << '\n';

  if (!json_out.empty()) {
    std::ofstream out(json_out);
    out << js.str();
    std::cout << "Wrote " << json_out << '\n';
  }

  if (fail_on_miss && (tex_miss > 0 || unresolved > 0 || failed > 0)) return 1;
  return failed > 0 ? 2 : 0;
}
