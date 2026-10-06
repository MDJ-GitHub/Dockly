// Copyright 2026 MDJ. Use of this source code is governed by a BSD-style
// license that can be found in the Chromium LICENSE file.

#include "chrome/browser/ui/views/tabs/dockly_hidden_tabs.h"

#include <memory>
#include <utility>

#include "base/callback_list.h"
#include "base/no_destructor.h"
#include "base/supports_user_data.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"

namespace sidebar_apps {

namespace {

const char kHiddenAppTabKey[] = "DocklyHiddenAppTab";

// Presence of this user data on a WebContents means "hidden app tab".
class HiddenAppTabMarker : public base::SupportsUserData::Data {};

base::RepeatingClosureList& GetCallbacks() {
  static base::NoDestructor<base::RepeatingClosureList> callbacks;
  return *callbacks;
}

}  // namespace

void SetHiddenAppTab(content::WebContents* contents, bool hidden) {
  if (!contents || IsHiddenAppTab(contents) == hidden) {
    return;
  }
  if (hidden) {
    contents->SetUserData(kHiddenAppTabKey,
                          std::make_unique<HiddenAppTabMarker>());
  } else {
    contents->RemoveUserData(kHiddenAppTabKey);
  }
  GetCallbacks().Notify();
}

bool IsHiddenAppTab(const content::WebContents* contents) {
  return contents && contents->GetUserData(kHiddenAppTabKey) != nullptr;
}

bool IsHiddenAppTab(tabs::TabInterface* tab) {
  return tab && IsHiddenAppTab(tab->GetContents());
}

base::CallbackListSubscription AddHiddenAppTabsChangedCallback(
    base::RepeatingClosure callback) {
  return GetCallbacks().Add(std::move(callback));
}

}  // namespace sidebar_apps
