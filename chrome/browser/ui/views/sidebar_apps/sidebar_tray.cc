// Copyright 2026 MDJ. Use of this source code is governed by a BSD-style
// license that can be found in the Chromium LICENSE file.

#include "chrome/browser/ui/views/sidebar_apps/sidebar_tray.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/status_icons/status_icon.h"
#include "chrome/browser/status_icons/status_tray.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/sidebar_apps/sidebar_autostart.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkRect.h"
#include "ui/base/base_window.h"
#include "ui/gfx/image/image_skia.h"

namespace sidebar_apps {

namespace {

constexpr int kIconSize = 32;

constexpr int kCommandOpen = 1;
constexpr int kCommandAutostart = 2;
constexpr int kCommandQuit = 3;

// The switch the Windows startup entry passes: start with the window hidden.
constexpr char kStartInTraySwitch[] = "start-in-tray";
// How long after startup windows stay hidden when started in the tray.
constexpr base::TimeDelta kStartInTrayWindow = base::Seconds(20);

// Set when the user opened the window from the tray; ends the hidden start.
bool g_start_in_tray_ended = false;

// Start the browser with this switch to make the close button close the
// window normally (handy while developing, so a rebuild is never blocked by a
// browser that is still running in the tray).
constexpr char kDisableCloseToTraySwitch[] = "disable-close-to-tray";

// A 3x5 pixel font for the digits 0-9. Each row is 3 bits, top row first.
// Drawing the digits by hand keeps the tray icon independent of any font
// setup.
constexpr std::array<std::array<uint8_t, 5>, 10> kDigitRows{{
    {{0b111, 0b101, 0b101, 0b101, 0b111}},  // 0
    {{0b010, 0b110, 0b010, 0b010, 0b111}},  // 1
    {{0b111, 0b001, 0b111, 0b100, 0b111}},  // 2
    {{0b111, 0b001, 0b111, 0b001, 0b111}},  // 3
    {{0b101, 0b101, 0b111, 0b001, 0b001}},  // 4
    {{0b111, 0b100, 0b111, 0b001, 0b111}},  // 5
    {{0b111, 0b100, 0b111, 0b101, 0b111}},  // 6
    {{0b111, 0b001, 0b001, 0b001, 0b001}},  // 7
    {{0b111, 0b101, 0b111, 0b101, 0b111}},  // 8
    {{0b111, 0b101, 0b111, 0b001, 0b111}},  // 9
}};

// The notification dot in the top-right corner.
constexpr SkColor kDotColor = SkColorSetRGB(0xFF, 0x3B, 0x30);
constexpr float kDotRadius = 4.0f;

// A pure white check mark (nothing unread).
void DrawCheckMark(SkCanvas& canvas) {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeCap(SkPaint::kRound_Cap);
  paint.setStrokeWidth(5.0f);
  paint.setColor(SK_ColorWHITE);
  canvas.drawLine(7.0f, 17.0f, 13.0f, 23.0f, paint);
  canvas.drawLine(13.0f, 23.0f, 25.0f, 9.0f, paint);
}

// |total| (capped at 99) as a big white number, with a red dot in the
// top-right corner. The dot no longer sits beside the number, so the digits
// can stay large for one and two digits alike.
void DrawDotAndNumber(SkCanvas& canvas, int total) {
  const int shown = std::clamp(total, 1, 99);
  std::vector<int> digits;
  if (shown >= 10) {
    digits = {shown / 10, shown % 10};
  } else {
    digits = {shown};
  }
  const int count = static_cast<int>(digits.size());

  // One digit: scale 5 (15x25). Two digits: scale 4 with a 2px gap
  // (26x20). Scale 5 would need 35px for two digits, which doesn't fit.
  const int scale = count == 1 ? 5 : 4;
  const int digit_width = 3 * scale;
  const int digit_gap = count == 1 ? 0 : 2;
  const int text_width = count * digit_width + (count - 1) * digit_gap;
  const int text_height = 5 * scale;
  const int text_left = (kIconSize - text_width) / 2;
  // Pushed 3px below center so the text clears the corner dot
  // (the dot covers y = 1..9, x = 23..31).
  const int top = (kIconSize - text_height) / 2 + 3;

  SkPaint fill;
  fill.setColor(SK_ColorWHITE);
  for (int d = 0; d < count; ++d) {
    const int digit_left = text_left + d * (digit_width + digit_gap);
    for (int row = 0; row < 5; ++row) {
      for (int col = 0; col < 3; ++col) {
        if (!(kDigitRows[digits[d]][row] & (1 << (2 - col)))) {
          continue;
        }
        const float x = static_cast<float>(digit_left + col * scale);
        const float y = static_cast<float>(top + row * scale);
        const float s = static_cast<float>(scale);
        canvas.drawRect(SkRect::MakeXYWH(x, y, s, s), fill);
      }
    }
  }

  SkPaint dot;
  dot.setAntiAlias(true);
  dot.setColor(kDotColor);
  canvas.drawCircle(kIconSize - kDotRadius - 1.0f, kDotRadius + 1.0f,
                    kDotRadius, dot);
}

gfx::ImageSkia RenderTrayIcon(int total) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(kIconSize, kIconSize);
  bitmap.eraseColor(SK_ColorTRANSPARENT);
  SkCanvas canvas(bitmap);
  if (total > 0) {
    DrawDotAndNumber(canvas, total);
  } else {
    DrawCheckMark(canvas);
  }
  return gfx::ImageSkia::CreateFrom1xBitmap(bitmap);
}

}  // namespace

