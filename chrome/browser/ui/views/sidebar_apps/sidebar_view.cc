// Copyright 2026 MDJ. Use of this source code is governed by a BSD-style
// license that can be found in the Chromium LICENSE file.

#include "chrome/browser/ui/views/sidebar_apps/sidebar_view.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/tab_ui_helper.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/sidebar_apps/sidebar_apps_constants.h"
#include "chrome/browser/ui/views/sidebar_apps/sidebar_tray.h"
#include "chrome/browser/ui/views/tabs/dockly_hidden_tabs.h"
#include "components/tabs/public/tab_interface.h"
#include "components/constrained_window/constrained_window_views.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/checkbox.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/menu/menu_types.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/layout/layout_types.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/window/dialog_delegate.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace sidebar_apps {

namespace {

// kSidebarWidth (56) minus 8 DIPs of margin on each side.
constexpr int kButtonSize = 40;
constexpr int kBadgeHeight = 16;
constexpr int kIconSize = 24;
// Corner radius of the hover / active highlight of an app button.
constexpr int kHighlightRadius = 10;
// Custom icons larger than this are refused.
constexpr size_t kMaxIconFileBytes = 5 * 1024 * 1024;

// How long after the app list changes (or loads) the host window waits before
// starting the apps. This gives session restore time to bring back tabs, which
// are then reused instead of duplicated.
constexpr base::TimeDelta kAppLaunchDelay = base::Seconds(2);

constexpr int kCommandReload = 1;
constexpr int kCommandRemove = 2;
constexpr int kCommandEdit = 3;
constexpr int kCommandAddCurrent = 4;
constexpr int kCommandAddByUrl = 5;

// A readable default name for a site: "https://web.whatsapp.com/" becomes
// "Whatsapp". Uses the registrable domain's first label.
std::string NameForUrl(const GURL& url) {
  std::string name = net::registry_controlled_domains::GetDomainAndRegistry(
      url, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  if (name.empty()) {
    name = url.host();
  }
  const size_t dot = name.find('.');
  if (dot != std::string::npos) {
    name.resize(dot);
  }
  if (!name.empty()) {
    name[0] = base::ToUpperASCII(name[0]);
  }
  return name;
}

bool IsDigit(char16_t c) {
  return c >= u'0' && c <= u'9';
}

// Reads the digits that start at |pos| in |title| and are followed by
// |closing|. Returns -1 if there is no such number (1 to 5 digits).
int ReadNumberAt(std::u16string_view title, size_t pos, char16_t closing) {
  int value = 0;
  size_t digits = 0;
  while (pos < title.size() && IsDigit(title[pos]) && digits < 5) {
    value = value * 10 + (title[pos] - u'0');
    ++pos;
    ++digits;
  }
  if (digits == 0 || pos >= title.size() || title[pos] != closing) {
    return -1;
  }
  return value;
}

// Most chat and mail sites put their unread count in the page title:
// "(3) Inbox", "[2] Chat", or "Inbox (12) - me@example.com". Returns 0 when
// the title has no such count.
int ParseUnreadCount(std::u16string_view title) {
  size_t start = 0;
  while (start < title.size() && title[start] == u' ') {
    ++start;
  }
  if (start < title.size() && title[start] == u'(') {
    return std::max(0, ReadNumberAt(title, start + 1, u')'));
  }
  if (start < title.size() && title[start] == u'[') {
    return std::max(0, ReadNumberAt(title, start + 1, u']'));
  }
  constexpr std::u16string_view kInbox = u"Inbox (";
  const size_t inbox = title.find(kInbox);
  if (inbox != std::u16string_view::npos) {
    return std::max(0, ReadNumberAt(title, inbox + kInbox.size(), u')'));
  }
  return 0;
}

SkColor ForegroundColor(const views::View* view) {
  const ui::ColorProvider* provider = view->GetColorProvider();
  return provider ? provider->GetColor(ui::kColorPrimaryForeground)
                  : SK_ColorGRAY;
}

// Ids are made by the model, but a file name is only ever built from plain
// characters.
bool IsSafeFileName(const std::string& id) {
  if (id.empty()) {
    return false;
  }
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// Icon files are read, written and deleted one after the other on this runner,
// so a load can never overtake a save.
const scoped_refptr<base::SequencedTaskRunner>& IconTaskRunner() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}));
  return *runner;
}

// Runs on the icon runner. Returns a null bitmap if |path| is not a usable PNG.
SkBitmap DecodeIconFile(const base::FilePath& path) {
  std::optional<std::vector<uint8_t>> bytes = base::ReadFileToBytes(path);
  if (!bytes || bytes->empty() || bytes->size() > kMaxIconFileBytes) {
    return SkBitmap();
  }
  SkBitmap bitmap = gfx::PNGCodec::Decode(*bytes);
  if (bitmap.drawsNothing()) {
    return SkBitmap();
  }
  bitmap.setImmutable();
  return bitmap;
}

