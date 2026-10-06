// Copyright 2026 MDJ. Use of this source code is governed by a BSD-style
// license that can be found in the Chromium LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_AUTOSTART_H_
#define CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_AUTOSTART_H_

namespace sidebar_apps {

// "Start with Windows": a value under HKCU\Software\Microsoft\Windows\
// CurrentVersion\Run that launches this browser at login with
// --start-in-tray, so it starts hidden in the tray and the sidebar apps begin
// receiving notifications right away.

// True if the startup entry exists.
bool IsAutostartEnabled();

// Creates (or removes) the startup entry for the running browser executable.
void SetAutostartEnabled(bool enabled);

}  // namespace sidebar_apps

#endif  // CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_AUTOSTART_H_
