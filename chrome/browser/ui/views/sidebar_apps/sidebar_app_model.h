// Copyright 2026 MDJ. Use of this source code is governed by a BSD-style
// license that can be found in the Chromium LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_APP_MODEL_H_
#define CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_APP_MODEL_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "url/gurl.h"

class Profile;

namespace sidebar_apps {

// One web app shown in the sidebar.
struct SidebarApp {
  // Stable unique id (random UUID). Never changes after creation.
  std::string id;
  std::string name;
  GURL url;
  // Optional custom icon as a data: URL. Empty means "use the site favicon".
  std::string custom_icon_data_url;
};

// The saved list of sidebar apps for one profile. Stored as JSON in
// <profile dir>/SidebarApps.json. Shared by all browser windows of the profile.
// Lives as user data on the Profile, so it is destroyed with the profile.
//
// All file access happens on a background sequence: the UI thread is not
// allowed to block. The list is therefore empty right after creation and
// observers are notified (OnSidebarAppsChanged) once the file has been read.
class SidebarAppModel : public base::SupportsUserData::Data {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnSidebarAppsChanged() = 0;
    // Called when the unread count of any app changes.
    virtual void OnUnreadCountsChanged() {}
  };

  // Returns the model for |profile|, creating it and starting the load on
  // first use.
  static SidebarAppModel* GetForProfile(Profile* profile);

  explicit SidebarAppModel(const base::FilePath& profile_dir);
  SidebarAppModel(const SidebarAppModel&) = delete;
  SidebarAppModel& operator=(const SidebarAppModel&) = delete;
  ~SidebarAppModel() override;

  const std::vector<SidebarApp>& apps() const { return apps_; }
  const SidebarApp* FindApp(const std::string& id) const;

  // Adds an app and returns its new id, or an empty string if |url| is not a
  // valid http(s) URL.
  std::string AddApp(const std::string& name,
                     const GURL& url,
                     const std::string& custom_icon_data_url);

  // Replaces the name, url and icon of the app with |app.id|. Returns false if
  // there is no such app or the url is invalid.
  bool UpdateApp(const SidebarApp& app);

  // Returns false if there is no app with |id|.
  bool RemoveApp(const std::string& id);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Unread notification counts, reported by the window that hosts the apps and
  // shown as badges. Not saved to disk. Counts are never negative.
  void SetUnreadCount(const std::string& app_id, int count);
  int GetUnreadCount(const std::string& app_id) const;
  int GetTotalUnreadCount() const;

  // Only one window per profile starts the apps in the background at launch.
  // Returns true if |host| is (or has just become) that window.
  bool TryClaimAppHost(const void* host);
  void ReleaseAppHost(const void* host);

 private:
  // Starts reading the file on the background sequence.
  void Load();
  // Runs on the UI thread with the file contents (nullopt if unreadable).
  void OnFileRead(std::optional<std::string> contents);
  // Serializes the list and writes it on the background sequence.
  void Save();
  void NotifyChanged();
  void NotifyUnreadChanged();

  const base::FilePath file_path_;
  std::vector<SidebarApp> apps_;
  base::ObserverList<Observer> observers_;
  // App id -> unread count. Apps without an entry have no unread items.
  std::map<std::string, int> unread_counts_;
  // All file reads and writes run on this sequence, in order.
  scoped_refptr<base::SequencedTaskRunner> io_task_runner_;
  // Identity of the window that hosts the background apps (0 = none). Only
  // compared, never dereferenced.
  uintptr_t app_host_ = 0;

  // Must be the last member.
  base::WeakPtrFactory<SidebarAppModel> weak_factory_{this};
};

}  // namespace sidebar_apps

#endif  // CHROME_BROWSER_UI_VIEWS_SIDEBAR_APPS_SIDEBAR_APP_MODEL_H_
