// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/system/message_center/unified_message_center_view.h"

#include "ash/public/cpp/ash_features.h"
#include "ash/public/cpp/ash_pref_names.h"
#include "ash/session/session_controller.h"
#include "ash/shell.h"
#include "ash/system/message_center/ash_message_center_lock_screen_controller.h"
#include "ash/system/message_center/message_center_scroll_bar.h"
#include "ash/system/tray/tray_constants.h"
#include "ash/system/unified/unified_system_tray_controller.h"
#include "ash/system/unified/unified_system_tray_model.h"
#include "ash/test/ash_test_base.h"
#include "base/macros.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "components/prefs/pref_service.h"
#include "ui/message_center/message_center.h"
#include "ui/message_center/views/message_view.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/widget/widget.h"

using message_center::MessageCenter;
using message_center::MessageView;
using message_center::Notification;

namespace ash {

namespace {

constexpr int kDefaultTrayMenuWidth = 360;
constexpr int kDefaultMaxHeight = 500;

class DummyEvent : public ui::Event {
 public:
  DummyEvent() : Event(ui::ET_UNKNOWN, base::TimeTicks(), 0) {}
  ~DummyEvent() override = default;
};

class TestUnifiedMessageCenterView : public UnifiedMessageCenterView {
 public:
  explicit TestUnifiedMessageCenterView(UnifiedSystemTrayModel* model)
      : UnifiedMessageCenterView(nullptr, model) {}

  ~TestUnifiedMessageCenterView() override = default;

  void SetNotificationRectBelowScroll(
      const gfx::Rect& rect_below_scroll) override {
    rect_below_scroll_ = rect_below_scroll;
  }

  const gfx::Rect& rect_below_scroll() const { return rect_below_scroll_; }

 private:
  gfx::Rect rect_below_scroll_;

