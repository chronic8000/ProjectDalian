#include "bf2_path.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <SDL_syswm.h>
#endif

namespace dalian {
namespace {

std::string slash_forward(std::string p) {
  for (char& c : p)
    if (c == '\\') c = '/';
  while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
  return p;
}

bool has_mods_dir(const std::filesystem::path& root) {
  std::error_code ec;
  return std::filesystem::is_directory(root / "mods", ec);
}

#ifdef _WIN32
std::string read_reg_install_dir(HKEY hive, const char* subkey, const char* value_name) {
  HKEY key = nullptr;
  if (RegOpenKeyExA(hive, subkey, 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS &&
      RegOpenKeyExA(hive, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return {};
  }
  char buf[MAX_PATH] = {};
  DWORD type = 0;
  DWORD size = sizeof(buf);
  const LONG ok = RegQueryValueExA(key, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
  RegCloseKey(key);
  if (ok != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || buf[0] == '\0') return {};
  return slash_forward(buf);
}

void add_registry_candidates(std::vector<std::string>& out) {
  const char* keys[] = {
      "SOFTWARE\\EA Games\\Battlefield 2",
      "SOFTWARE\\WOW6432Node\\EA Games\\Battlefield 2",
      "SOFTWARE\\Electronic Arts\\EA Games\\Battlefield 2",
      "SOFTWARE\\WOW6432Node\\Electronic Arts\\EA Games\\Battlefield 2",
      "SOFTWARE\\Electronic Arts\\Battlefield 2",
      "SOFTWARE\\WOW6432Node\\Electronic Arts\\Battlefield 2",
  };
  const char* vals[] = {"Install Dir", "InstallDir", "InstallPath", "Path"};
  for (const char* k : keys) {
    for (const char* v : vals) {
      std::string p = read_reg_install_dir(HKEY_LOCAL_MACHINE, k, v);
      if (p.empty()) p = read_reg_install_dir(HKEY_CURRENT_USER, k, v);
      if (!p.empty()) out.push_back(std::move(p));
    }
  }
}
#endif

void push_unique(std::vector<std::string>& out, std::string path) {
  path = slash_forward(std::move(path));
  if (path.empty()) return;
  for (const auto& e : out) {
    if (e == path) return;
  }
  out.push_back(std::move(path));
}

}  // namespace

bool is_valid_bf2_root(const std::string& path) {
  if (path.empty()) return false;
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) && has_mods_dir(path);
}

std::string normalize_bf2_root(std::string path) {
  path = slash_forward(std::move(path));
  if (path.empty()) return {};
  std::filesystem::path p(path);
  std::error_code ec;
  if (!std::filesystem::exists(p, ec)) return {};

  // User picked the mods folder itself.
  if (p.filename() == "mods" && has_mods_dir(p.parent_path())) {
    return slash_forward(p.parent_path().string());
  }
  if (has_mods_dir(p)) return slash_forward(p.string());

  // Picked EA Games / Origin Games parent — look for Battlefield 2 under it.
  for (const char* child : {"Battlefield 2", "Battlefield2", "BF2"}) {
    const auto c = p / child;
    if (has_mods_dir(c)) return slash_forward(c.string());
  }
  // Picked a level or mod folder accidentally — climb a few parents.
  auto cur = p;
  for (int i = 0; i < 5; ++i) {
    if (has_mods_dir(cur)) return slash_forward(cur.string());
    if (!cur.has_parent_path() || cur == cur.parent_path()) break;
    cur = cur.parent_path();
  }
  return {};
}

std::vector<std::string> bf2_install_candidates() {
  std::vector<std::string> out;

  // Classic retail / DVD / GOG-style.
  push_unique(out, "C:/Program Files (x86)/Battlefield2");
  push_unique(out, "C:/Program Files/Battlefield2");
  push_unique(out, "C:/Program Files (x86)/Battlefield 2");
  push_unique(out, "C:/Program Files/Battlefield 2");
  push_unique(out, "D:/Games/Battlefield 2");
  push_unique(out, "D:/Games/Battlefield2");
  push_unique(out, "E:/Games/Battlefield 2");

  // EA App default library root + game folder.
  push_unique(out, "C:/Program Files/EA Games/Battlefield 2");
  push_unique(out, "C:/Program Files (x86)/EA Games/Battlefield 2");
  push_unique(out, "C:/Program Files/EA Games/Battlefield2");
  push_unique(out, "D:/EA Games/Battlefield 2");
  push_unique(out, "D:/Program Files/EA Games/Battlefield 2");
  push_unique(out, "E:/EA Games/Battlefield 2");

  // Legacy Origin.
  push_unique(out, "C:/Program Files (x86)/Origin Games/Battlefield 2");
  push_unique(out, "C:/Program Files/Origin Games/Battlefield 2");
  push_unique(out, "D:/Origin Games/Battlefield 2");

  // Electronic Arts branded trees.
  push_unique(out, "C:/Program Files/Electronic Arts/Battlefield 2");
  push_unique(out, "C:/Program Files (x86)/Electronic Arts/Battlefield 2");

#ifdef _WIN32
  add_registry_candidates(out);
  if (const char* pf = std::getenv("ProgramFiles")) {
    push_unique(out, std::string(pf) + "/EA Games/Battlefield 2");
    push_unique(out, std::string(pf) + "/Battlefield 2");
    push_unique(out, std::string(pf) + "/Battlefield2");
  }
  if (const char* pf86 = std::getenv("ProgramFiles(x86)")) {
    push_unique(out, std::string(pf86) + "/EA Games/Battlefield 2");
    push_unique(out, std::string(pf86) + "/Origin Games/Battlefield 2");
    push_unique(out, std::string(pf86) + "/Battlefield2");
    push_unique(out, std::string(pf86) + "/Battlefield 2");
  }
#endif

  return out;
}

std::string browse_bf2_install_folder(SDL_Window* window) {
#ifdef _WIN32
  HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool co_owned = SUCCEEDED(co_hr) || co_hr == S_FALSE || co_hr == RPC_E_CHANGED_MODE;

  HWND hwnd = nullptr;
  if (window) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info)) hwnd = info.info.win.window;
  }