// Runs on the icon runner. Checks that |source| is a good PNG, then keeps a
// copy of it at |dest|. Returns a null bitmap on any failure.
SkBitmap ImportIconFile(const base::FilePath& source,
                        const base::FilePath& dest) {
  SkBitmap bitmap = DecodeIconFile(source);
  if (bitmap.isNull()) {
    return bitmap;
  }
  if (!base::CreateDirectory(dest.DirName()) ||
      !base::CopyFile(source, dest)) {
    return SkBitmap();
  }
  return bitmap;
}

// Soft rounded highlight drawn behind a button.
void PaintRoundedHighlight(gfx::Canvas* canvas,
                           const views::View* view,
                           SkAlpha alpha,
                           int radius) {
  if (alpha == 0) {
    return;
  }
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setStyle(cc::PaintFlags::kFill_Style);
  flags.setColor(SkColorSetA(ForegroundColor(view), alpha));
  canvas->DrawRoundRect(view->GetLocalBounds(), radius, flags);
}

}  // namespace

// The button of one app: the site's icon (or the first letter of its name)
// with a soft rounded rectangle behind it while the mouse is over it, while it
// is pressed, and while the app is the one being shown.
class SidebarButton : public views::LabelButton {
  METADATA_HEADER(SidebarButton, views::LabelButton)

 public:
  SidebarButton(views::Button::PressedCallback callback,
                const std::u16string& text)
      : views::LabelButton(std::move(callback), text) {}

  void SetActive(bool active) {
    if (active_ == active) {
      return;
    }
    active_ = active;
    SchedulePaint();
  }

 protected:
  // views::View:
  void OnPaintBackground(gfx::Canvas* canvas) override {
    SkAlpha alpha = active_ ? 0x2E : 0x00;
    if (GetState() == STATE_PRESSED) {
      alpha = 0x4A;
    } else if (GetState() == STATE_HOVERED) {
      alpha = active_ ? 0x3C : 0x24;
    }
    PaintRoundedHighlight(canvas, this, alpha, kHighlightRadius);
  }

 private:
  bool active_ = false;
};

BEGIN_METADATA(SidebarButton)
END_METADATA

// The "add app" button: a round button with a big plus. The circle is faint
// at rest and gets stronger when hovered or pressed.
class AddButton : public views::LabelButton {
  METADATA_HEADER(AddButton, views::LabelButton)

 public:
  explicit AddButton(views::Button::PressedCallback callback)
      : views::LabelButton(std::move(callback), std::u16string()) {
    GetViewAccessibility().SetName(u"Add app");
  }

 protected:
  // views::Button (Button::OnPaint() is final; this is its extension point):
  void PaintButtonContents(gfx::Canvas* canvas) override {
    SkAlpha alpha = 0x14;
    if (GetState() == STATE_PRESSED) {
      alpha = 0x48;
    } else if (GetState() == STATE_HOVERED) {
      alpha = 0x30;
    }
    const SkColor color = ForegroundColor(this);
    const gfx::Rect bounds = GetLocalBounds();
    const gfx::PointF center(bounds.CenterPoint());
    const float radius =
        std::min(bounds.width(), bounds.height()) / 2.0f - 2.0f;

    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setStyle(cc::PaintFlags::kFill_Style);
    fill.setColor(SkColorSetA(color, alpha));
    canvas->DrawCircle(center, radius, fill);

    constexpr float kArm = 9.0f;  // Half the length of each stroke of the plus.
    cc::PaintFlags stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(cc::PaintFlags::kStroke_Style);
    stroke.setStrokeWidth(2.5f);
    stroke.setStrokeCap(cc::PaintFlags::kRound_Cap);
    stroke.setColor(color);
    canvas->DrawLine(gfx::PointF(center.x() - kArm, center.y()),
                     gfx::PointF(center.x() + kArm, center.y()), stroke);
    canvas->DrawLine(gfx::PointF(center.x(), center.y() - kArm),
                     gfx::PointF(center.x(), center.y() + kArm), stroke);
  }
};

BEGIN_METADATA(AddButton)
END_METADATA

// One sidebar entry: the app button plus a small round badge in its top-right
// corner.
class AppEntryView : public views::View {
  METADATA_HEADER(AppEntryView, views::View)