// static
SidebarTray& SidebarTray::Get() {
  static base::NoDestructor<SidebarTray> tray;
  return *tray;
}

// static
void SidebarTray::AttachToModel(SidebarAppModel* model) {
  Get().Attach(model);
}

// static
bool SidebarTray::ShouldHideInsteadOfClosing(BrowserWindowInterface* browser) {
  SidebarTray& tray = Get();
  if (!browser || tray.quitting_ || browser_shutdown::IsTryingToQuit()) {
    return false;
  }
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          kDisableCloseToTraySwitch)) {
    return false;
  }
  return tray.host_window_ && tray.host_window_.get() == browser;
}

// static
bool SidebarTray::ShouldSuppressShow() {
  if (g_start_in_tray_ended ||
      !base::CommandLine::ForCurrentProcess()->HasSwitch(kStartInTraySwitch)) {
    return false;
  }
  // The clock starts the first time a window asks to be shown, which is during
  // startup.
  static const base::TimeTicks deadline =
      base::TimeTicks::Now() + kStartInTrayWindow;
  return base::TimeTicks::Now() < deadline;
}

SidebarTray::SidebarTray() = default;
SidebarTray::~SidebarTray() = default;

void SidebarTray::SetLastActiveWindow(
    base::WeakPtr<BrowserWindowInterface> window) {
  last_active_window_ = std::move(window);
}

void SidebarTray::SetHostWindow(base::WeakPtr<BrowserWindowInterface> window) {
  host_window_ = std::move(window);
}

void SidebarTray::Attach(SidebarAppModel* model) {
  if (model_ || !model) {
    return;  // Already following a model.
  }
  model_ = model;
  model_->AddObserver(this);
  UpdateIcon();
}

void SidebarTray::OnSidebarAppsChanged() {}

void SidebarTray::OnUnreadCountsChanged() {
  UpdateIcon();
}

void SidebarTray::UpdateIcon() {
  if (!model_ || !g_browser_process || g_browser_process->IsShuttingDown()) {
    return;
  }
  StatusTray* tray = g_browser_process->status_tray();
  if (!tray) {
    return;
  }
  const int total = model_->GetTotalUnreadCount();
  const gfx::ImageSkia image = RenderTrayIcon(total);
  const std::u16string tool_tip =
      total > 0 ? u"Dockly: " + base::NumberToString16(total) + u" unread"
                : u"Dockly: nothing unread";
  if (!icon_) {
    icon_ = tray->CreateStatusIcon(StatusTray::OTHER_ICON, image, tool_tip);
    if (icon_) {
      icon_->AddObserver(this);
      CreateContextMenu();
    }
    return;
  }
  icon_->SetImage(image);
  icon_->SetToolTip(tool_tip);
}

void SidebarTray::CreateContextMenu() {
  if (!icon_) {
    return;
  }
  auto menu = std::make_unique<StatusIconMenuModel>(this);
  menu->AddItem(kCommandOpen, u"Open Dockly");
  menu->AddSeparator(ui::NORMAL_SEPARATOR);
  menu->AddCheckItem(kCommandAutostart, u"Start with Windows");
  menu->SetCommandIdChecked(kCommandAutostart, IsAutostartEnabled());
  menu->AddSeparator(ui::NORMAL_SEPARATOR);
  menu->AddItem(kCommandQuit, u"Quit Dockly");
  icon_->SetContextMenu(std::move(menu));
}

void SidebarTray::ExecuteCommand(int command_id, int event_flags) {
  switch (command_id) {
    case kCommandOpen:
      ShowWindow();
      break;
    case kCommandAutostart:
      SetAutostartEnabled(!IsAutostartEnabled());
      // Rebuild the menu once this one has finished running, so the check
      // box shows the new state. (The tray object lives for the whole
      // process.)
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&SidebarTray::CreateContextMenu,
                                    base::Unretained(this)));
      break;
    case kCommandQuit:
      // From here on closing a window really closes it.
      quitting_ = true;
      chrome::AttemptUserExit();
      break;
    default:
      break;
  }
}

void SidebarTray::OnStatusIconClicked() {
  ShowWindow();
}

void SidebarTray::ShowWindow() {
  // The user asked for the window: stop keeping it hidden.
  g_start_in_tray_ended = true;
  BrowserWindowInterface* browser =
      host_window_ ? host_window_.get() : last_active_window_.get();
  if (!browser) {
    return;
  }
  ui::BaseWindow* window = browser->GetWindow();
  if (!window) {
    return;
  }
  if (window->IsMinimized()) {
    window->Restore();
  }
  window->Show();
  window->Activate();
}

}  // namespace sidebar_apps
