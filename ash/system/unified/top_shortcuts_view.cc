// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/system/unified/top_shortcuts_view.h"

#include "ash/accessibility/accessibility_controller.h"
#include "ash/public/cpp/ash_view_ids.h"
#include "ash/resources/vector_icons/vector_icons.h"
#include "ash/session/session_controller.h"
#include "ash/shell.h"
#include "ash/shutdown_controller.h"
#include "ash/strings/grit/ash_strings.h"
#include "ash/system/tray/tray_constants.h"
#include "ash/system/tray/tray_popup_utils.h"
#include "ash/system/unified/collapse_button.h"
#include "ash/system/unified/sign_out_button.h"
#include "ash/system/unified/top_shortcut_button.h"
#include "ash/system/unified/unified_system_tray_controller.h"
#include "ash/system/unified/user_chooser_view.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/view_class_properties.h"
namespace ash {

namespace {

class UserAvatarButton : public views::Button {
 public:
  UserAvatarButton(views::ButtonListener* listener);
  ~UserAvatarButton() override = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(UserAvatarButton);
};

UserAvatarButton::UserAvatarButton(views::ButtonListener* listener)
    : Button(listener) {
  SetLayoutManager(std::make_unique<views::FillLayout>());
  SetBorder(views::CreateEmptyBorder(kUnifiedCircularButtonFocusPadding));
  AddChildView(CreateUserAvatarView(0 /* user_index */));

  SetTooltipText(GetUserItemAccessibleString(0 /* user_index */));
  SetInstallFocusRingOnFocus(true);
  SetFocusForPlatform();

  int focus_ring_radius =
      kTrayItemSize + kUnifiedCircularButtonFocusPadding.width();
  auto path = std::make_unique<SkPath>();
  path->addOval(gfx::RectToSkRect(
      gfx::Rect(gfx::Size(focus_ring_radius, focus_ring_radius))));
  SetProperty(views::kHighlightPathKey, path.release());
}

}  // namespace

TopShortcutButtonContainer::TopShortcutButtonContainer() = default;

TopShortcutButtonContainer::~TopShortcutButtonContainer() = default;

// Buttons are equally spaced by the default value, but the gap will be
// narrowed evenly when the parent view is not large enough.
void TopShortcutButtonContainer::Layout() {
  gfx::Rect child_area(GetContentsBounds());

  int total_horizontal_size = 0;
  int num_visible = 0;
  for (int i = 0; i < child_count(); i++) {
    const views::View* child = child_at(i);
    if (!child->visible())
      continue;
    int child_horizontal_size = child->GetPreferredSize().width();
    if (child_horizontal_size == 0)
      continue;
    total_horizontal_size += child_horizontal_size;
    num_visible++;
  }

  int spacing = 0;
  if (num_visible > 1) {
    spacing = std::max(kUnifiedTopShortcutButtonMinSpacing,
                         std::min(kUnifiedTopShortcutButtonDefaultSpacing,
                                  (child_area.width() - total_horizontal_size) /
                                      (num_visible - 1)));
  }

  int sign_out_button_width = 0;
  if (sign_out_button_ && sign_out_button_->visible()) {
    // resize the sign-out button
    int remainder = child_area.width() -
                    (num_visible - 1) * kUnifiedTopShortcutButtonMinSpacing -
                    total_horizontal_size +
                    sign_out_button_->GetPreferredSize().width();
    sign_out_button_width = std::max(
        0, std::min(sign_out_button_->GetPreferredSize().width(), remainder));
  }

  int horizontal_position = child_area.x();

  if (user_avatar_button_ && user_avatar_button_->visible()) {
    int vertical_position =
        child_area.y() + kUnifiedTopShortcutContainerTopPadding;
    horizontal_position -= kUnifiedCircularButtonFocusPadding.left();

    gfx::Size size = user_avatar_button_->GetPreferredSize();
    gfx::Rect user_avatar_bounds(horizontal_position, vertical_position,
                                 size.width(), size.height());
    user_avatar_button_->SetBoundsRect(user_avatar_bounds);

    horizontal_position += user_avatar_bounds.size().width() + spacing -
                           kUnifiedCircularButtonFocusPadding.right();
  }

  int vertical_position = child_area.y() +
                          kUnifiedTopShortcutContainerTopPadding +
                          kUnifiedCircularButtonFocusPadding.bottom();
  for (int i = 1; i < child_count(); i++) {
    views::View* child = child_at(i);
    if (!child->visible())
      continue;
    gfx::Rect bounds(child_area);
    bounds.set_x(horizontal_position);
    bounds.set_y(vertical_position);

    int width = (child == sign_out_button_) ? sign_out_button_width
                                            : child->GetPreferredSize().width();
    bounds.set_width(width);
    bounds.set_height(child->GetHeightForWidth(width));
    child->SetBoundsRect(bounds);
    horizontal_position += width + spacing;
  }
}

gfx::Size TopShortcutButtonContainer::CalculatePreferredSize() const {
  int total_horizontal_size = 0;
  int num_visible = 0;
  for (int i = 0; i < child_count(); i++) {
    const views::View* child = child_at(i);
    if (!child->visible())
      continue;
    int child_horizontal_size = child->GetPreferredSize().width();
    if (child_horizontal_size == 0)
      continue;
    total_horizontal_size += child_horizontal_size;
    num_visible++;
  }
  int width =
      (num_visible == 0)
          ? 0
          : total_horizontal_size +
                (num_visible - 1) * kUnifiedTopShortcutButtonDefaultSpacing;
  int height = kTrayItemSize + kUnifiedCircularButtonFocusPadding.height() +
               kUnifiedTopShortcutContainerTopPadding;
  return gfx::Size(width, height);
}

void TopShortcutButtonContainer::AddUserAvatarButton(
    views::View* user_avatar_button) {
  AddChildView(user_avatar_button);
  user_avatar_button_ = user_avatar_button;
}

void TopShortcutButtonContainer::AddSignOutButton(
    views::View* sign_out_button) {
  AddChildView(sign_out_button);
  sign_out_button_ = sign_out_button;
}

TopShortcutsView::TopShortcutsView(UnifiedSystemTrayController* controller)
    : controller_(controller) {
  DCHECK(controller_);

  auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::kHorizontal, kUnifiedTopShortcutPadding,
      kUnifiedTopShortcutSpacing));
  layout->set_cross_axis_alignment(
      views::BoxLayout::CROSS_AXIS_ALIGNMENT_START);
  container_ = new TopShortcutButtonContainer();
  AddChildView(container_);

  if (Shell::Get()->session_controller()->login_status() !=
      LoginStatus::NOT_LOGGED_IN) {
    user_avatar_button_ = new UserAvatarButton(this);
    user_avatar_button_->SetEnabled(controller->IsUserChooserEnabled());
    container_->AddUserAvatarButton(user_avatar_button_);
  }

  // Show the buttons in this row as disabled if the user is at the login
  // screen, lock screen, or in a secondary account flow. The exception is
  // |power_button_| which is always shown as enabled.
  const bool can_show_web_ui = TrayPopupUtils::CanOpenWebUISettings();

  sign_out_button_ = new SignOutButton(this);
  container_->AddSignOutButton(sign_out_button_);

  bool reboot = Shell::Get()->shutdown_controller()->reboot_on_shutdown();
  power_button_ = new TopShortcutButton(
      this, kUnifiedMenuPowerIcon,
      reboot ? IDS_ASH_STATUS_TRAY_REBOOT : IDS_ASH_STATUS_TRAY_SHUTDOWN);
  power_button_->set_id(VIEW_ID_POWER_BUTTON);
  container_->AddChildView(power_button_);

  lock_button_ = new TopShortcutButton(this, kUnifiedMenuLockIcon,
                                       IDS_ASH_STATUS_TRAY_LOCK);
  lock_button_->SetVisible(can_show_web_ui &&
                           Shell::Get()->session_controller()->CanLockScreen());
  container_->AddChildView(lock_button_);

  settings_button_ = new TopShortcutButton(this, kUnifiedMenuSettingsIcon,
                                           IDS_ASH_STATUS_TRAY_SETTINGS);
  settings_button_->SetVisible(can_show_web_ui);
  container_->AddChildView(settings_button_);

  // |collapse_button_| should be right-aligned, so we make the buttons
  // container flex occupying all remaining space.
  layout->SetFlexForView(container_, 1);

  collapse_button_ = new CollapseButton(this);
  AddChildView(collapse_button_);

  OnAccessibilityStatusChanged();

  Shell::Get()->accessibility_controller()->AddObserver(this);
}

TopShortcutsView::~TopShortcutsView() {
  Shell::Get()->accessibility_controller()->RemoveObserver(this);
}

void TopShortcutsView::SetExpandedAmount(double expanded_amount) {
  collapse_button_->SetExpandedAmount(expanded_amount);
}

void TopShortcutsView::ButtonPressed(views::Button* sender,
                                     const ui::Event& event) {
  if (sender == user_avatar_button_)
    controller_->ShowUserChooserView();
  else if (sender == sign_out_button_)
    controller_->HandleSignOutAction();
  else if (sender == lock_button_)
    controller_->HandleLockAction();
  else if (sender == settings_button_)
    controller_->HandleSettingsAction();
  else if (sender == power_button_)
    controller_->HandlePowerAction();
  else if (sender == collapse_button_)
    controller_->ToggleExpanded();
}

void TopShortcutsView::OnAccessibilityStatusChanged() {
  collapse_button_->SetEnabled(
      !Shell::Get()->accessibility_controller()->spoken_feedback_enabled());
}

}  // namespace ash