 public:
  AppEntryView(views::Button::PressedCallback callback,
               const std::u16string& name) {
    SetPreferredSize(gfx::Size(kButtonSize, kButtonSize));
    letter_ = name.substr(0, 1);
    button_ = AddChildView(
        std::make_unique<SidebarButton>(std::move(callback), letter_));
    button_->SetTooltipText(name);
    badge_ = AddChildView(std::make_unique<views::Label>());
    badge_->SetEnabledColor(SK_ColorWHITE);
    badge_->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    badge_->SetBackground(
        views::CreateRoundedRectBackground(SkColorSetRGB(0xE5, 0x39, 0x35),
                                           kBadgeHeight / 2));
    // Clicks and right-clicks on the badge go to the button below it.
    badge_->SetCanProcessEventsWithinSubtree(false);
    badge_->SetVisible(false);
  }

  views::LabelButton* button() { return button_; }

  // Highlights the entry of the app that is currently shown.
  void SetActive(bool active) { button_->SetActive(active); }

  // Right-clicking the entry or its button opens the controller's menu.
  void SetContextMenuController(views::ContextMenuController* controller) {
    set_context_menu_controller(controller);
    button_->set_context_menu_controller(controller);
  }

  // Replaces the letter with the site's icon.
  void SetIcon(const gfx::ImageSkia& icon) {
    if (icon.isNull()) {
      return;
    }
    button_->SetImageModel(
        views::Button::STATE_NORMAL,
        ui::ImageModel::FromImageSkia(
            gfx::ImageSkiaOperations::CreateResizedImage(
                icon, skia::ImageOperations::RESIZE_BEST,
                gfx::Size(kIconSize, kIconSize))));
    button_->SetText(std::u16string());
  }

  // Goes back to the letter (used when a custom icon is removed).
  void ClearIcon() {
    button_->SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel());
    button_->SetText(letter_);
  }

  void SetBadgeCount(int count) {
    if (count <= 0) {
      badge_->SetVisible(false);
      return;
    }
    badge_->SetText(count > 99 ? u"99+" : base::NumberToString16(count));
    badge_->SetVisible(true);
    LayoutChildren();
  }

 protected:
  // views::View:
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    LayoutChildren();
  }
  void OnThemeChanged() override {
    views::View::OnThemeChanged();
    if (const ui::ColorProvider* provider = GetColorProvider()) {
      // The letter shown until the site's icon loads follows the theme.
      button_->SetEnabledTextColors(
          provider->GetColor(ui::kColorPrimaryForeground));
    }
  }

 private:
  void LayoutChildren() {
    button_->SetBoundsRect(gfx::Rect(size()));
    const int badge_width = badge_->GetText().size() > 2 ? 28 : 18;
    // Top-right corner, drawn on top of the icon.
    badge_->SetBoundsRect(
        gfx::Rect(width() - badge_width, 0, badge_width, kBadgeHeight));
  }

  std::u16string letter_;
  raw_ptr<SidebarButton> button_ = nullptr;
  raw_ptr<views::Label> badge_ = nullptr;
};

BEGIN_METADATA(AppEntryView)
END_METADATA

// Watches one app tab's title and reports the unread count found in it.
class SidebarView::TitleObserver : public content::WebContentsObserver {
 public:
  TitleObserver(content::WebContents* contents,
                base::RepeatingCallback<void(int)> callback)
      : content::WebContentsObserver(contents),
        callback_(std::move(callback)) {
    Report();
  }

  // content::WebContentsObserver:
  void TitleWasSet(content::NavigationEntry* entry) override { Report(); }
  void WebContentsDestroyed() override { callback_.Run(0); }

 private:
  void Report() {
    if (web_contents()) {
      callback_.Run(ParseUnreadCount(web_contents()->GetTitle()));
    }
  }

  base::RepeatingCallback<void(int)> callback_;
};

SidebarView::SidebarView(Profile* profile, BrowserWindowInterface* browser)
    : browser_(browser),
      profile_(profile),
      model_(SidebarAppModel::GetForProfile(profile)) {
  SetID(kSidebarViewId);

  auto* layout = SetLayoutManager(std::make_unique<views::FlexLayout>());
  layout->SetOrientation(views::LayoutOrientation::kVertical);
  layout->SetCrossAxisAlignment(views::LayoutAlignment::kStretch);
  layout->SetInteriorMargin(gfx::Insets::VH(8, 8));

  if (browser_) {
    active_tab_subscription_ = browser_->RegisterActiveTabDidChange(
        base::BindRepeating(&SidebarView::OnActiveTabChanged,
                            base::Unretained(this)));
  }

  model_observation_.Observe(model_);
  RebuildButtons();
  ScheduleAppLaunch();

  // One tray icon per process, following the first regular profile.
  if (!profile_->IsOffTheRecord()) {
    SidebarTray::AttachToModel(model_);
    if (browser_) {
      SidebarTray::Get().SetLastActiveWindow(browser_->GetWeakPtr());
    }
  }
}

SidebarView::~SidebarView() {
  model_->ReleaseAppHost(this);
}

