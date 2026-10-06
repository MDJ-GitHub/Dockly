// Copyright 2026 MDJ. Use of this source code is governed by a BSD-style
// license that can be found in the Chromium LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_TRAY_H_
#define CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_TRAY_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/status_icons/status_icon_menu_model.h"
#include "chrome/browser/status_icons/status_icon_observer.h"
#include "chrome/browser/ui/views/sidebar_apps/sidebar_app_model.h"

class BrowserWindowInterface;
class StatusIcon;

namespace sidebar_apps {

// The system tray icon.
//
// - It shows the total number of unread items across all sidebar apps (a dot
//   and a white number), or a white check mark when nothing is unread.
// - Left-click brings the browser window back. Right-click opens a menu with
//   "Open" and "Quit".
// - It also decides whether closing the window that hosts the sidebar apps
//   should hide it to the tray instead, so the apps keep running.
//
// There is one tray icon per process. It follows the model of the first
// regular (non-incognito) profile that shows a sidebar. The object is created
// on first use and intentionally never destroyed: the tray itself is torn down
// by the browser process at exit.
class SidebarTray : public SidebarAppModel::Observer,
                    public StatusIconObserver,
                    public StatusIconMenuModel::Delegate {
 public:
  // Creates the tray icon (once) and keeps it in sync with |model|. Later
  // calls, for other models, do nothing.
  static void AttachToModel(SidebarAppModel* model);

  static SidebarTray& Get();

  // True if a request to close |browser| should hide its window to the tray
  // instead: it is the window that hosts the sidebar apps and the browser is
  // not being quit.
  static bool ShouldHideInsteadOfClosing(BrowserWindowInterface* browser);

  // True while a browser that was started with --start-in-tray should keep
  // its windows hidden. BrowserView::Show() and friends check this during the
  // first moments after startup; it turns false after about 20 seconds, or as
  // soon as the user opens the window from the tray.
  static bool ShouldSuppressShow();

  // Remembers a window to bring forward when the tray icon is clicked. The
  // sidebar of each regular window calls this when it is created and whenever
  // its active tab changes, so the tray always knows a recent, live window.
  void SetLastActiveWindow(base::WeakPtr<BrowserWindowInterface> window);

  // The window that holds the apps' tabs. Closing it hides it to the tray.
  void SetHostWindow(base::WeakPtr<BrowserWindowInterface> window);

  SidebarTray(const SidebarTray&) = delete;
  SidebarTray& operator=(const SidebarTray&) = delete;
  ~SidebarTray() override;

  // SidebarAppModel::Observer:
  void OnSidebarAppsChanged() override;
  void OnUnreadCountsChanged() override;

  // StatusIconObserver:
  void OnStatusIconClicked() override;

  // StatusIconMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  friend class base::NoDestructor<SidebarTray>;
  SidebarTray();

  void Attach(SidebarAppModel* model);
  void UpdateIcon();
  void CreateContextMenu();
  // Shows and activates the host window (or the last active one).
  void ShowWindow();

  // The model may be destroyed with its profile, and the status icon is
  // destroyed by the tray at exit, so neither pointer is owned or assumed to
  // outlive this object. Both are only used from model notifications.
  raw_ptr<SidebarAppModel, DanglingUntriaged> model_ = nullptr;
  raw_ptr<StatusIcon, DanglingUntriaged> icon_ = nullptr;
  base::WeakPtr<BrowserWindowInterface> last_active_window_;
  base::WeakPtr<BrowserWindowInterface> host_window_;
  // Set when the user chose "Quit" in the tray menu.
  bool quitting_ = false;
};

}  // namespace sidebar_apps

#endif  // CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_TRAY_H_