  DISALLOW_COPY_AND_ASSIGN(TestUnifiedMessageCenterView);
};

}  // namespace

class UnifiedMessageCenterViewTest : public AshTestBase,
                                     public views::ViewObserver {
 public:
  UnifiedMessageCenterViewTest() = default;
  ~UnifiedMessageCenterViewTest() override = default;

  // AshTestBase:
  void SetUp() override {
    AshTestBase::SetUp();
    model_ = std::make_unique<UnifiedSystemTrayModel>();
  }

  void TearDown() override {
    message_center_view_.reset();
    model_.reset();
    AshTestBase::TearDown();
  }

  // views::ViewObserver:
  void OnViewPreferredSizeChanged(views::View* view) override {
    if (view->GetPreferredSize() == view->size())
      return;
    view->SetBoundsRect(view->visible() ? gfx::Rect(view->GetPreferredSize())
                                        : gfx::Rect());
    view->Layout();
    ++size_changed_count_;
  }

 protected:
  std::string AddNotification() {
    std::string id = base::NumberToString(id_++);
    MessageCenter::Get()->AddNotification(std::make_unique<Notification>(
        message_center::NOTIFICATION_TYPE_BASE_FORMAT, id,
        base::UTF8ToUTF16("test title"), base::UTF8ToUTF16("test message"),
        gfx::Image(), base::string16() /* display_source */, GURL(),
        message_center::NotifierId(), message_center::RichNotificationData(),
        new message_center::NotificationDelegate()));
    return id;
  }

  void CreateMessageCenterView(int max_height = kDefaultMaxHeight) {
    message_center_view_ =
        std::make_unique<TestUnifiedMessageCenterView>(model_.get());
    message_center_view_->AddObserver(this);
    message_center_view_->SetMaxHeight(max_height);
    message_center_view_->SetAvailableHeight(max_height);
    message_center_view_->set_owned_by_client();
    OnViewPreferredSizeChanged(message_center_view_.get());
    size_changed_count_ = 0;
  }

  void AnimateToMiddle() {
    GetMessageListView()->animation_->SetCurrentValue(0.5);
    GetMessageListView()->AnimationProgressed(
        GetMessageListView()->animation_.get());
  }

  void AnimateToEnd() { GetMessageListView()->animation_->End(); }

  void AnimateUntilIdle() {
    while (GetMessageListView()->animation_->is_animating())
      GetMessageListView()->animation_->End();
  }

  gfx::Rect GetMessageViewVisibleBounds(int index) {
    gfx::Rect bounds = GetMessageListView()->child_at(index)->bounds();
    bounds -= GetScroller()->GetVisibleRect().OffsetFromOrigin();
    bounds += GetScroller()->bounds().OffsetFromOrigin();
    return bounds;
  }

  UnifiedMessageListView* GetMessageListView() {
    return message_center_view()->message_list_view_;
  }

  views::ScrollView* GetScroller() { return message_center_view()->scroller_; }

  MessageCenterScrollBar* GetScrollBar() {
    return message_center_view()->scroll_bar_;
  }

  views::View* GetScrollerContents() {
    return message_center_view()->scroller_->contents();
  }

  views::View* GetStackingCounter() {
    return message_center_view()->stacking_counter_;
  }

  views::View* GetStackingCounterLabel() {
    return message_center_view()->stacking_counter_->count_label_;
  }

  views::View* GetStackingCounterClearAllButton() {
    return message_center_view()->stacking_counter_->clear_all_button_;
  }

  message_center::MessageView* ToggleFocusToMessageView(int index,
                                                        bool reverse) {
    auto* focus_manager = message_center_view()->GetFocusManager();
    if (!focus_manager)
      return nullptr;

    message_center::MessageView* focused_message_view = nullptr;
    const int max_focus_toggles = 5 * GetMessageListView()->child_count();
    for (int i = 0; i < max_focus_toggles; ++i) {
      focus_manager->AdvanceFocus(reverse);
      auto* focused_view = focus_manager->GetFocusedView();
      // The MessageView is wrapped in container view in the MessageList.
      if (focused_view->parent() == GetMessageListView()->child_at(index)) {
        focused_message_view =
            static_cast<message_center::MessageView*>(focused_view);
        break;
      }
    }
    return focused_message_view;
  }

  void EnableNotificationStackingBarRedesign() {
    scoped_feature_list_.InitAndEnableFeature(
        features::kNotificationStackingBarRedesign);
  }

  TestUnifiedMessageCenterView* message_center_view() {
    return message_center_view_.get();
  }

  int size_changed_count() const { return size_changed_count_; }

  UnifiedSystemTrayModel* model() { return model_.get(); }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
  int id_ = 0;
  int size_changed_count_ = 0;

  std::unique_ptr<UnifiedSystemTrayModel> model_;
  std::unique_ptr<TestUnifiedMessageCenterView> message_center_view_;

  DISALLOW_COPY_AND_ASSIGN(UnifiedMessageCenterViewTest);
};

TEST_F(UnifiedMessageCenterViewTest, AddAndRemoveNotification) {
  CreateMessageCenterView();
  EXPECT_FALSE(message_center_view()->visible());

  auto id0 = AddNotification();
  EXPECT_TRUE(message_center_view()->visible());
  EXPECT_EQ(3 * kUnifiedNotificationCenterSpacing,
            GetScrollerContents()->height() -
                GetScroller()->GetVisibleRect().bottom());

  MessageCenter::Get()->RemoveNotification(id0, true /* by_user */);
  AnimateToEnd();
  AnimateToMiddle();
  EXPECT_TRUE(message_center_view()->visible());
  AnimateToEnd();
  EXPECT_FALSE(message_center_view()->visible());
}

TEST_F(UnifiedMessageCenterViewTest, ContentsRelayout) {
  std::vector<std::string> ids;
  for (size_t i = 0; i < 10; ++i)
    ids.push_back(AddNotification());
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());
  // MessageCenterView is maxed out.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());
  const int previous_contents_height = GetScrollerContents()->height();
  const int previous_list_height = GetMessageListView()->height();

  MessageCenter::Get()->RemoveNotification(ids.back(), true /* by_user */);
  AnimateUntilIdle();
  EXPECT_TRUE(message_center_view()->visible());
  EXPECT_GT(previous_contents_height, GetScrollerContents()->height());
  EXPECT_GT(previous_list_height, GetMessageListView()->height());
}

TEST_F(UnifiedMessageCenterViewTest, InsufficientHeight) {
  CreateMessageCenterView();
  AddNotification();
  EXPECT_TRUE(message_center_view()->visible());

  message_center_view()->SetAvailableHeight(kUnifiedNotificationMinimumHeight -
                                            1);
  EXPECT_FALSE(message_center_view()->visible());

  message_center_view()->SetAvailableHeight(kUnifiedNotificationMinimumHeight);
  EXPECT_TRUE(message_center_view()->visible());
}