void SidebarView::OnThemeChanged() {
  views::View::OnThemeChanged();
  const ui::ColorProvider* provider = GetColorProvider();
  if (!provider) {
    return;
  }
  // Same color as the toolbar, the bookmark bar and the active tab.
  SetBackground(
      views::CreateSolidBackground(provider->GetColor(kColorToolbar)));
  ApplyThemeToAddButton();
  UpdateActiveIndicator();
}

void SidebarView::OnSidebarAppsChanged() {
  PruneRemovedApps();
  RebuildButtons();
  ScheduleAppLaunch();
}

void SidebarView::OnUnreadCountsChanged() {
  UpdateBadges();
}

void SidebarView::RebuildButtons() {
  entries_.clear();
  add_button_ = nullptr;
  RemoveAllChildViews();
  for (const SidebarApp& app : model_->apps()) {
    auto entry = std::make_unique<AppEntryView>(
        base::BindRepeating(&SidebarView::OnAppPressed, base::Unretained(this),
                            app.id),
        base::UTF8ToUTF16(app.name));
    entry->SetProperty(views::kMarginsKey, gfx::Insets::VH(4, 0));
    entry->SetContextMenuController(this);
    AppEntryView* raw_entry = entry.get();
    AddChildView(std::move(entry));
    entries_[app.id] = raw_entry;
    auto custom = custom_icons_.find(app.id);
    auto icon = app_icons_.find(app.id);
    if (custom != custom_icons_.end()) {
      raw_entry->SetIcon(custom->second);
    } else if (icon != app_icons_.end()) {
      raw_entry->SetIcon(icon->second);
    }
    // Picks up a custom icon saved earlier (or by another window).
    LoadCustomIcon(app.id);
  }

  // An empty, stretchy view pushes the add button to the bottom.
  auto spacer = std::make_unique<views::View>();
  spacer->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::MinimumFlexSizeRule::kScaleToZero,
                               views::MaximumFlexSizeRule::kUnbounded));
  AddChildView(std::move(spacer));

  auto add_button = std::make_unique<AddButton>(
      base::BindRepeating(&SidebarView::OnAddPressed, base::Unretained(this)));
  add_button->SetTooltipText(
      u"Add the current page to the sidebar (right-click for more)");
  add_button->set_context_menu_controller(this);
  add_button->SetPreferredSize(gfx::Size(kButtonSize, kButtonSize));
  add_button->SetProperty(views::kMarginsKey, gfx::Insets::VH(4, 0));
  add_button_ = AddChildView(std::move(add_button));
  ApplyThemeToAddButton();

  UpdateBadges();
  UpdateActiveApp();
}

void SidebarView::ApplyThemeToAddButton() {
  // The button paints itself from the theme; just make it repaint.
  if (add_button_) {
    add_button_->SchedulePaint();
  }
}

void SidebarView::UpdateBadges() {
  for (const auto& [id, entry] : entries_) {
    const int count = model_->GetUnreadCount(id);
    entry->SetBadgeCount(count);
    if (count == 0) {
      // Back to a clean state: take the site's icon again (see
      // OnAppTabUiChanged()).
      OnAppTabUiChanged(id);
    }
  }
}

void SidebarView::OnAddPressed(const ui::Event& event) {
  AddCurrentPageAsApp();
}

void SidebarView::AddCurrentPageAsApp() {
  if (!browser_) {
    return;
  }
  content::WebContents* contents =
      browser_->GetTabStripModel()->GetActiveWebContents();
  if (!contents || IsHiddenAppTab(contents)) {
    return;  // Nothing to add, or this page already is a sidebar app.
  }
  const GURL url = contents->GetLastCommittedURL();
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return;
  }

  // Already a sidebar app for this site? Go to it instead of adding another.
  const url::Origin origin = url::Origin::Create(url);
  for (const SidebarApp& existing : model_->apps()) {
    if (url::Origin::Create(existing.url) == origin) {
      OpenOrActivateApp(existing);
      return;
    }
  }

  const std::string id = model_->AddApp(NameForUrl(url), url, std::string());
  const SidebarApp* app = model_->FindApp(id);
  if (!app) {
    return;
  }
  // Turn the current tab into the app's hidden tab; the page stays on screen.
  TrackAppTab(*app, contents);
  UpdateActiveApp();
}

void SidebarView::PruneRemovedApps() {
  for (auto it = app_tabs_.begin(); it != app_tabs_.end();) {
    if (model_->FindApp(it->first)) {
      ++it;
      continue;
    }
    const std::string id = it->first;
    if (content::WebContents* contents = it->second.get()) {
      // The tab stays open, now as an ordinary tab.
      SetHiddenAppTab(contents, false);
    }
    title_observers_.erase(id);
    favicon_subscriptions_.erase(id);
    app_icons_.erase(id);
    it = app_tabs_.erase(it);
  }
  // Delete the custom icons of apps that are gone.
  std::vector<std::string> removed_icons;
  for (const auto& [id, icon] : custom_icons_) {
    if (!model_->FindApp(id)) {
      removed_icons.push_back(id);
    }
  }
  for (const std::string& id : removed_icons) {
    RemoveCustomIcon(id);
  }
}

