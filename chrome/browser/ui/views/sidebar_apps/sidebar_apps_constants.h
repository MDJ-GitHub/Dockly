// Copyright 2026 MDJ. Use of this source code is governed by a BSD-style
// license that can be found in the Chromium LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_APPS_CONSTANTS_H_
#define CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_APPS_CONSTANTS_H_

// Kept deliberately tiny so that the browser layout code can include it
// without pulling in any heavy headers.
namespace sidebar_apps {

// Width of the sidebar strip, in DIPs.
inline constexpr int kSidebarWidth = 56;

// View ids used to find our views among BrowserView's children.
inline constexpr int kSidebarViewId = 0x44434B31;
inline constexpr int kAppViewId = 0x44434B32;

}  // namespace sidebar_apps

#endif  // CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_APPS_CONSTANTS_H_