  IFileDialog* dialog = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog));
  std::string result;
  if (SUCCEEDED(hr) && dialog) {
    DWORD opts = 0;
    dialog->GetOptions(&opts);
    dialog->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Select Battlefield 2 install folder");
    hr = dialog->Show(hwnd);
    if (SUCCEEDED(hr)) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item)) && item) {
        PWSTR path_w = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path_w)) && path_w) {
          char narrow[MAX_PATH * 4] = {};
          WideCharToMultiByte(CP_UTF8, 0, path_w, -1, narrow, sizeof(narrow), nullptr, nullptr);
          result = narrow;
          CoTaskMemFree(path_w);
        }
        item->Release();
      }
    }
    dialog->Release();
  }

  if (co_owned && SUCCEEDED(co_hr)) CoUninitialize();
  return result;
#else
  (void)window;
  return {};
#endif
}

bool apply_bf2_root(Settings& settings, const std::string& chosen) {
  const std::string root = normalize_bf2_root(chosen);
  if (root.empty() || !is_valid_bf2_root(root)) {
    std::cerr << "BF2 path rejected (need a folder that contains mods/): " << chosen << '\n';
    return false;
  }
  if (settings.bf2_root == root) return true;
  settings.bf2_root = root;
  settings.save();
  std::cout << "BF2 install path set to: " << root << '\n';
  return true;
}

std::string resolve_bf2_root(Settings& settings, const char* argv1) {
  if (is_valid_bf2_root(settings.bf2_root)) return settings.bf2_root;
  // Stale / mistyped saved path — clear so UI shows unset until we find one.
  if (!settings.bf2_root.empty() && !is_valid_bf2_root(settings.bf2_root)) {
    std::cerr << "Saved BF2 path is invalid, rescanning: " << settings.bf2_root << '\n';
    settings.bf2_root.clear();
  }

  if (argv1) {
    std::string p = argv1;
    for (char& c : p)
      if (c == '\\') c = '/';
    const auto pos = p.find("/mods/");
    if (pos != std::string::npos) {
      if (apply_bf2_root(settings, p.substr(0, pos))) return settings.bf2_root;
    }
    // argv might be the install root itself.
    if (apply_bf2_root(settings, p)) return settings.bf2_root;
  }

  for (const auto& probe : bf2_install_candidates()) {
    if (apply_bf2_root(settings, probe)) return settings.bf2_root;
  }
  return {};
}

}  // namespace dalian