void SidebarView::ShowContextMenuForViewImpl(
    views::View* source,
    const gfx::Point& point,
    ui::mojom::MenuSourceType source_type) {
  if (!source->GetWidget()) {
    return;
  }
  context_menu_model_ = std::make_unique<ui::SimpleMenuModel>(this);
  context_menu_app_id_.clear();

  if (add_button_ && source == add_button_.get()) {
    context_menu_model_->AddItem(kCommandAddCurrent, u"Add the current page");
    context_menu_model_->AddItem(kCommandAddByUrl, u"Add by address...");
  } else {
    for (const auto& [id, entry] : entries_) {
      if (source == entry.get() || source == entry->button()) {
        context_menu_app_id_ = id;
        break;
      }
    }
    if (context_menu_app_id_.empty()) {
      return;
    }
    context_menu_model_->AddItem(kCommandEdit, u"Edit...");
    context_menu_model_->AddItem(kCommandReload, u"Reload");
    context_menu_model_->AddItem(kCommandRemove, u"Remove from sidebar");
  }

  context_menu_runner_ = std::make_unique<views::MenuRunner>(
      context_menu_model_.get(),
      views::MenuRunner::HAS_MNEMONICS | views::MenuRunner::CONTEXT_MENU);
  context_menu_runner_->RunMenuAt(
      source->GetWidget(), /*button_controller=*/nullptr,
      gfx::Rect(point, gfx::Size()), views::MenuAnchorPosition::kTopLeft,
      source_type);
}

void SidebarView::ExecuteCommand(int command_id, int event_flags) {
  // Commands that do not belong to one app.
  if (command_id == kCommandAddCurrent) {
    AddCurrentPageAsApp();
    return;
  }
  if (command_id == kCommandAddByUrl) {
    ShowAppDialog(std::string());
    return;
  }

  const SidebarApp* app = model_->FindApp(context_menu_app_id_);
  if (!app) {
    return;
  }
  const std::string id = app->id;  // |app| dies when the app is removed.
  switch (command_id) {
    case kCommandEdit:
      ShowAppDialog(id);
      break;
    case kCommandReload: {
      auto it = app_tabs_.find(id);
      if (it != app_tabs_.end() && it->second) {
        it->second->GetController().Reload(content::ReloadType::NORMAL,
                                           /*check_for_repost=*/true);
      }
      break;
    }
    case kCommandRemove:
      model_->RemoveApp(id);
      break;
    default:
      break;
  }
}

void SidebarView::ShowAppDialog(const std::string& app_id) {
  views::Widget* parent_widget = GetWidget();
  if (!parent_widget) {
    return;
  }
  const SidebarApp* existing =
      app_id.empty() ? nullptr : model_->FindApp(app_id);
  if (!app_id.empty() && !existing) {
    return;
  }

  // The form: a label and a text field for the name and for the address.
  auto contents = std::make_unique<views::View>();
  auto* layout =
      contents->SetLayoutManager(std::make_unique<views::FlexLayout>());
  layout->SetOrientation(views::LayoutOrientation::kVertical);
  layout->SetCrossAxisAlignment(views::LayoutAlignment::kStretch);

  auto add_field = [&contents](const std::u16string& label,
                               const std::u16string& text,
                               const std::u16string& placeholder =
                                   std::u16string()) {
    contents->AddChildView(std::make_unique<views::Label>(label));
    auto field = std::make_unique<views::Textfield>();
    field->SetText(text);
    field->SetPlaceholderText(placeholder);
    field->GetViewAccessibility().SetName(label);
    field->SetPreferredSize(gfx::Size(340, 30));
    field->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(2, 0, 12, 0));
    return contents->AddChildView(std::move(field));
  };
  views::Textfield* name_field = add_field(
      u"Name", existing ? base::UTF8ToUTF16(existing->name) : std::u16string());
  views::Textfield* url_field =
      add_field(u"Address (for example https://web.whatsapp.com)",
                existing ? base::UTF8ToUTF16(existing->url.spec())
                         : std::u16string());
  views::Textfield* icon_field = add_field(
      u"Custom icon (optional, path to a PNG file)", std::u16string(),
      u"C:\\Icons\\mail.png");
  views::Checkbox* reset_icon_checkbox = nullptr;
  if (existing && custom_icons_.contains(app_id)) {
    reset_icon_checkbox = contents->AddChildView(
        std::make_unique<views::Checkbox>(u"Go back to the site's own icon"));
  }

  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->SetTitle(existing ? u"Edit sidebar app" : u"Add sidebar app");
  dialog->SetModalType(ui::mojom::ModalType::kWindow);
  dialog->SetButtonLabel(ui::mojom::DialogButton::kOk, u"Save");
  dialog->SetButtonLabel(ui::mojom::DialogButton::kCancel, u"Cancel");
  dialog->SetContentsView(std::move(contents));
  dialog->SetInitiallyFocusedView(existing ? url_field : name_field);
  // The weak pointer is passed as an ordinary argument (not as the receiver)
  // because this callback returns a value.
  dialog->SetAcceptCallbackWithClose(base::BindRepeating(
      [](base::WeakPtr<SidebarView> view, views::Textfield* name,
         views::Textfield* url, views::Textfield* icon,
         views::Checkbox* reset_icon, std::string id) {
        return view &&
               view->OnAppDialogAccepted(name, url, icon, reset_icon, id);
      },
      weak_factory_.GetWeakPtr(), base::Unretained(name_field),
      base::Unretained(url_field), base::Unretained(icon_field),
      base::Unretained(reset_icon_checkbox), app_id));

  views::Widget* widget = constrained_window::CreateBrowserModalDialogViews(
      std::move(dialog), parent_widget->GetNativeWindow());
  if (widget) {
    widget->Show();
  }
}