TEST_F(UnifiedMessageCenterViewTest, NotVisibleWhenLocked) {
  // Disable the lock screen notification if the feature is enable.
  PrefService* user_prefs =
      Shell::Get()->session_controller()->GetLastActiveUserPrefService();
  user_prefs->SetString(prefs::kMessageCenterLockScreenMode,
                        prefs::kMessageCenterLockScreenModeHide);

  ASSERT_FALSE(AshMessageCenterLockScreenController::IsEnabled());

  AddNotification();
  AddNotification();

  BlockUserSession(BLOCKED_BY_LOCK_SCREEN);
  CreateMessageCenterView();

  EXPECT_FALSE(message_center_view()->visible());
}

TEST_F(UnifiedMessageCenterViewTest, VisibleWhenLocked) {
  // This test is only valid if the lock screen feature is enabled.
  // TODO(yoshiki): Clean up after the feature is launched crbug.com/913764.
  if (!features::IsLockScreenNotificationsEnabled())
    return;

  // Enables the lock screen notification if the feature is disabled.
  PrefService* user_prefs =
      Shell::Get()->session_controller()->GetLastActiveUserPrefService();
  user_prefs->SetString(prefs::kMessageCenterLockScreenMode,
                        prefs::kMessageCenterLockScreenModeShow);

  ASSERT_TRUE(AshMessageCenterLockScreenController::IsEnabled());

  AddNotification();
  AddNotification();

  BlockUserSession(BLOCKED_BY_LOCK_SCREEN);
  CreateMessageCenterView();

  EXPECT_TRUE(message_center_view()->visible());
}

TEST_F(UnifiedMessageCenterViewTest, ClearAllPressed) {
  AddNotification();
  AddNotification();
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  // ScrollView fills MessageCenterView.
  EXPECT_EQ(message_center_view()->bounds(), GetScroller()->bounds());
  EXPECT_EQ(GetMessageListView()->GetPreferredSize().width(),
            message_center_view()->GetPreferredSize().width());

  // MessageCenterView returns smaller height to hide Clear All button.
  EXPECT_EQ(kUnifiedNotificationCenterSpacing,
            message_center_view()->GetPreferredSize().height() -
                GetMessageListView()->GetPreferredSize().height());

  // ScrollView has larger height than MessageListView because it has Clear All
  // button.
  EXPECT_EQ(4 * kUnifiedNotificationCenterSpacing,
            GetScrollerContents()->GetPreferredSize().height() -
                GetMessageListView()->GetPreferredSize().height());

  // When Clear All button is pressed, all notifications are removed and the
  // view becomes invisible.
  message_center_view()->ButtonPressed(nullptr, DummyEvent());
  AnimateUntilIdle();
  EXPECT_FALSE(message_center_view()->visible());
}

TEST_F(UnifiedMessageCenterViewTest, InitialPosition) {
  AddNotification();
  AddNotification();
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  // MessageCenterView is not maxed out.
  EXPECT_LT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());

  EXPECT_EQ(kUnifiedNotificationCenterSpacing,
            message_center_view()->bounds().bottom() -
                GetMessageViewVisibleBounds(1).bottom());
}

TEST_F(UnifiedMessageCenterViewTest, InitialPositionMaxOut) {
  for (size_t i = 0; i < 6; ++i)
    AddNotification();
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  // MessageCenterView is maxed out.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());

  EXPECT_EQ(kUnifiedNotificationCenterSpacing,
            message_center_view()->bounds().bottom() -
                GetMessageViewVisibleBounds(5).bottom());
}

TEST_F(UnifiedMessageCenterViewTest, InitialPositionWithLargeNotification) {
  AddNotification();
  AddNotification();
  CreateMessageCenterView(100 /* max_height */);
  EXPECT_TRUE(message_center_view()->visible());

  // MessageCenterView is shorter than the notification.
  gfx::Rect message_view_bounds = GetMessageViewVisibleBounds(1);
  EXPECT_LT(message_center_view()->bounds().height(),
            message_view_bounds.height());

  // Top of the second notification aligns with the top of MessageCenterView.
  EXPECT_EQ(kStackingNotificationCounterHeight, message_view_bounds.y());
}

