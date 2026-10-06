// Copyright 2026 MDJ. Use of this source code is governed by a BSD-style
// license that can be found in the Chromium LICENSE file.

#include "chrome/browser/ui/views/sidebar_apps/sidebar_autostart.h"

#include <string>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/win/registry.h"

namespace sidebar_apps {

namespace {

constexpr wchar_t kRunKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"Dockly";

// "C:\path\to\chrome.exe" --start-in-tray
std::wstring BuildStartupCommand() {
  base::FilePath exe_path;
  if (!base::PathService::Get(base::FILE_EXE, &exe_path)) {
    return std::wstring();
  }
  return L"\"" + exe_path.value() + L"\" --start-in-tray";
}

}  // namespace

bool IsAutostartEnabled() {
  base::win::RegKey key(HKEY_CURRENT_USER, kRunKeyPath, KEY_QUERY_VALUE);
  std::wstring value;
  return key.Valid() && key.ReadValue(kValueName, &value) == ERROR_SUCCESS &&
         !value.empty();
}

void SetAutostartEnabled(bool enabled) {
  base::win::RegKey key(HKEY_CURRENT_USER, kRunKeyPath, KEY_SET_VALUE);
  if (!key.Valid()) {
    LOG(WARNING) << "Could not open the Windows startup registry key.";
    return;
  }
  if (enabled) {
    const std::wstring command = BuildStartupCommand();
    if (command.empty()) {
      return;
    }
    if (key.WriteValue(kValueName, command.c_str()) != ERROR_SUCCESS) {
      LOG(WARNING) << "Could not write the Windows startup entry.";
    }
  } else {
    if (key.DeleteValue(kValueName) != ERROR_SUCCESS) {
      LOG(WARNING) << "Could not remove the Windows startup entry.";
    }
  }
}

}  // namespace sidebar_apps