bool SidebarView::OnAppDialogAccepted(views::Textfield* name_field,
                                      views::Textfield* url_field,
                                      views::Textfield* icon_field,
                                      views::Checkbox* reset_icon_checkbox,
                                      const std::string& app_id) {
  std::string url_text = base::UTF16ToUTF8(url_field->GetText());
  url_text = std::string(base::TrimWhitespaceASCII(url_text, base::TRIM_ALL));
  if (url_text.empty()) {
    url_field->SetInvalid(true);
    return false;
  }
  // Be forgiving: "web.whatsapp.com" means "https://web.whatsapp.com".
  if (url_text.find("://") == std::string::npos) {
    url_text = "https://" + url_text;
  }
  const GURL url(url_text);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    url_field->SetInvalid(true);
    return false;
  }

  std::string name = base::UTF16ToUTF8(name_field->GetText());
  name = std::string(base::TrimWhitespaceASCII(name, base::TRIM_ALL));
  if (name.empty()) {
    name = NameForUrl(url);
  }

  // The custom icon: only the name is checked here (no file access on this
  // thread); a file that turns out to be unusable is reported in the log.
  base::FilePath icon_source;
  const std::u16string icon_text(base::TrimString(
      icon_field->GetText(), u" \t\"", base::TRIM_ALL));
  if (!icon_text.empty()) {
    icon_source = base::FilePath::FromUTF16Unsafe(icon_text);
    if (!icon_source.MatchesExtension(FILE_PATH_LITERAL(".png"))) {
      icon_field->SetInvalid(true);
      return false;
    }
  }

  if (app_id.empty()) {
    const std::string new_id = model_->AddApp(name, url, std::string());
    if (new_id.empty()) {
      return false;
    }
    if (!icon_source.empty()) {
      SetCustomIconFromFile(new_id, icon_source);
    }
    return true;
  }

  const SidebarApp* existing = model_->FindApp(app_id);
  if (!existing) {
    return true;  // Removed in the meantime; nothing to save.
  }
  SidebarApp updated = *existing;
  const bool url_changed = updated.url != url;
  updated.name = name;
  updated.url = url;
  if (!model_->UpdateApp(updated)) {
    return false;
  }
  if (!icon_source.empty()) {
    SetCustomIconFromFile(app_id, icon_source);
  } else if (reset_icon_checkbox && reset_icon_checkbox->GetChecked()) {
    RemoveCustomIcon(app_id);
  }
  // If the address changed, send the app's page there.
  if (url_changed) {
    auto it = app_tabs_.find(app_id);
    if (it != app_tabs_.end() && it->second) {
      it->second->GetController().LoadURLWithParams(
          content::NavigationController::LoadURLParams(url));
    }
  }
  return true;
}

void SidebarView::OnAppPressed(const std::string& app_id,
                               const ui::Event& event) {
  if (const SidebarApp* app = model_->FindApp(app_id)) {
    OpenOrActivateApp(*app);
  }
}

void SidebarView::OnActiveTabChanged(BrowserWindowInterface* browser) {
  UpdateActiveApp();
  if (!profile_->IsOffTheRecord() && browser_) {
    SidebarTray::Get().SetLastActiveWindow(browser_->GetWeakPtr());
  }
}