TEST_F(UnifiedMessageCenterViewTest, ScrollPositionWhenResized) {
  for (size_t i = 0; i < 6; ++i)
    AddNotification();
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  // MessageCenterView is maxed out.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());
  gfx::Rect previous_visible_rect = GetScroller()->GetVisibleRect();

  gfx::Size new_size = message_center_view()->size();
  new_size.set_height(250);
  message_center_view()->SetPreferredSize(new_size);
  OnViewPreferredSizeChanged(message_center_view());

  EXPECT_EQ(previous_visible_rect.bottom(),
            GetScroller()->GetVisibleRect().bottom());

  GetScroller()->ScrollToPosition(GetScrollBar(), 200);
  message_center_view()->OnMessageCenterScrolled();
  previous_visible_rect = GetScroller()->GetVisibleRect();

  new_size.set_height(300);
  message_center_view()->SetPreferredSize(new_size);
  OnViewPreferredSizeChanged(message_center_view());

  EXPECT_EQ(previous_visible_rect.bottom(),
            GetScroller()->GetVisibleRect().bottom());
}

TEST_F(UnifiedMessageCenterViewTest, StackingCounterLayout) {
  for (size_t i = 0; i < 6; ++i)
    AddNotification();
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  // MessageCenterView is maxed out.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());

  EXPECT_TRUE(GetStackingCounter()->visible());
  EXPECT_EQ(0, GetStackingCounter()->bounds().y());
  EXPECT_EQ(GetStackingCounter()->bounds().bottom(),
            GetScroller()->bounds().y());

  // Scroll to the top, making the counter invisbile.
  GetScroller()->ScrollToPosition(GetScrollBar(), 0);
  message_center_view()->OnMessageCenterScrolled();

  EXPECT_FALSE(GetStackingCounter()->visible());
  EXPECT_EQ(0, GetScroller()->bounds().y());
}

TEST_F(UnifiedMessageCenterViewTest,
       StackingCounterNotAffectingMessageViewBounds) {
  for (size_t i = 0; i < 6; ++i)
    AddNotification();
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  // MessageCenterView is maxed out.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());

  // Scroll to the top, making the counter invisbile.
  GetScroller()->ScrollToPosition(GetScrollBar(), 0);
  message_center_view()->OnMessageCenterScrolled();
  EXPECT_FALSE(GetStackingCounter()->visible());

  gfx::Rect previous_bounds = GetMessageViewVisibleBounds(2);

  const int scroll_amount = GetMessageViewVisibleBounds(0).height() -
                            kStackingNotificationCounterHeight + 1;
  GetScroller()->ScrollToPosition(GetScrollBar(), scroll_amount);
  message_center_view()->OnMessageCenterScrolled();

  EXPECT_TRUE(GetStackingCounter()->visible());
  // The offset change matches with the scroll amount plus the stacking bar
  // height.
  EXPECT_EQ(
      previous_bounds -
          gfx::Vector2d(0, scroll_amount + kStackingNotificationCounterHeight),
      GetMessageViewVisibleBounds(2));

  GetScroller()->ScrollToPosition(GetScrollBar(), scroll_amount - 1);
  message_center_view()->OnMessageCenterScrolled();
  EXPECT_FALSE(GetStackingCounter()->visible());
}

TEST_F(UnifiedMessageCenterViewTest, StackingCounterRemovedWithNotifications) {
  std::vector<std::string> ids;
  for (size_t i = 0; i < 6; ++i)
    ids.push_back(AddNotification());
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  // MessageCenterView is maxed out.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());

  EXPECT_TRUE(GetStackingCounter()->visible());
  for (size_t i = 0; i < 5; ++i) {
    MessageCenter::Get()->RemoveNotification(ids[i], true /* by_user */);
    AnimateUntilIdle();
  }
  EXPECT_FALSE(GetStackingCounter()->visible());
}

TEST_F(UnifiedMessageCenterViewTest, RedesignedStackingCounterLayout) {
  EnableNotificationStackingBarRedesign();

  for (size_t i = 0; i < 6; ++i)
    AddNotification();

  // MessageCenterView is maxed out.
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());

  EXPECT_TRUE(GetStackingCounter()->visible());
  EXPECT_EQ(0, GetStackingCounter()->bounds().y());
  EXPECT_EQ(GetStackingCounter()->bounds().bottom(),
            GetScroller()->bounds().y());
  EXPECT_TRUE(GetStackingCounterLabel()->visible());
  EXPECT_TRUE(GetStackingCounterClearAllButton()->visible());

  // Scroll to the top, making the counter label invisible.
  GetScroller()->ScrollToPosition(GetScrollBar(), 0);
  message_center_view()->OnMessageCenterScrolled();
  EXPECT_TRUE(GetStackingCounter()->visible());
  EXPECT_FALSE(GetStackingCounterLabel()->visible());
  EXPECT_TRUE(GetStackingCounterClearAllButton()->visible());
}

