#include "archive.hpp"

#include <algorithm>
#include <cctype>

#include "miniz.h"

namespace bf2 {

std::string ArchiveMount::normalize_path(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  std::string lower;
  lower.reserve(path.size());
  for (char c : path) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lower;
}

bool ArchiveMount::mount(const std::string& archive_path, const std::string& namespace_name) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, archive_path.c_str(), 0)) {
    return false;
  }

  ArchiveEntry entry;
  entry.archive_path = archive_path;
  entry.namespace_name = namespace_name;

  const mz_uint file_count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < file_count; ++i) {
    if (mz_zip_reader_is_file_a_directory(&zip, i)) {
      continue;
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
      continue;
    }
    entry.index[normalize_path(stat.m_filename)] = static_cast<std::size_t>(i);
  }

  mz_zip_reader_end(&zip);
  archives_.push_back(std::move(entry));
  return true;
}

bool ArchiveMount::exists(const std::string& virtual_path) const {
  const auto key = normalize_path(virtual_path);
  for (auto it = archives_.rbegin(); it != archives_.rend(); ++it) {
    if (it->index.contains(key)) {
      return true;
    }
  }
  return false;
}

std::optional<std::vector<std::uint8_t>> ArchiveMount::read(const std::string& virtual_path) const {
  const auto key = normalize_path(virtual_path);
  for (auto it = archives_.rbegin(); it != archives_.rend(); ++it) {
    const auto found = it->index.find(key);
    if (found == it->index.end()) {
      continue;
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, it->archive_path.c_str(), 0)) {
      continue;
    }

    std::size_t out_size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(found->second), &out_size, 0);
    if (data == nullptr) {
      mz_zip_reader_end(&zip);
      continue;
    }

    std::vector<std::uint8_t> result(static_cast<std::uint8_t*>(data),
                                     static_cast<std::uint8_t*>(data) + out_size);
    mz_free(data);
    mz_zip_reader_end(&zip);
    return result;
  }

  return std::nullopt;
}

std::vector<std::string> ArchiveMount::list(const std::string& prefix) const {
  const auto normalized_prefix = normalize_path(prefix);
  std::unordered_map<std::string, bool> seen;
  for (const auto& archive : archives_) {
    for (const auto& [path, _] : archive.index) {
      if (normalized_prefix.empty() || path.rfind(normalized_prefix, 0) == 0) {
        seen[path] = true;
      }
    }
  }
  std::vector<std::string> paths;
  paths.reserve(seen.size());
  for (const auto& [path, _] : seen) {
    paths.push_back(path);
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

void ArchiveMount::clear() { archives_.clear(); }

}  // namespace bf2