void SidebarView::OpenOrActivateApp(const SidebarApp& app) {
  if (!browser_) {
    return;
  }
  TabStripModel* tab_strip = browser_->GetTabStripModel();

  // Already open in this window (normally started in the background at
  // launch): switch to it right away, no loading.
  auto it = app_tabs_.find(app.id);
  if (it != app_tabs_.end() && it->second) {
    const int index = tab_strip->GetIndexOfWebContents(it->second.get());
    if (index != TabStripModel::kNoTab) {
      tab_strip->ActivateTabAt(index);
      return;
    }
  }

  // Not open yet (or its tab was closed): open it in the foreground.
  OpenAppTab(app, /*foreground=*/true);
  UpdateActiveApp();
}

void SidebarView::OpenAppTab(const SidebarApp& app, bool foreground) {
  content::OpenURLParams params(
      app.url, content::Referrer(),
      foreground ? WindowOpenDisposition::NEW_FOREGROUND_TAB
                 : WindowOpenDisposition::NEW_BACKGROUND_TAB,
      ui::PAGE_TRANSITION_AUTO_BOOKMARK, /*is_renderer_initiated=*/false);
  content::WebContents* contents = browser_->OpenURL(
      params, base::OnceCallback<void(content::NavigationHandle&)>());
  if (!contents) {
    LOG(WARNING) << "Could not open a tab for sidebar app " << app.id;
    return;
  }
  TrackAppTab(app, contents);
}

void SidebarView::TrackAppTab(const SidebarApp& app,
                              content::WebContents* contents) {
  app_tabs_[app.id] = contents->GetWeakPtr();
  // Keep it a normal tab of the window, but make the tab strip not draw it.
  SetHiddenAppTab(contents, true);
  // Show the site's favicon on the button once the page has one.
  if (tabs::TabInterface* tab = tabs::TabInterface::GetFromContents(contents)) {
    if (TabUIHelper* helper = TabUIHelper::From(tab)) {
      favicon_subscriptions_[app.id] = helper->AddTabUIChangeCallback(
          base::BindRepeating(&SidebarView::OnAppTabUiChanged,
                              base::Unretained(this), app.id));
      OnAppTabUiChanged(app.id);
    }
  }
  // Only the host window reports unread counts, so that two windows showing
  // the same app do not overwrite each other.
  if (is_app_host_) {
    title_observers_[app.id] = std::make_unique<TitleObserver>(
        contents,
        base::BindRepeating(&SidebarView::OnUnreadCountParsed,
                            base::Unretained(this), app.id));
  }
}

void SidebarView::OnAppTabUiChanged(const std::string& app_id) {
  // A custom icon always wins.
  if (custom_icons_.contains(app_id)) {
    return;
  }
  // Many sites draw an unread badge into their favicon, which would sit under
  // our own badge. While the app has unread items, keep the icon we already
  // have (it was taken when there were none).
  if (app_icons_.contains(app_id) && model_->GetUnreadCount(app_id) > 0) {
    return;
  }
  auto tab_it = app_tabs_.find(app_id);
  if (tab_it == app_tabs_.end() || !tab_it->second) {
    return;
  }
  tabs::TabInterface* tab =
      tabs::TabInterface::GetFromContents(tab_it->second.get());
  TabUIHelper* helper = tab ? TabUIHelper::From(tab) : nullptr;
  if (!helper) {
    return;
  }
  // Until the site's real icon arrives this is a generic placeholder, which
  // is not an image; keep showing the letter in that case.
  const ui::ImageModel favicon = helper->GetFavicon();
  if (!favicon.IsImage()) {
    return;
  }
  const gfx::ImageSkia icon = favicon.GetImage().AsImageSkia();
  if (icon.isNull()) {
    return;
  }
  auto cached = app_icons_.find(app_id);
  if (cached != app_icons_.end() && cached->second.BackedBySameObjectAs(icon)) {
    return;
  }
  app_icons_[app_id] = icon;
  auto entry = entries_.find(app_id);
  if (entry != entries_.end()) {
    entry->second->SetIcon(icon);
  }
}

void SidebarView::OnUnreadCountParsed(const std::string& app_id, int count) {
  model_->SetUnreadCount(app_id, count);
}

void SidebarView::ScheduleAppLaunch() {
  // Incognito windows never start the apps, and only one window per profile
  // does, so that a second window does not start a second copy of everything.
  if (!browser_ || profile_->IsOffTheRecord() || model_->apps().empty()) {
    return;
  }
  if (!is_app_host_) {
    is_app_host_ = model_->TryClaimAppHost(this);
    if (is_app_host_) {
      // Closing this window will now hide it to the tray.
      SidebarTray::Get().SetHostWindow(browser_->GetWeakPtr());
    }
  }
  if (!is_app_host_) {
    return;
  }
  launch_timer_.Start(FROM_HERE, kAppLaunchDelay,
                      base::BindOnce(&SidebarView::LaunchMissingApps,
                                     base::Unretained(this)));
}

