// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_SYSTEM_UNIFIED_UNIFIED_SYSTEM_TRAY_VIEW_H_
#define ASH_SYSTEM_UNIFIED_UNIFIED_SYSTEM_TRAY_VIEW_H_

#include "ash/ash_export.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"

namespace ash {

class FeaturePodButton;
class FeaturePodsContainerView;
class TopShortcutsView;
class NotificationHiddenView;
class UnifiedMessageCenterView;
class UnifiedSystemInfoView;
class UnifiedSystemTrayController;

// Container view of slider views. If SetExpandedAmount() is called with 1.0,
// the behavior is same as vertiacal BoxLayout, but otherwise it shows
// intermediate state during animation.
class UnifiedSlidersContainerView : public views::View {
 public:
  explicit UnifiedSlidersContainerView(bool initially_expanded);
  ~UnifiedSlidersContainerView() override;

  // Change the expanded state. 0.0 if collapsed, and 1.0 if expanded.
  // Otherwise, it shows intermediate state.
  void SetExpandedAmount(double expanded_amount);

  // Get height of the view when |expanded_amount| is set to 1.0.
  int GetExpandedHeight() const;

  // Update opacity of each child slider views based on |expanded_amount_|.
  void UpdateOpacity();

  // views::View:
  void Layout() override;
  gfx::Size CalculatePreferredSize() const override;

 private:
  double expanded_amount_;

  DISALLOW_COPY_AND_ASSIGN(UnifiedSlidersContainerView);
};

// View class of the main bubble in UnifiedSystemTray.
//
// The UnifiedSystemTray contains two sub components:
//    1. MessageCenter: contains the list of notifications
//    2. SystemTray: contains quick settings controls
// Note that the term "UnifiedSystemTray" refers to entire bubble containing
// both (1) and (2).
class ASH_EXPORT UnifiedSystemTrayView : public views::View,
                                         public views::FocusTraversable {
 public:
  UnifiedSystemTrayView(UnifiedSystemTrayController* controller,
                        bool initially_expanded);
  ~UnifiedSystemTrayView() override;

  // Set the maximum height that the view can take.
  void SetMaxHeight(int max_height);

  // Add feature pod button to |feature_pods_|.
  void AddFeaturePodButton(FeaturePodButton* button);

  // Add slider view.
  void AddSliderView(views::View* slider_view);

  // Hide the main view and show the given |detailed_view|.
  void SetDetailedView(views::View* detailed_view);

  // Remove the detailed view set by SetDetailedView, and show the main view.
  // It deletes |detailed_view| and children.
  void ResetDetailedView();

  // Save and restore keyboard focus of feature pod.
  void SaveFeaturePodFocus();
  void RestoreFeaturePodFocus();

  // Change the expanded state. 0.0 if collapsed, and 1.0 if expanded.
  // Otherwise, it shows intermediate state.
  void SetExpandedAmount(double expanded_amount);

  // Get height of the system tray (excluding the message center) when
  // |expanded_amount| is set to 1.0.
  //
  // Note that this function is used to calculate the transform-based
  // collapse/expand animation, which is currently only enabled when there are
  // no notifications.
  int GetExpandedSystemTrayHeight() const;

  // Get height of the system menu (excluding the message center) when
  // |expanded_amount| is set to 0.0.
  int GetCollapsedSystemTrayHeight() const;

  // Get current height of the view (including the message center).
  int GetCurrentHeight() const;

  // Return true if layer transform can be used against the view. During
  // animation, the height of the view changes, but resizing of the bubble
  // is performance bottleneck. If this method returns true, the embedder can
  // call SetTransform() to move this view in order to avoid resizing.
  bool IsTransformEnabled() const;

  // Update the top of the SystemTray part to imitate notification list
  // scrolling under SystemTray. |rect_below_scroll| is the region of
  // notifications covered by SystemTray part, and its coordinate is relative to
  // UnifiedSystemTrayView. It can be empty.
  void SetNotificationRectBelowScroll(const gfx::Rect& rect_below_scroll);

  // Create background of UnifiedSystemTray that is semi-transparent and has
  // rounded corners.
  static std::unique_ptr<views::Background> CreateBackground();

  // views::View:
  void OnGestureEvent(ui::GestureEvent* event) override;
  void ChildPreferredSizeChanged(views::View* child) override;
  views::FocusTraversable* GetFocusTraversable() override;

  // views::FocusTraversable:
  views::FocusSearch* GetFocusSearch() override;
  views::FocusTraversable* GetFocusTraversableParent() override;
  views::View* GetFocusTraversableParentView() override;

  NotificationHiddenView* notification_hidden_view_for_testing() {
    return notification_hidden_view_;
  }

 private:
  class FocusSearch;

  double expanded_amount_;

  // Unowned.
  UnifiedSystemTrayController* const controller_;

  // Owned by views hierarchy.
  NotificationHiddenView* const notification_hidden_view_;
  TopShortcutsView* const top_shortcuts_view_;
  FeaturePodsContainerView* const feature_pods_container_;
  UnifiedSlidersContainerView* const sliders_container_;
  UnifiedSystemInfoView* const system_info_view_;
  views::View* const system_tray_container_;
  views::View* const detailed_view_container_;
  UnifiedMessageCenterView* const message_center_view_;

  // The maximum height available to the view.
  int max_height_ = 0;

  const std::unique_ptr<FocusSearch> focus_search_;
  const std::unique_ptr<ui::EventHandler> interacted_by_tap_recorder_;

  DISALLOW_COPY_AND_ASSIGN(UnifiedSystemTrayView);
};

}  // namespace ash

#endif  // ASH_SYSTEM_UNIFIED_UNIFIED_SYSTEM_TRAY_VIEW_H_
