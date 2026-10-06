// Copyright 2026 MDJ. Use of this source code is governed by a BSD-style
// license that can be found in the Chromium LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_VIEW_H_

#include <map>
#include <memory>
#include <string>

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "chrome/browser/ui/views/sidebar_apps/sidebar_app_model.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/mojom/menu_source_type.mojom-forward.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/view.h"

class BrowserWindowInterface;
class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace ui {
class Event;
}  // namespace ui

namespace views {
class Checkbox;
class LabelButton;
class MenuRunner;
class Textfield;
}  // namespace views

namespace sidebar_apps {

class AppEntryView;

// The strip on the left edge of a browser window showing one button per saved
// sidebar app, with a badge for unread notifications.
//
// Each app is a real browser tab of this window that the tab strip does not
// draw (so permission prompts, file uploads, downloads, popups and everything
// else work as in any tab). Clicking an app's button activates its tab.
// Switching to any other tab goes back to normal browsing.
//
// One window per profile (the "host") also opens every app in a background tab
// shortly after startup, so the apps can receive notifications and calls
// before they are clicked. The host reads each app's unread count from its
// page title (for example "(3) Inbox") and publishes it through the model.
class SidebarView : public views::View,
                    public SidebarAppModel::Observer,
                    public views::ContextMenuController,
                    public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(SidebarView, views::View)

 public:
  SidebarView(Profile* profile, BrowserWindowInterface* browser);
  SidebarView(const SidebarView&) = delete;
  SidebarView& operator=(const SidebarView&) = delete;
  ~SidebarView() override;

  // views::View:
  void OnThemeChanged() override;

  // SidebarAppModel::Observer:
  void OnSidebarAppsChanged() override;
  void OnUnreadCountsChanged() override;

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override;

  // ui::SimpleMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  class TitleObserver;

  void RebuildButtons();
  void OnAppPressed(const std::string& app_id, const ui::Event& event);
  void OnAddPressed(const ui::Event& event);
  // Shows the "Add app" form (|app_id| empty) or the "Edit app" form.
  void ShowAppDialog(const std::string& app_id);
  // Called when the form's Save button is pressed. Returns false to keep the
  // form open (for example when the address is not valid).
  bool OnAppDialogAccepted(views::Textfield* name_field,
                           views::Textfield* url_field,
                           views::Textfield* icon_field,
                           views::Checkbox* reset_icon_checkbox,
                           const std::string& app_id);
  // Custom icons: a PNG chosen by the user, copied into the profile folder
  // (SidebarIcons/<app id>.png) so it survives restarts. It always wins over
  // the icon of the site.
  base::FilePath CustomIconPath(const std::string& app_id) const;
  void SetCustomIconFromFile(const std::string& app_id,
                             const base::FilePath& source);
  void LoadCustomIcon(const std::string& app_id);
  void OnCustomIconLoaded(const std::string& app_id,
                          bool report_failure,
                          const SkBitmap& bitmap);
  void RemoveCustomIcon(const std::string& app_id);
  void ApplyThemeToAddButton();
  // Adds the page of the active tab as a new app and turns that tab into the
  // app's hidden tab. If the site already is an app, switches to it instead.
  void AddCurrentPageAsApp();
  // Apps removed from the model: show their tab again as an ordinary tab and
  // forget everything we tracked for them.
  void PruneRemovedApps();
  void OnActiveTabChanged(BrowserWindowInterface* browser);

  // Activates the tab of |app|, opening it first if needed.
  void OpenOrActivateApp(const SidebarApp& app);
  // Opens |app| in a new tab, in the foreground or the background, and hides
  // that tab from the tab strip.
  void OpenAppTab(const SidebarApp& app, bool foreground);
  // Remembers |contents| as the tab of |app|, hides it from the tab strip and
  // starts watching its title for an unread count.
  void TrackAppTab(const SidebarApp& app, content::WebContents* contents);

  // Background startup of the apps (host window only).
  void ScheduleAppLaunch();
  void LaunchMissingApps();
  // Looks for an ordinary tab of this window that already shows |app| (for
  // example one brought back by session restore) and, if found, turns it into
  // the app's hidden tab instead of opening a duplicate.
  bool AdoptExistingTab(const SidebarApp& app);

  void OnUnreadCountParsed(const std::string& app_id, int count);
  void UpdateBadges();

  // Shows the site's favicon on the app's button once the page has one.
  void OnAppTabUiChanged(const std::string& app_id);

  // Works out which app (if any) owns the active tab and updates the buttons.
  void UpdateActiveApp();
  // Highlights the button of the app that is currently shown.
  void UpdateActiveIndicator();

  raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<Profile> profile_;
  raw_ptr<SidebarAppModel> model_;
  std::string active_app_id_;
  bool is_app_host_ = false;
  // The entries currently in the strip, by app id. Children are owned by the
  // view hierarchy; this map is cleared before they are removed.
  std::map<std::string, raw_ptr<AppEntryView>> entries_;
  // The tab opened for each app in this window, by app id.
  std::map<std::string, base::WeakPtr<content::WebContents>> app_tabs_;
  // Favicons seen so far, by app id, so rebuilt buttons can show them again.
  std::map<std::string, gfx::ImageSkia> app_icons_;
  // Icons chosen by the user, by app id. These replace the site's icon.
  std::map<std::string, gfx::ImageSkia> custom_icons_;
  // Tells us when an app tab's favicon may have changed.
  std::map<std::string, base::CallbackListSubscription> favicon_subscriptions_;
  // Watches each app tab's title for an unread count (host window only).
  std::map<std::string, std::unique_ptr<TitleObserver>> title_observers_;
  raw_ptr<views::LabelButton> add_button_ = nullptr;
  // The right-click menu of an app button, and the app it was opened for.
  std::unique_ptr<ui::SimpleMenuModel> context_menu_model_;
  std::unique_ptr<views::MenuRunner> context_menu_runner_;
  std::string context_menu_app_id_;
  base::OneShotTimer launch_timer_;
  base::CallbackListSubscription active_tab_subscription_;
  base::ScopedObservation<SidebarAppModel, SidebarAppModel::Observer>
      model_observation_{this};
  base::WeakPtrFactory<SidebarView> weak_factory_{this};
};

}  // namespace sidebar_apps

#endif  // CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_VIEW_H_