void SidebarView::LaunchMissingApps() {
  if (!browser_) {
    return;
  }
  for (const SidebarApp& app : model_->apps()) {
    auto it = app_tabs_.find(app.id);
    if (it != app_tabs_.end() && it->second) {
      continue;  // Already has a tab in this window.
    }
    if (!AdoptExistingTab(app)) {
      OpenAppTab(app, /*foreground=*/false);
    }
  }
  UpdateActiveApp();
}

bool SidebarView::AdoptExistingTab(const SidebarApp& app) {
  TabStripModel* tab_strip = browser_->GetTabStripModel();
  const url::Origin app_origin = url::Origin::Create(app.url);
  for (int i = 0; i < tab_strip->count(); ++i) {
    content::WebContents* contents = tab_strip->GetWebContentsAt(i);
    if (!contents || IsHiddenAppTab(contents)) {
      continue;
    }
    if (url::Origin::Create(contents->GetVisibleURL()) != app_origin) {
      continue;
    }
    TrackAppTab(app, contents);
    // Tabs brought back by session restore are not loaded until they are
    // shown; load this one now so the app can start receiving notifications.
    contents->GetController().LoadIfNecessary();
    return true;
  }
  return false;
}

void SidebarView::UpdateActiveApp() {
  const bool was_app_shown = !active_app_id_.empty();
  active_app_id_.clear();
  if (browser_) {
    content::WebContents* active =
        browser_->GetTabStripModel()->GetActiveWebContents();
    for (const auto& [id, tab] : app_tabs_) {
      if (active && tab.get() == active) {
        active_app_id_ = id;
        break;
      }
    }
  }
  UpdateActiveIndicator();

  // The toolbar and the bookmark bar are hidden while an app is shown, so the
  // window has to lay itself out again whenever that changes.
  if (was_app_shown != !active_app_id_.empty() && parent()) {
    parent()->InvalidateLayout();
  }
}

void SidebarView::UpdateActiveIndicator() {
  for (const auto& [id, entry] : entries_) {
    entry->SetActive(id == active_app_id_);
  }
}

base::FilePath SidebarView::CustomIconPath(const std::string& app_id) const {
  if (!IsSafeFileName(app_id)) {
    return base::FilePath();
  }
  return profile_->GetPath()
      .AppendASCII("SidebarIcons")
      .AppendASCII(app_id + ".png");
}

void SidebarView::SetCustomIconFromFile(const std::string& app_id,
                                        const base::FilePath& source) {
  const base::FilePath dest = CustomIconPath(app_id);
  if (dest.empty()) {
    return;
  }
  IconTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ImportIconFile, source, dest),
      base::BindOnce(&SidebarView::OnCustomIconLoaded,
                     weak_factory_.GetWeakPtr(), app_id,
                     /*report_failure=*/true));
}

void SidebarView::LoadCustomIcon(const std::string& app_id) {
  const base::FilePath path = CustomIconPath(app_id);
  if (path.empty()) {
    return;
  }
  IconTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&DecodeIconFile, path),
      base::BindOnce(&SidebarView::OnCustomIconLoaded,
                     weak_factory_.GetWeakPtr(), app_id,
                     /*report_failure=*/false));
}

void SidebarView::OnCustomIconLoaded(const std::string& app_id,
                                     bool report_failure,
                                     const SkBitmap& bitmap) {
  if (bitmap.isNull()) {
    // No custom icon saved for this app is the normal case.
    LOG_IF(WARNING, report_failure)
        << "Could not use the chosen icon file for sidebar app " << app_id
        << " (it must be a PNG of at most 5 MB).";
    return;
  }
  if (!model_->FindApp(app_id)) {
    return;
  }
  const gfx::ImageSkia icon = gfx::ImageSkia::CreateFrom1xBitmap(bitmap);
  custom_icons_[app_id] = icon;
  auto entry = entries_.find(app_id);
  if (entry != entries_.end()) {
    entry->second->SetIcon(icon);
  }
}

void SidebarView::RemoveCustomIcon(const std::string& app_id) {
  custom_icons_.erase(app_id);
  const base::FilePath path = CustomIconPath(app_id);
  if (!path.empty()) {
    IconTaskRunner()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](const base::FilePath& file) {
                         base::DeleteFile(file);
                       },
                       path));
  }
  // Back to the site's own icon (or the letter until it has one).
  app_icons_.erase(app_id);
  auto entry = entries_.find(app_id);
  if (entry != entries_.end()) {
    entry->second->ClearIcon();
  }
  OnAppTabUiChanged(app_id);
}

BEGIN_METADATA(SidebarView)
END_METADATA

}  // namespace sidebar_apps