TEST_F(UnifiedMessageCenterViewTest,
       RedesignedStackingCounterMessageListScrolled) {
  EnableNotificationStackingBarRedesign();

  for (size_t i = 0; i < 6; ++i)
    AddNotification();
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());
  EXPECT_TRUE(GetStackingCounterLabel()->visible());
  EXPECT_TRUE(GetStackingCounterClearAllButton()->visible());

  // MessageCenterView is maxed out.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());

  // Scroll to the top, making the counter label invisible.
  GetScroller()->ScrollToPosition(GetScrollBar(), 0);
  message_center_view()->OnMessageCenterScrolled();
  EXPECT_TRUE(GetStackingCounter()->visible());
  EXPECT_FALSE(GetStackingCounterLabel()->visible());
  EXPECT_TRUE(GetStackingCounterClearAllButton()->visible());

  gfx::Rect previous_bounds = GetMessageViewVisibleBounds(2);

  // Scrolling past a notification should make the counter label visible.
  const int scroll_amount = GetMessageViewVisibleBounds(0).height() + 1;
  GetScroller()->ScrollToPosition(GetScrollBar(), scroll_amount);
  message_center_view()->OnMessageCenterScrolled();

  EXPECT_TRUE(GetStackingCounterLabel()->visible());
  // The offset change matches with the scroll amount.
  EXPECT_EQ(previous_bounds - gfx::Vector2d(0, scroll_amount),
            GetMessageViewVisibleBounds(2));

  // Scrolling back a tiny bit to reveal the notification should make the
  // counter label invisible again.
  GetScroller()->ScrollToPosition(GetScrollBar(), scroll_amount - 2);
  message_center_view()->OnMessageCenterScrolled();
  EXPECT_TRUE(GetStackingCounter()->visible());
  EXPECT_FALSE(GetStackingCounterLabel()->visible());
  EXPECT_TRUE(GetStackingCounterClearAllButton()->visible());
}

TEST_F(UnifiedMessageCenterViewTest,
       RedesignedStackingCounterNotificationRemoval) {
  EnableNotificationStackingBarRedesign();

  std::vector<std::string> ids;
  for (size_t i = 0; i < 6; ++i)
    ids.push_back(AddNotification());
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  // MessageCenterView is maxed out.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());

  // Dismiss until there are 2 notifications. The bar should still be visible.
  EXPECT_TRUE(GetStackingCounter()->visible());
  for (size_t i = 0; i < 4; ++i) {
    MessageCenter::Get()->RemoveNotification(ids[i], true /* by_user */);
    AnimateUntilIdle();
  }
  EXPECT_TRUE(GetStackingCounter()->visible());
  EXPECT_FALSE(GetStackingCounterLabel()->visible());
  EXPECT_TRUE(GetStackingCounterClearAllButton()->visible());

  // Dismiss until there is only 1 notification left. The bar should be
  // invisible.
  MessageCenter::Get()->RemoveNotification(ids[4], true /* by_user */);
  AnimateUntilIdle();
  EXPECT_FALSE(GetStackingCounter()->visible());
}

TEST_F(UnifiedMessageCenterViewTest, RectBelowScroll) {
  for (size_t i = 0; i < 6; ++i)
    AddNotification();
  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  // MessageCenterView is maxed out.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());
  message_center_view()->OnMessageCenterScrolled();

  EXPECT_EQ(0, message_center_view()->rect_below_scroll().height());

  GetScroller()->ScrollToPosition(GetScrollBar(), 0);
  message_center_view()->OnMessageCenterScrolled();
  EXPECT_LT(0, message_center_view()->rect_below_scroll().height());
}

TEST_F(UnifiedMessageCenterViewTest,
       RectBelowScrollWithTargetingFirstNotification) {
  std::vector<std::string> ids;
  for (size_t i = 0; i < 10; ++i)
    ids.push_back(AddNotification());
  // Set the first notification as the target.
  model()->SetTargetNotification(ids[0]);

  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());
  message_center_view()->OnMessageCenterScrolled();

  EXPECT_EQ(0, GetScroller()->GetVisibleRect().y());
  EXPECT_EQ(
      GetMessageListView()->height() - GetScroller()->GetVisibleRect().height(),
      message_center_view()->rect_below_scroll().height());
}

