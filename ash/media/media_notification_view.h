// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_MEDIA_MEDIA_NOTIFICATION_VIEW_H_
#define ASH_MEDIA_MEDIA_NOTIFICATION_VIEW_H_

#include "ash/ash_export.h"
#include "services/media_session/public/mojom/media_session.mojom.h"
#include "ui/message_center/views/message_view.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/label.h"

namespace gfx {
class ImageSkia;
}  // namespace gfx

namespace media_session {
struct MediaMetadata;
}  // namespace media_session

namespace media_session {
enum class MediaSessionAction;
}  // namespace media_session

namespace message_center {
class NotificationHeaderView;
}  // namespace message_center

namespace views {
class BoxLayout;
class ToggleImageButton;
class View;
}  // namespace views

namespace ash {

class MediaNotificationBackground;

// MediaNotificationView will show up as a custom notification. It will show the
// currently playing media and provide playback controls. There will also be
// control buttons (e.g. close) in the top right corner that will hide and show
// if the notification is hovered.
class ASH_EXPORT MediaNotificationView : public message_center::MessageView,
                                         public views::ButtonListener {
 public:
  // The name of the histogram used when recorded whether the artwork was
  // present.
  static const char kArtworkHistogramName[];

  // The name of the histogram used when recording the type of metadata that was
  // displayed.
  static const char kMetadataHistogramName[];

  // The type of metadata that was displayed. This is used in metrics so new
  // values must only be added to the end.
  enum class Metadata {
    kTitle,
    kArtist,
    kAlbum,
    kCount,
    kMaxValue = kCount,
  };

  explicit MediaNotificationView(
      const message_center::Notification& notification);
  ~MediaNotificationView() override;

  // message_center::MessageView:
  void UpdateWithNotification(
      const message_center::Notification& notification) override;
  message_center::NotificationControlButtonsView* GetControlButtonsView()
      const override;
  void SetExpanded(bool expanded) override;
  void UpdateCornerRadius(int top_radius, int bottom_radius) override;

  // views::View:
  void OnMouseEvent(ui::MouseEvent* event) override;

  // views::ButtonListener:
  void ButtonPressed(views::Button* sender, const ui::Event& event) override;

  void UpdateWithMediaSessionInfo(
      const media_session::mojom::MediaSessionInfoPtr& session_info);
  void UpdateWithMediaMetadata(const media_session::MediaMetadata& metadata);
  void UpdateWithMediaActions(
      const std::set<media_session::mojom::MediaSessionAction>& actions);
  void UpdateWithMediaArtwork(const gfx::ImageSkia& image);
  void UpdateWithMediaIcon(const gfx::ImageSkia& image);

 private:
  friend class MediaNotificationViewTest;

  void UpdateControlButtonsVisibilityWithNotification(
      const message_center::Notification& notification);

  // Creates an image button with |icon| and adds it to |button_row_|. When
  // clicked it will trigger |action| on the sesssion.
  void CreateMediaButton(const gfx::VectorIcon& icon,
                         media_session::mojom::MediaSessionAction action);

  void UpdateActionButtonsVisibility();
  void UpdateViewForExpandedState();

  MediaNotificationBackground* GetMediaNotificationBackground();

  bool IsExpandable() const;
  bool IsActuallyExpanded() const;

  std::set<media_session::mojom::MediaSessionAction> CalculateVisibleActions(
      bool expanded) const;

  // View containing close and settings buttons.
  std::unique_ptr<message_center::NotificationControlButtonsView>
      control_buttons_view_;

  bool has_artwork_ = false;

  // Whether this notification is expanded or not.
  bool expanded_ = false;

  // Set of enabled actions.
  std::set<media_session::mojom::MediaSessionAction> enabled_actions_;

  // Container views directly attached to this view.
  message_center::NotificationHeaderView* header_row_ = nullptr;
  views::View* button_row_ = nullptr;
  views::ToggleImageButton* play_pause_button_ = nullptr;
  views::View* title_artist_row_ = nullptr;
  views::Label* title_label_ = nullptr;
  views::Label* artist_label_ = nullptr;
  views::View* layout_row_ = nullptr;
  views::View* main_row_ = nullptr;

  views::BoxLayout* title_artist_row_layout_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(MediaNotificationView);
};

}  // namespace ash

#endif  // ASH_MEDIA_MEDIA_NOTIFICATION_VIEW_H_
