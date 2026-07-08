#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf2 {

class ArchiveMount {
public:
  bool mount(const std::string& archive_path, const std::string& namespace_name = "Objects");
  bool exists(const std::string& virtual_path) const;
  std::optional<std::vector<std::uint8_t>> read(const std::string& virtual_path) const;
  std::vector<std::string> list(const std::string& prefix = {}) const;
  void clear();

private:
  struct ArchiveEntry {
    std::string archive_path;
    std::string namespace_name;
    std::unordered_map<std::string, std::size_t> index;
  };

  std::vector<ArchiveEntry> archives_;
  static std::string normalize_path(std::string path);
};

}  // namespace bf2