TEST_F(UnifiedMessageCenterViewTest, RectBelowScrollWithTargetingNotification) {
  std::vector<std::string> ids;
  for (size_t i = 0; i < 10; ++i)
    ids.push_back(AddNotification());
  // Set the second last notification as the target.
  model()->SetTargetNotification(ids[8]);

  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());
  message_center_view()->OnMessageCenterScrolled();

  EXPECT_EQ(GetMessageListView()->GetLastNotificationBounds().height(),
            message_center_view()->rect_below_scroll().height());
}

TEST_F(UnifiedMessageCenterViewTest,
       RectBelowScrollWithTargetingLastNotification) {
  std::vector<std::string> ids;
  for (size_t i = 0; i < 10; ++i)
    ids.push_back(AddNotification());
  // Set the second last notification as the target.
  model()->SetTargetNotification(ids[9]);

  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());
  message_center_view()->OnMessageCenterScrolled();

  EXPECT_EQ(0, message_center_view()->rect_below_scroll().height());
}

TEST_F(UnifiedMessageCenterViewTest,
       RectBelowScrollWithTargetingInvalidNotification) {
  std::vector<std::string> ids;
  for (size_t i = 0; i < 10; ++i)
    ids.push_back(AddNotification());
  // Set the second last notification as the target.
  model()->SetTargetNotification("INVALID_ID");

  CreateMessageCenterView();
  EXPECT_TRUE(message_center_view()->visible());

  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());
  message_center_view()->OnMessageCenterScrolled();

  EXPECT_EQ(0, message_center_view()->rect_below_scroll().height());
}

TEST_F(UnifiedMessageCenterViewTest, FocusClearedAfterNotificationRemoval) {
  CreateMessageCenterView();

  // We need to create a widget in order to initialize a FocusManager.
  auto widget = CreateTestWidget();
  widget->GetRootView()->AddChildView(message_center_view());
  widget->Show();

  // Add notifications and focus on a child view in the last notification.
  AddNotification();
  auto id1 = AddNotification();

  // Toggle focus to the last notification MessageView.
  auto* focused_message_view =
      ToggleFocusToMessageView(1 /* index */, true /* reverse */);
  ASSERT_TRUE(focused_message_view);
  EXPECT_EQ(id1, focused_message_view->notification_id());

  // Remove the notification and observe that the focus is cleared.
  MessageCenter::Get()->RemoveNotification(id1, true /* by_user */);
  AnimateUntilIdle();
  EXPECT_FALSE(message_center_view()->GetFocusManager()->GetFocusedView());

  widget->GetRootView()->RemoveChildView(message_center_view());
}

TEST_F(UnifiedMessageCenterViewTest, FocusChangeUpdatesStackingBar) {
  CreateMessageCenterView();

  // We need to create a widget in order to initialize a FocusManager.
  auto widget = CreateTestWidget();
  widget->GetRootView()->AddChildView(message_center_view());
  widget->SetSize(gfx::Size(kDefaultTrayMenuWidth, kDefaultMaxHeight));
  widget->Show();

  // Add notifications such that the stacking counter is shown.
  std::string first_notification_id = AddNotification();
  for (int i = 0; i < 6; ++i)
    AddNotification();
  std::string last_notification_id = AddNotification();

  // The ListView should be taller than the MessageCenterView so we can scroll
  // and show the stacking counter.
  EXPECT_GT(GetMessageListView()->bounds().height(),
            message_center_view()->bounds().height());
  EXPECT_TRUE(GetStackingCounter()->visible());

  // Advancing focus causes list to scroll to the top, which hides the counter.
  auto* message_view =
      ToggleFocusToMessageView(0 /* index */, false /* reverse */);
  EXPECT_EQ(first_notification_id, message_view->notification_id());
  EXPECT_FALSE(GetStackingCounter()->visible());

  // Reversing the focus more scrolls the list to the bottom, reshowing the
  // counter.
  message_view = ToggleFocusToMessageView(7 /* index */, false /* reverse */);
  EXPECT_EQ(last_notification_id, message_view->notification_id());
  EXPECT_TRUE(GetStackingCounter()->visible());
}

}  // namespace ash
