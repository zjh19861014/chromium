// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/app_list/views/app_list_view.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "ash/app_list/app_list_util.h"
#include "ash/app_list/model/app_list_model.h"
#include "ash/app_list/views/app_list_folder_view.h"
#include "ash/app_list/views/app_list_main_view.h"
#include "ash/app_list/views/apps_container_view.h"
#include "ash/app_list/views/contents_view.h"
#include "ash/app_list/views/search_box_view.h"
#include "ash/assistant/ui/assistant_ui_constants.h"
#include "ash/public/cpp/app_list/app_list_config.h"
#include "ash/public/cpp/app_list/app_list_features.h"
#include "ash/public/cpp/ash_features.h"
#include "ash/public/cpp/shell_window_ids.h"
#include "ash/public/cpp/wallpaper_types.h"
#include "base/bind.h"
#include "base/macros.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/user_metrics.h"
#include "base/strings/string_util.h"
#include "ui/accessibility/ax_node.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/platform/aura_window_properties.h"
#include "ui/aura/window.h"
#include "ui/aura/window_targeter.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/ui_base_switches.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_animation_element.h"
#include "ui/compositor/layer_animation_observer.h"
#include "ui/compositor/layer_animation_sequence.h"
#include "ui/compositor/scoped_animation_duration_scale_mode.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/skia_util.h"
#include "ui/keyboard/keyboard_controller.h"
#include "ui/strings/grit/ui_strings.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/views_delegate.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/wm/core/coordinate_conversion.h"
#include "ui/wm/core/ime_util_chromeos.h"
#include "ui/wm/core/shadow_types.h"

using ash::ColorProfileType;

namespace app_list {

namespace {

// The height of the half app list from the bottom of the screen.
constexpr int kHalfAppListHeight = 561;

// The fraction of app list height that the app list must be released at in
// order to transition to the next state.
constexpr int kAppListThresholdDenominator = 3;

// The scroll offset in order to transition from PEEKING to FULLSCREEN
constexpr int kAppListMinScrollToSwitchStates = 20;

// The DIP distance from the bezel in which a gesture drag end results in a
// closed app list.
constexpr int kAppListBezelMargin = 50;

// The size of app info dialog in fullscreen app list.
constexpr int kAppInfoDialogWidth = 512;
constexpr int kAppInfoDialogHeight = 384;

// The animation duration for app list movement.
constexpr float kAppListAnimationDurationTestMs = 0;
constexpr float kAppListAnimationDurationMs = 200;
constexpr float kAppListAnimationDurationFromFullscreenMs = 250;

// Events within this threshold from the top of the view will be reserved for
// home launcher gestures, if they can be processed.
constexpr int kAppListHomeLaucherGesturesThreshold = 32;

// Quality of the shield background blur.
constexpr float kAppListBlurQuality = 0.33f;

// Set animation durations to 0 for testing.
// TODO(oshima): Use ui::ScopedAnimationDurationScaleMode instead.
bool short_animations_for_testing;

// Histogram for the app list dragging. The suffix ClamshellMode is added
// in case a similar UI is added to TabletMode in the future.
constexpr char kAppListDragInClamshellHistogram[] =
    "Apps.StateTransition.Drag.PresentationTime.ClamshellMode";
constexpr char kAppListDragInClamshellMaxLatencyHistogram[] =
    "Apps.StateTransition.Drag.PresentationTime.MaxLatency.ClamshellMode";

// This view forwards the focus to the search box widget by providing it as a
// FocusTraversable when a focus search is provided.
class SearchBoxFocusHost : public views::View {
 public:
  explicit SearchBoxFocusHost(views::Widget* search_box_widget)
      : search_box_widget_(search_box_widget) {}

  ~SearchBoxFocusHost() override {}

  views::FocusTraversable* GetFocusTraversable() override {
    return search_box_widget_;
  }

 private:
  views::Widget* search_box_widget_;

  DISALLOW_COPY_AND_ASSIGN(SearchBoxFocusHost);
};

SkColor GetBackgroundShieldColor(const std::vector<SkColor>& prominent_colors,
                                 float color_opacity) {
  const U8CPU sk_opacity_value = static_cast<U8CPU>(255 * color_opacity);

  const SkColor default_color = SkColorSetA(
      app_list::AppListView::kDefaultBackgroundColor, sk_opacity_value);

  if (prominent_colors.empty())
    return default_color;

  DCHECK_EQ(static_cast<size_t>(ColorProfileType::NUM_OF_COLOR_PROFILES),
            prominent_colors.size());

  const SkColor dark_muted =
      prominent_colors[static_cast<int>(ColorProfileType::DARK_MUTED)];
  if (SK_ColorTRANSPARENT == dark_muted)
    return default_color;

  return SkColorSetA(
      color_utils::GetResultingPaintColor(
          SkColorSetA(SK_ColorBLACK, AppListView::kAppListColorDarkenAlpha),
          dark_muted),
      sk_opacity_value);
}

DEFINE_UI_CLASS_PROPERTY_KEY(bool, kExcludeWindowFromEventHandling, false)

// This targeter prevents routing events to sub-windows, such as
// RenderHostWindow in order to handle events in context of app list.
class AppListEventTargeter : public aura::WindowTargeter {
 public:
  AppListEventTargeter() = default;
  ~AppListEventTargeter() override = default;

  // aura::WindowTargeter:
  bool SubtreeShouldBeExploredForEvent(aura::Window* window,
                                       const ui::LocatedEvent& event) override {
    if (window->GetProperty(kExcludeWindowFromEventHandling)) {
      // Allow routing to sub-windows for ET_MOUSE_MOVED event which is used by
      // accessibility to enter the mode of exploration of WebView contents.
      if (event.type() != ui::ET_MOUSE_MOVED)
        return false;
    }

    if (window->GetProperty(ash::assistant::ui::kOnlyAllowMouseClickEvents)) {
      if (event.type() != ui::ET_MOUSE_PRESSED &&
          event.type() != ui::ET_MOUSE_RELEASED) {
        return false;
      }
    }

    return aura::WindowTargeter::SubtreeShouldBeExploredForEvent(window, event);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(AppListEventTargeter);
};

}  // namespace

class AppListView::StateAnimationMetricsReporter
    : public ui::AnimationMetricsReporter {
 public:
  explicit StateAnimationMetricsReporter(AppListView* view) : view_(view) {}

  ~StateAnimationMetricsReporter() override = default;

  void Start(bool is_in_tablet_mode) {
    DCHECK(!started_);
    is_in_tablet_mode_ = is_in_tablet_mode;
#if defined(DCHECK)
    started_ = ui::ScopedAnimationDurationScaleMode::duration_scale_mode() !=
               ui::ScopedAnimationDurationScaleMode::ZERO_DURATION;
#endif
  }

  void Report(int value) override {
    UMA_HISTOGRAM_PERCENTAGE("Apps.StateTransition.AnimationSmoothness", value);
    if (is_in_tablet_mode_) {
      UMA_HISTOGRAM_PERCENTAGE(
          "Apps.StateTransition.AnimationSmoothness.TabletMode", value);
    } else {
      UMA_HISTOGRAM_PERCENTAGE(
          "Apps.StateTransition.AnimationSmoothness.ClamshellMode", value);
    }
    view_->OnStateTransitionAnimationCompleted();
#if defined(DCHECK)
    started_ = false;
#endif
  }

 private:
#if defined(DCHECK)
  bool started_ = false;
#endif
  bool is_in_tablet_mode_ = false;
  AppListView* view_;

  DISALLOW_COPY_AND_ASSIGN(StateAnimationMetricsReporter);
};

// An animation observer to hide the view at the end of the animation.
class HideViewAnimationObserver : public ui::ImplicitAnimationObserver {
 public:
  HideViewAnimationObserver() : target_(NULL) {}

  ~HideViewAnimationObserver() override {
    if (target_)
      StopObservingImplicitAnimations();
  }

  void SetTarget(views::View* target) {
    if (target_)
      StopObservingImplicitAnimations();
    target_ = target;
  }

 private:
  // Overridden from ui::ImplicitAnimationObserver:
  void OnImplicitAnimationsCompleted() override {
    if (target_) {
      target_->SetVisible(false);
      target_ = NULL;
    }
  }

  views::View* target_;

  DISALLOW_COPY_AND_ASSIGN(HideViewAnimationObserver);
};

// An animation observer to transition between states.
class TransitionAnimationObserver : public ui::ImplicitAnimationObserver {
 public:
  explicit TransitionAnimationObserver(AppListView* view) : view_(view) {}

  // ui::ImplicitAnimationObserver:
  void OnImplicitAnimationsCompleted() override {
    DCHECK(view_);
    view_->Layout();
  }

 private:
  AppListView* const view_;

  DISALLOW_COPY_AND_ASSIGN(TransitionAnimationObserver);
};

// The view for the app list background shield which changes color and radius.
class AppListBackgroundShieldView : public views::View {
 public:
  explicit AppListBackgroundShieldView(ui::LayerType layer_type)
      : color_(AppListView::kDefaultBackgroundColor), corner_radius_(0) {
    SetPaintToLayer(layer_type);
    layer()->SetFillsBoundsOpaquely(false);
    if (layer()->type() == ui::LAYER_SOLID_COLOR)
      layer()->SetColor(color_);
  }

  ~AppListBackgroundShieldView() override = default;

  void UpdateColor(SkColor color) {
    if (color_ == color)
      return;

    color_ = color;
    if (layer()->type() == ui::LAYER_SOLID_COLOR)
      layer()->SetColor(color);
    else
      SchedulePaint();
  }

  void UpdateCornerRadius(int corner_radius) {
    if (corner_radius_ == corner_radius)
      return;

    corner_radius_ = corner_radius;
    if (!layer())
      SchedulePaint();
  }

  // Overridden from views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setStyle(cc::PaintFlags::kFill_Style);
    flags.setColor(color_);
    canvas->DrawRoundRect(GetContentsBounds(), corner_radius_, flags);
  }

  SkColor GetColorForTest() const { return color_; }

 private:
  SkColor color_;
  int corner_radius_;

  DISALLOW_COPY_AND_ASSIGN(AppListBackgroundShieldView);
};

////////////////////////////////////////////////////////////////////////////////
// AppListView::TestApi

AppListView::TestApi::TestApi(AppListView* view) : view_(view) {
  DCHECK(view_);
}

AppListView::TestApi::~TestApi() = default;

AppsGridView* AppListView::TestApi::GetRootAppsGridView() {
  return view_->GetRootAppsGridView();
}

////////////////////////////////////////////////////////////////////////////////
// AppListView:

AppListView::AppListView(AppListViewDelegate* delegate)
    : delegate_(delegate),
      model_(delegate->GetModel()),
      search_model_(delegate->GetSearchModel()),
      is_background_blur_enabled_(app_list_features::IsBackgroundBlurEnabled()),
      hide_view_animation_observer_(
          std::make_unique<HideViewAnimationObserver>()),
      transition_animation_observer_(
          std::make_unique<TransitionAnimationObserver>(this)),
      state_animation_metrics_reporter_(
          std::make_unique<StateAnimationMetricsReporter>(this)),
      weak_ptr_factory_(this) {
  CHECK(delegate);
}

AppListView::~AppListView() {
  hide_view_animation_observer_.reset();
  // Remove child views first to ensure no remaining dependencies on delegate_.
  RemoveAllChildViews(true);
}

// static
void AppListView::ExcludeWindowFromEventHandling(aura::Window* window) {
  DCHECK(window);
  window->SetProperty(kExcludeWindowFromEventHandling, true);
}

// static
void AppListView::SetShortAnimationForTesting(bool enabled) {
  short_animations_for_testing = enabled;
}

// static
bool AppListView::ShortAnimationsForTesting() {
  return short_animations_for_testing;
}

void AppListView::Initialize(const InitParams& params) {
  base::Time start_time = base::Time::Now();
  is_tablet_mode_ = params.is_tablet_mode;
  is_side_shelf_ = params.is_side_shelf;
  InitContents(params.initial_apps_page);
  AddAccelerator(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));
  AddAccelerator(ui::Accelerator(ui::VKEY_BROWSER_BACK, ui::EF_NONE));
  parent_window_ = params.parent;

  InitializeFullscreen(params.parent);

  InitChildWidgets();

  SetState(app_list_state_);

  // Ensures that the launcher won't open underneath the a11y keyboard
  CloseKeyboardIfVisible();

  // Tablet mode is enabled before the app list is shown, so apply the changes
  // that should occur upon entering the tablet mode here.
  if (is_tablet_mode())
    OnTabletModeChanged(is_tablet_mode_);

  UMA_HISTOGRAM_TIMES(kAppListCreationTimeHistogram,
                      base::Time::Now() - start_time);
  RecordFolderMetrics();
}

void AppListView::SetDragAndDropHostOfCurrentAppList(
    ApplicationDragAndDropHost* drag_and_drop_host) {
  app_list_main_view_->SetDragAndDropHostOfCurrentAppList(drag_and_drop_host);
}

void AppListView::ShowWhenReady() {
  app_list_main_view_->ShowAppListWhenReady();
}

void AppListView::Dismiss() {
  CloseKeyboardIfVisible();
  app_list_main_view_->Close();
  SetState(ash::mojom::AppListViewState::kClosed);
  delegate_->DismissAppList();
  GetWidget()->Deactivate();
}

bool AppListView::CloseOpenedPage() {
  if (!app_list_main_view_)
    return false;

  if (app_list_main_view_->contents_view()->IsShowingSearchResults() ||
      GetAppsContainerView()->IsInFolderView()) {
    return app_list_main_view_->contents_view()->Back();
  }
  return false;
}

void AppListView::Back() {
  app_list_main_view_->contents_view()->Back();
}

void AppListView::OnPaint(gfx::Canvas* canvas) {
  views::WidgetDelegateView::OnPaint(canvas);
  if (!next_paint_callback_.is_null()) {
    next_paint_callback_.Run();
    next_paint_callback_.Reset();
  }
}

const char* AppListView::GetClassName() const {
  return "AppListView";
}

bool AppListView::CanProcessEventsWithinSubtree() const {
  if (!delegate_->CanProcessEventsOnApplistViews())
    return false;

  return views::View::CanProcessEventsWithinSubtree();
}

bool AppListView::AcceleratorPressed(const ui::Accelerator& accelerator) {
  switch (accelerator.key_code()) {
    case ui::VKEY_ESCAPE:
    case ui::VKEY_BROWSER_BACK:
      // If the ContentsView does not handle the back action, then this is the
      // top level, so we close the app list.
      if (!app_list_main_view_->contents_view()->Back() && !is_tablet_mode()) {
        Dismiss();
      }
      break;
    default:
      NOTREACHED();
      return false;
  }

  // Don't let DialogClientView handle the accelerator.
  return true;
}

void AppListView::Layout() {
  const gfx::Rect contents_bounds = GetContentsBounds();

  // Exclude the shelf height from the contents bounds to avoid apps grid from
  // overlapping with shelf.
  gfx::Rect main_bounds = contents_bounds;
  main_bounds.Inset(0, 0, 0, AppListConfig::instance().shelf_height());

  // The AppListMainView's size is supposed to be the same as AppsContainerView.
  const gfx::Size min_main_size = GetAppsContainerView()->GetMinimumSize();

  if ((main_bounds.width() > 0 && main_bounds.height() > 0) &&
      (main_bounds.width() < min_main_size.width() ||
       main_bounds.height() < min_main_size.height())) {
    // Scale down the AppListMainView if AppsContainerView does not fit in the
    // display.
    const float scale = std::min(
        (main_bounds.width()) / static_cast<float>(min_main_size.width()),
        main_bounds.height() / static_cast<float>(min_main_size.height()));
    DCHECK_GT(scale, 0);
    const gfx::RectF scaled_main_bounds(main_bounds.x(), main_bounds.y(),
                                        main_bounds.width() / scale,
                                        main_bounds.height() / scale);
    gfx::Transform transform;
    transform.Scale(scale, scale);
    app_list_main_view_->SetTransform(transform);
    app_list_main_view_->SetBoundsRect(gfx::ToEnclosedRect(scaled_main_bounds));
  } else {
    app_list_main_view_->SetTransform(gfx::Transform());
    app_list_main_view_->SetBoundsRect(main_bounds);
  }

  gfx::Rect app_list_background_shield_bounds = contents_bounds;
  // Inset bottom by 2 * |kAppListBackgroundRadius| to account for the rounded
  // corners on the top and bottom of the |app_list_background_shield_|.
  // Only add the inset to the bottom to keep padding at the top of the AppList
  // the same.
  app_list_background_shield_bounds.Inset(0, 0, 0,
                                          -kAppListBackgroundRadius * 2);
  app_list_background_shield_->SetBoundsRect(app_list_background_shield_bounds);
  app_list_background_shield_->UpdateCornerRadius(kAppListBackgroundRadius);
  if (is_background_blur_enabled_ && app_list_background_shield_mask_ &&
      !is_tablet_mode() &&
      app_list_background_shield_->layer()->size() !=
          app_list_background_shield_mask_->layer()->size()) {
    // Update the blur mask for the |app_list_background_shield_| with same
    // shape and size if their bounds don't match.
    app_list_background_shield_mask_->layer()->SetBounds(
        app_list_background_shield_bounds);
  }

  UpdateAppListBackgroundYPosition();
}

ax::mojom::Role AppListView::GetAccessibleWindowRole() {
  // Default role of root view is ax::mojom::Role::kWindow which traps ChromeVox
  // focus within the root view. Assign ax::mojom::Role::kGroup here to allow
  // the focus to move from elements in app list view to search box.
  return ax::mojom::Role::kGroup;
}

class AppListView::FullscreenWidgetObserver : views::WidgetObserver {
 public:
  explicit FullscreenWidgetObserver(app_list::AppListView* view)
      : widget_observer_(this) {
    view_ = view;
    widget_observer_.Add(view_->GetWidget());
  }
  ~FullscreenWidgetObserver() override {}

  // Overridden from WidgetObserver:
  void OnWidgetClosing(views::Widget* widget) override {
    if (view_->app_list_state() != ash::mojom::AppListViewState::kClosed)
      view_->SetState(ash::mojom::AppListViewState::kClosed);
    widget_observer_.Remove(view_->GetWidget());
  }

 private:
  app_list::AppListView* view_;
  ScopedObserver<views::Widget, WidgetObserver> widget_observer_;
  DISALLOW_COPY_AND_ASSIGN(FullscreenWidgetObserver);
};

views::View* AppListView::GetAppListBackgroundShieldForTest() {
  return app_list_background_shield_;
}

SkColor AppListView::GetAppListBackgroundShieldColorForTest() {
  DCHECK(app_list_background_shield_);
  return app_list_background_shield_->GetColorForTest();
}

void AppListView::InitContents(int initial_apps_page) {
  // The shield view that colors/blurs the background of the app list and
  // makes it transparent.
  bool use_background_blur = is_background_blur_enabled_ && !is_tablet_mode();
  app_list_background_shield_ = new AppListBackgroundShieldView(
      use_background_blur ? ui::LAYER_SOLID_COLOR : ui::LAYER_TEXTURED);
  SetBackgroundShieldColor();
  if (use_background_blur) {
    if (ash::features::ShouldUseShaderRoundedCorner()) {
      app_list_background_shield_->layer()->SetRoundedCornerRadius(
          {kAppListBackgroundRadius, kAppListBackgroundRadius, 0, 0});
    } else {
      app_list_background_shield_mask_ = views::Painter::CreatePaintedLayer(
          views::Painter::CreateSolidRoundRectPainter(
              SK_ColorBLACK, kAppListBackgroundRadius));
      app_list_background_shield_mask_->layer()->SetFillsBoundsOpaquely(false);
      app_list_background_shield_->layer()->SetMaskLayer(
          app_list_background_shield_mask_->layer());
    }
    app_list_background_shield_->layer()->SetBackgroundBlur(
        AppListConfig::instance().blur_radius());
    app_list_background_shield_->layer()->SetBackdropFilterQuality(
        kAppListBlurQuality);
  }
  AddChildView(app_list_background_shield_);
  app_list_main_view_ = new AppListMainView(delegate_, this);
  AddChildView(app_list_main_view_);
  // This will be added to the |search_box_widget_| after the app list widget is
  // initialized.
  search_box_view_ = new SearchBoxView(app_list_main_view_, delegate_, this);
  search_box_view_->Init();

  app_list_main_view_->Init(0, search_box_view_);

  announcement_view_ = new views::View();
  AddChildView(announcement_view_);
}

void AppListView::InitChildWidgets() {
  DCHECK(search_box_view_);

  // Create the search box widget.
  views::Widget::InitParams search_box_widget_params(
      views::Widget::InitParams::TYPE_CONTROL);
  search_box_widget_params.parent = GetWidget()->GetNativeView();
  search_box_widget_params.opacity =
      views::Widget::InitParams::TRANSLUCENT_WINDOW;
  search_box_widget_params.name = "SearchBoxView";
  search_box_widget_params.delegate = search_box_view_;

  // Create a widget for the SearchBoxView to live in. This allows the
  // SearchBoxView to be on top of the custom launcher page's WebContents
  // (otherwise the search box events will be captured by the WebContents).
  search_box_widget_ = new views::Widget;
  search_box_widget_->Init(search_box_widget_params);

  // Assign an accessibility role to the native window of search box widget, so
  // that hitting search+right could move ChromeVox focus across search box to
  // other elements in app list view.
  search_box_widget_->GetNativeWindow()->SetProperty(
      ui::kAXRoleOverride,
      static_cast<ax::mojom::Role>(ax::mojom::Role::kGroup));

  // The search box will not naturally receive focus by itself (because it is in
  // a separate widget). Create this SearchBoxFocusHost in the main widget to
  // forward the focus search into to the search box.
  search_box_focus_host_ = new SearchBoxFocusHost(search_box_widget_);
  AddChildView(search_box_focus_host_);
  search_box_widget_->SetFocusTraversableParentView(search_box_focus_host_);
  search_box_widget_->SetFocusTraversableParent(
      GetWidget()->GetFocusTraversable());

  app_list_main_view_->contents_view()->Layout();
}

void AppListView::InitializeFullscreen(gfx::NativeView parent) {
  fullscreen_widget_ = new views::Widget;
  views::Widget::InitParams app_list_overlay_view_params(
      views::Widget::InitParams::TYPE_WINDOW_FRAMELESS);

  app_list_overlay_view_params.name = "AppList";
  app_list_overlay_view_params.parent = parent;
  app_list_overlay_view_params.delegate = this;
  app_list_overlay_view_params.opacity =
      views::Widget::InitParams::TRANSLUCENT_WINDOW;
  app_list_overlay_view_params.layer_type = ui::LAYER_NOT_DRAWN;
  fullscreen_widget_->Init(app_list_overlay_view_params);
  fullscreen_widget_->GetNativeWindow()->SetEventTargeter(
      std::make_unique<AppListEventTargeter>());

  // The widget's initial position will be off the bottom of the display.
  // Set native view's bounds directly to avoid screen position controller
  // setting bounds in the display where the widget has the largest
  // intersection.
  // TODO(mash): Redesign this animation to position the widget to cover the
  // entire screen, then animate the layer up into position. crbug.com/768437
  // The initial bounds of app list should be the same as that in closed state.
  fullscreen_widget_->GetNativeView()->SetBounds(
      GetPreferredWidgetBoundsForState(ash::mojom::AppListViewState::kClosed));

  // Enable arrow key in FocusManager. Arrow left/right and up/down triggers
  // the same focus movement as tab/shift+tab.
  fullscreen_widget_->GetFocusManager()
      ->set_arrow_key_traversal_enabled_for_widget(true);

  widget_observer_ = std::make_unique<FullscreenWidgetObserver>(this);
  fullscreen_widget_->GetNativeView()->AddObserver(this);
}

void AppListView::HandleClickOrTap(ui::LocatedEvent* event) {
  // If the virtual keyboard is visible, dismiss the keyboard and return early.
  if (CloseKeyboardIfVisible()) {
    search_box_view_->NotifyGestureEvent();
    return;
  }

  // Clear focus if the located event is not handled by any child view.
  GetFocusManager()->ClearFocus();

  if (GetAppsContainerView()->IsInFolderView()) {
    // Close the folder if it is opened.
    GetAppsContainerView()->app_list_folder_view()->CloseFolderPage();
    return;
  }

  if ((event->IsGestureEvent() &&
       (event->AsGestureEvent()->type() == ui::ET_GESTURE_LONG_PRESS ||
        event->AsGestureEvent()->type() == ui::ET_GESTURE_LONG_TAP ||
        event->AsGestureEvent()->type() == ui::ET_GESTURE_TWO_FINGER_TAP)) ||
      (event->IsMouseEvent() &&
       event->AsMouseEvent()->IsOnlyRightMouseButton())) {
    // Don't show menus on empty areas of the AppListView in clamshell mode.
    if (!is_tablet_mode())
      return;

    // Home launcher is shown on top of wallpaper with trasparent background. So
    // trigger the wallpaper context menu for the same events.
    gfx::Point onscreen_location(event->location());
    ConvertPointToScreen(this, &onscreen_location);
    delegate_->ShowWallpaperContextMenu(
        onscreen_location, event->IsGestureEvent() ? ui::MENU_SOURCE_TOUCH
                                                   : ui::MENU_SOURCE_MOUSE);
    return;
  }

  if (!search_box_view_->is_search_box_active() &&
      model_->state() != ash::AppListState::kStateEmbeddedAssistant) {
    if (!is_tablet_mode())
      Dismiss();
    return;
  }

  // Reset the AppListState if the embedded Assistant UI is shown.
  if (app_list_main_view()->contents_view()->IsShowingEmbeddedAssistantUI())
    Back();

  search_box_view_->ClearSearchAndDeactivateSearchBox();
}

void AppListView::StartDrag(const gfx::Point& location) {
  // Convert drag point from widget coordinates to screen coordinates because
  // the widget bounds changes during the dragging.
  initial_drag_point_ = location;
  ConvertPointToScreen(this, &initial_drag_point_);
  initial_window_bounds_ = fullscreen_widget_->GetWindowBoundsInScreen();
}

void AppListView::UpdateDrag(const gfx::Point& location) {
  // Update the widget bounds based on the initial widget bounds and drag delta.
  gfx::Point location_in_screen_coordinates = location;
  ConvertPointToScreen(this, &location_in_screen_coordinates);
  int new_y_position = location_in_screen_coordinates.y() -
                       initial_drag_point_.y() + initial_window_bounds_.y();

  UpdateYPositionAndOpacity(new_y_position,
                            GetAppListBackgroundOpacityDuringDragging());
}

void AppListView::EndDrag(const gfx::Point& location) {
  // When the SearchBoxView closes the app list, ignore the final event.
  if (app_list_state_ == ash::mojom::AppListViewState::kClosed)
    return;

  // Change the app list state based on where the drag ended. If fling velocity
  // was over the threshold, snap to the next state in the direction of the
  // fling.
  if (std::abs(last_fling_velocity_) >= kDragVelocityThreshold) {
    // If the user releases drag with velocity over the threshold, snap to
    // the next state, ignoring the drag release position.

    if (last_fling_velocity_ > 0) {
      switch (app_list_state_) {
        case ash::mojom::AppListViewState::kPeeking:
        case ash::mojom::AppListViewState::kHalf:
        case ash::mojom::AppListViewState::kFullscreenSearch:
        case ash::mojom::AppListViewState::kFullscreenAllApps:
          Dismiss();
          break;
        case ash::mojom::AppListViewState::kClosed:
          NOTREACHED();
          break;
      }
    } else {
      switch (app_list_state_) {
        case ash::mojom::AppListViewState::kFullscreenAllApps:
        case ash::mojom::AppListViewState::kFullscreenSearch:
          SetState(app_list_state_);
          break;
        case ash::mojom::AppListViewState::kHalf:
          SetState(ash::mojom::AppListViewState::kFullscreenSearch);
          break;
        case ash::mojom::AppListViewState::kPeeking:
          UMA_HISTOGRAM_ENUMERATION(kAppListPeekingToFullscreenHistogram,
                                    kSwipe, kMaxPeekingToFullscreen);
          SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
          break;
        case ash::mojom::AppListViewState::kClosed:
          NOTREACHED();
          break;
      }
    }
  } else {
    const int fullscreen_height = GetFullscreenStateHeight();
    int app_list_height = 0;
    switch (app_list_state_) {
      case ash::mojom::AppListViewState::kFullscreenAllApps:
      case ash::mojom::AppListViewState::kFullscreenSearch:
        app_list_height = fullscreen_height;
        break;
      case ash::mojom::AppListViewState::kHalf:
        app_list_height = std::min(fullscreen_height, kHalfAppListHeight);
        break;
      case ash::mojom::AppListViewState::kPeeking:
        app_list_height = AppListConfig::instance().peeking_app_list_height();
        break;
      case ash::mojom::AppListViewState::kClosed:
        NOTREACHED();
        break;
    }

    const int app_list_threshold =
        app_list_height / kAppListThresholdDenominator;
    gfx::Point location_in_screen_coordinates = location;
    ConvertPointToScreen(this, &location_in_screen_coordinates);
    const int drag_delta =
        initial_drag_point_.y() - location_in_screen_coordinates.y();
    const int location_y_in_current_work_area =
        location_in_screen_coordinates.y() -
        GetDisplayNearestView().work_area().y();
    // If the drag ended near the bezel, close the app list and return early.
    if (location_y_in_current_work_area >=
        (fullscreen_height - kAppListBezelMargin)) {
      Dismiss();
      return;
    }
    switch (app_list_state_) {
      case ash::mojom::AppListViewState::kFullscreenAllApps:
        if (drag_delta < -app_list_threshold) {
          if (is_tablet_mode_ || is_side_shelf_)
            Dismiss();
          else
            SetState(ash::mojom::AppListViewState::kPeeking);
        } else {
          SetState(app_list_state_);
        }
        break;
      case ash::mojom::AppListViewState::kFullscreenSearch:
        if (drag_delta < -app_list_threshold)
          Dismiss();
        else
          SetState(app_list_state_);
        break;
      case ash::mojom::AppListViewState::kHalf:
        if (drag_delta > app_list_threshold)
          SetState(ash::mojom::AppListViewState::kFullscreenSearch);
        else if (drag_delta < -app_list_threshold)
          Dismiss();
        else
          SetState(app_list_state_);
        break;
      case ash::mojom::AppListViewState::kPeeking:
        if (drag_delta > app_list_threshold) {
          SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
          UMA_HISTOGRAM_ENUMERATION(kAppListPeekingToFullscreenHistogram,
                                    kSwipe, kMaxPeekingToFullscreen);
        } else if (drag_delta < -app_list_threshold) {
          Dismiss();
        } else {
          SetState(app_list_state_);
        }
        break;
      case ash::mojom::AppListViewState::kClosed:
        NOTREACHED();
        break;
    }
  }
  UpdateChildViewsYPositionAndOpacity();
  initial_drag_point_ = gfx::Point();
}

void AppListView::SetChildViewsForStateTransition(
    ash::mojom::AppListViewState target_state) {
  if (target_state != ash::mojom::AppListViewState::kPeeking &&
      target_state != ash::mojom::AppListViewState::kFullscreenAllApps &&
      target_state != ash::mojom::AppListViewState::kHalf) {
    return;
  }

  app_list_main_view_->contents_view()->OnAppListViewTargetStateChanged(
      target_state);

  if (target_state == ash::mojom::AppListViewState::kHalf)
    return;

  if (GetAppsContainerView()->IsInFolderView())
    GetAppsContainerView()->ResetForShowApps();

  app_list_main_view_->contents_view()->SetActiveState(
      ash::AppListState::kStateApps, !is_side_shelf_);

  if (target_state == ash::mojom::AppListViewState::kPeeking) {
    // Set the apps to the initial page when PEEKING.
    PaginationModel* pagination_model = GetAppsPaginationModel();
    if (pagination_model->total_pages() > 0 &&
        pagination_model->selected_page() != 0) {
      pagination_model->SelectPage(0, false /* animate */);
    }
  }
}

void AppListView::ConvertAppListStateToFullscreenEquivalent(
    ash::mojom::AppListViewState* target_state) {
  if (!(is_side_shelf_ || is_tablet_mode_))
    return;

  // If side shelf or tablet mode are active, all transitions should be
  // made to the tablet mode/side shelf friendly versions.
  if (*target_state == ash::mojom::AppListViewState::kHalf) {
    *target_state = ash::mojom::AppListViewState::kFullscreenSearch;
  } else if (*target_state == ash::mojom::AppListViewState::kPeeking) {
    // FULLSCREEN_ALL_APPS->PEEKING in tablet/side shelf mode should close
    // instead of going to PEEKING.
    *target_state =
        app_list_state_ == ash::mojom::AppListViewState::kFullscreenAllApps
            ? ash::mojom::AppListViewState::kClosed
            : ash::mojom::AppListViewState::kFullscreenAllApps;
  }
}

void AppListView::RecordStateTransitionForUma(
    ash::mojom::AppListViewState new_state) {
  AppListStateTransitionSource transition =
      GetAppListStateTransitionSource(new_state);
  // kMaxAppListStateTransition denotes a transition we are not interested in
  // recording (ie. PEEKING->PEEKING).
  if (transition == kMaxAppListStateTransition)
    return;

  UMA_HISTOGRAM_ENUMERATION(kAppListStateTransitionSourceHistogram, transition,
                            kMaxAppListStateTransition);

  switch (transition) {
    case kPeekingToFullscreenAllApps:
    case KHalfToFullscreenSearch:
      base::RecordAction(base::UserMetricsAction("AppList_PeekingToFull"));
      break;

    case kFullscreenAllAppsToPeeking:
      base::RecordAction(base::UserMetricsAction("AppList_FullToPeeking"));
      break;

    default:
      break;
  }
}

void AppListView::MaybeCreateAccessibilityEvent(
    ash::mojom::AppListViewState new_state) {
  if (new_state != ash::mojom::AppListViewState::kPeeking &&
      new_state != ash::mojom::AppListViewState::kFullscreenAllApps)
    return;

  base::string16 state_announcement;

  if (new_state == ash::mojom::AppListViewState::kPeeking) {
    state_announcement = l10n_util::GetStringUTF16(
        IDS_APP_LIST_SUGGESTED_APPS_ACCESSIBILITY_ANNOUNCEMENT);
  } else {
    state_announcement = l10n_util::GetStringUTF16(
        IDS_APP_LIST_ALL_APPS_ACCESSIBILITY_ANNOUNCEMENT);
  }
  announcement_view_->GetViewAccessibility().OverrideName(state_announcement);
  announcement_view_->NotifyAccessibilityEvent(ax::mojom::Event::kAlert, true);
}

display::Display AppListView::GetDisplayNearestView() const {
  return display::Screen::GetScreen()->GetDisplayNearestView(parent_window_);
}

AppsContainerView* AppListView::GetAppsContainerView() {
  return app_list_main_view_->contents_view()->GetAppsContainerView();
}

AppsGridView* AppListView::GetRootAppsGridView() {
  return GetAppsContainerView()->apps_grid_view();
}

AppsGridView* AppListView::GetFolderAppsGridView() {
  return GetAppsContainerView()->app_list_folder_view()->items_grid_view();
}

AppListStateTransitionSource AppListView::GetAppListStateTransitionSource(
    ash::mojom::AppListViewState target_state) const {
  switch (app_list_state_) {
    case ash::mojom::AppListViewState::kClosed:
      // CLOSED->X transitions are not useful for UMA.
      return kMaxAppListStateTransition;
    case ash::mojom::AppListViewState::kPeeking:
      switch (target_state) {
        case ash::mojom::AppListViewState::kClosed:
          return kPeekingToClosed;
        case ash::mojom::AppListViewState::kHalf:
          return kPeekingToHalf;
        case ash::mojom::AppListViewState::kFullscreenAllApps:
          return kPeekingToFullscreenAllApps;
        case ash::mojom::AppListViewState::kPeeking:
          // PEEKING->PEEKING is used when resetting the widget position after a
          // failed state transition. Not useful for UMA.
          return kMaxAppListStateTransition;
        case ash::mojom::AppListViewState::kFullscreenSearch:
          // PEEKING->FULLSCREEN_SEARCH is not a valid transition.
          NOTREACHED();
          return kMaxAppListStateTransition;
      }
    case ash::mojom::AppListViewState::kHalf:
      switch (target_state) {
        case ash::mojom::AppListViewState::kClosed:
          return kHalfToClosed;
        case ash::mojom::AppListViewState::kPeeking:
          return kHalfToPeeking;
        case ash::mojom::AppListViewState::kFullscreenSearch:
          return KHalfToFullscreenSearch;
        case ash::mojom::AppListViewState::kHalf:
          // HALF->HALF is used when resetting the widget position after a
          // failed state transition. Not useful for UMA.
          return kMaxAppListStateTransition;
        case ash::mojom::AppListViewState::kFullscreenAllApps:
          // HALF->FULLSCREEN_ALL_APPS is not a valid transition.
          NOTREACHED();
          return kMaxAppListStateTransition;
      }

    case ash::mojom::AppListViewState::kFullscreenAllApps:
      switch (target_state) {
        case ash::mojom::AppListViewState::kClosed:
          return kFullscreenAllAppsToClosed;
        case ash::mojom::AppListViewState::kPeeking:
          return kFullscreenAllAppsToPeeking;
        case ash::mojom::AppListViewState::kFullscreenSearch:
          return kFullscreenAllAppsToFullscreenSearch;
        case ash::mojom::AppListViewState::kHalf:
          // FULLSCREEN_ALL_APPS->HALF is not a valid transition.
          NOTREACHED();
          return kMaxAppListStateTransition;
        case ash::mojom::AppListViewState::kFullscreenAllApps:
          // FULLSCREEN_ALL_APPS->FULLSCREEN_ALL_APPS is used when resetting the
          // widget positon after a failed state transition. Not useful for UMA.
          return kMaxAppListStateTransition;
      }
    case ash::mojom::AppListViewState::kFullscreenSearch:
      switch (target_state) {
        case ash::mojom::AppListViewState::kClosed:
          return kFullscreenSearchToClosed;
        case ash::mojom::AppListViewState::kFullscreenAllApps:
          return kFullscreenSearchToFullscreenAllApps;
        case ash::mojom::AppListViewState::kFullscreenSearch:
          // FULLSCREEN_SEARCH->FULLSCREEN_SEARCH is used when resetting the
          // widget position after a failed state transition. Not useful for
          // UMA.
          return kMaxAppListStateTransition;
        case ash::mojom::AppListViewState::kPeeking:
          // FULLSCREEN_SEARCH->PEEKING is not a valid transition.
          NOTREACHED();
          return kMaxAppListStateTransition;
        case ash::mojom::AppListViewState::kHalf:
          // FULLSCREEN_SEARCH->HALF is not a valid transition.
          NOTREACHED();
          return kMaxAppListStateTransition;
      }
  }
}

views::View* AppListView::GetInitiallyFocusedView() {
  return app_list_main_view_->search_box_view()->search_box();
}

void AppListView::OnScrollEvent(ui::ScrollEvent* event) {
  if (!HandleScroll(gfx::Vector2d(event->x_offset(), event->y_offset()),
                    event->type())) {
    return;
  }

  event->SetHandled();
  event->StopPropagation();
}

void AppListView::OnMouseEvent(ui::MouseEvent* event) {
  switch (event->type()) {
    case ui::ET_MOUSE_PRESSED:
      event->SetHandled();
      HandleClickOrTap(event);
      break;
    case ui::ET_MOUSEWHEEL:
      if (HandleScroll(event->AsMouseWheelEvent()->offset(), ui::ET_MOUSEWHEEL))
        event->SetHandled();
      break;
    default:
      break;
  }
}

void AppListView::OnGestureEvent(ui::GestureEvent* event) {
  switch (event->type()) {
    case ui::ET_GESTURE_TAP:
    case ui::ET_GESTURE_LONG_PRESS:
    case ui::ET_GESTURE_LONG_TAP:
    case ui::ET_GESTURE_TWO_FINGER_TAP:
      SetIsInDrag(false);
      event->SetHandled();
      HandleClickOrTap(event);
      break;
    case ui::ET_SCROLL_FLING_START:
    case ui::ET_GESTURE_SCROLL_BEGIN: {
      // If the search box is active when we start our drag, let it know.
      if (search_box_view_->is_search_box_active())
        search_box_view_->NotifyGestureEvent();

      if (event->location().y() < kAppListHomeLaucherGesturesThreshold) {
        if (delegate_->ProcessHomeLauncherGesture(event, gfx::Point())) {
          SetIsInDrag(false);
          event->SetHandled();
          HandleClickOrTap(event);
          return;
        }
      }

      // Avoid scrolling events for the app list in tablet mode.
      if (is_side_shelf_ || is_tablet_mode())
        return;
      // There may be multiple scroll begin events in one drag because the
      // relative location of the finger and widget is almost unchanged and
      // scroll begin event occurs when the relative location changes beyond a
      // threshold. So avoid resetting the initial drag point in drag.
      if (!is_in_drag_)
        StartDrag(event->location());
      SetIsInDrag(true);
      event->SetHandled();
      break;
    }
    case ui::ET_GESTURE_SCROLL_UPDATE: {
      gfx::Point location_in_screen = event->location();
      views::View::ConvertPointToScreen(this, &location_in_screen);
      if (delegate_->ProcessHomeLauncherGesture(event, location_in_screen)) {
        SetIsInDrag(true);
        event->SetHandled();
        return;
      }

      // Avoid scrolling events for the app list in tablet mode.
      if (is_side_shelf_ || is_tablet_mode())
        return;
      SetIsInDrag(true);
      last_fling_velocity_ = event->details().scroll_y();
      UpdateDrag(event->location());
      event->SetHandled();
      break;
    }
    case ui::ET_GESTURE_END: {
      gfx::Point location_in_screen = event->location();
      views::View::ConvertPointToScreen(this, &location_in_screen);
      if (delegate_->ProcessHomeLauncherGesture(event, location_in_screen)) {
        SetIsInDrag(false);
        event->SetHandled();
        return;
      }

      if (!is_in_drag_)
        break;
      // Avoid scrolling events for the app list in tablet mode.
      if (is_side_shelf_ || is_tablet_mode())
        return;
      SetIsInDrag(false);
      EndDrag(event->location());
      event->SetHandled();
      break;
    }
    case ui::ET_MOUSEWHEEL: {
      if (HandleScroll(event->AsMouseWheelEvent()->offset(), ui::ET_MOUSEWHEEL))
        event->SetHandled();
      break;
    }
    default:
      break;
  }
}

void AppListView::OnKeyEvent(ui::KeyEvent* event) {
  RedirectKeyEventToSearchBox(event);
}

void AppListView::OnTabletModeChanged(bool started) {
  is_tablet_mode_ = started;

  // Bottom shelf is enforced in tablet mode. When tablet mode ends, the
  // AppListView is destroyed so no need to update |is_side_shelf_|.
  if (started)
    is_side_shelf_ = false;

  search_box_view_->OnTabletModeChanged(started);
  search_model_->SetTabletMode(started);
  GetAppsContainerView()->OnTabletModeChanged(started);

  if (!started) {
    Dismiss();
    return;
  }

  if (is_in_drag_) {
    SetIsInDrag(false);
    UpdateChildViewsYPositionAndOpacity();
  }

  // Set fullscreen state. When current state is fullscreen, we still need to
  // set it again because app list may be in dragging.
  SetState(app_list_state_ == ash::mojom::AppListViewState::kHalf ||
                   app_list_state_ ==
                       ash::mojom::AppListViewState::kFullscreenSearch
               ? ash::mojom::AppListViewState::kFullscreenSearch
               : ash::mojom::AppListViewState::kFullscreenAllApps);

  // In tablet mode, AppListView should not be moved because of the change in
  // virtual keyboard's visibility.
  if (started) {
    fullscreen_widget_->GetNativeView()->ClearProperty(
        wm::kVirtualKeyboardRestoreBoundsKey);
  }

  // Update background color opacity.
  SetBackgroundShieldColor();

  // Update background blur.
  if (is_background_blur_enabled_)
    app_list_background_shield_->layer()->SetBackgroundBlur(0);
}

void AppListView::OnWallpaperColorsChanged() {
  SetBackgroundShieldColor();
  search_box_view_->OnWallpaperColorsChanged();
}

bool AppListView::HandleScroll(const gfx::Vector2d& offset,
                               ui::EventType type) {
  // Ignore 0-offset events to prevent spurious dismissal, see crbug.com/806338
  // The system generates 0-offset ET_SCROLL_FLING_CANCEL events during simple
  // touchpad mouse moves. Those may be passed via mojo APIs and handled here.
  if ((offset.y() == 0 && offset.x() == 0) || is_in_drag() ||
      ShouldIgnoreScrollEvents()) {
    return false;
  }

  if (app_list_state_ != ash::mojom::AppListViewState::kPeeking &&
      app_list_state_ != ash::mojom::AppListViewState::kFullscreenAllApps) {
    return false;
  }

  // Let the Apps grid view handle the event first in FULLSCREEN_ALL_APPS.
  if (app_list_state_ == ash::mojom::AppListViewState::kFullscreenAllApps) {
    AppsGridView* apps_grid_view = GetAppsContainerView()->IsInFolderView()
                                       ? GetFolderAppsGridView()
                                       : GetRootAppsGridView();
    if (apps_grid_view->HandleScrollFromAppListView(offset, type))
      return true;
  }

  // The AppList should not be dismissed with scroll in tablet mode.
  if (is_tablet_mode())
    return true;

  // If the event is a mousewheel event, the offset is always large enough,
  // otherwise the offset must be larger than the scroll threshold.
  if (type == ui::ET_MOUSEWHEEL ||
      abs(offset.y()) > kAppListMinScrollToSwitchStates) {
    if (app_list_state_ == ash::mojom::AppListViewState::kFullscreenAllApps) {
      if (offset.y() > 0)
        Dismiss();
      return true;
    }

    SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
    const AppListPeekingToFullscreenSource source =
        type == ui::ET_MOUSEWHEEL ? kMousewheelScroll : kMousepadScroll;
    UMA_HISTOGRAM_ENUMERATION(kAppListPeekingToFullscreenHistogram, source,
                              kMaxPeekingToFullscreen);
  }
  return true;
}

void AppListView::SetState(ash::mojom::AppListViewState new_state) {
  // Do not allow the state to be changed once it has been set to CLOSED.
  if (app_list_state_ == ash::mojom::AppListViewState::kClosed)
    return;

  ash::mojom::AppListViewState new_state_override = new_state;
  ConvertAppListStateToFullscreenEquivalent(&new_state_override);
  MaybeCreateAccessibilityEvent(new_state_override);
  SetChildViewsForStateTransition(new_state_override);
  StartAnimationForState(new_state_override);
  RecordStateTransitionForUma(new_state_override);
  model_->SetStateFullscreen(new_state_override);
  app_list_state_ = new_state_override;

  // Animations are skipped for side shelf mode, so trigger a layout to update
  // children immediately.
  if (is_side_shelf_)
    Layout();

  if (new_state_override == ash::mojom::AppListViewState::kClosed)
    return;

  if (fullscreen_widget_->IsActive()) {
    // Reset the focus to initially focused view. This should be
    // done before updating visibility of views, because setting
    // focused view invisible automatically moves focus to next
    // focusable view, which potentially causes bugs.
    GetInitiallyFocusedView()->RequestFocus();
  }

  // Updates the visibility of app list items according to the change of
  // |app_list_state_|.
  GetAppsContainerView()->UpdateControlVisibility(app_list_state_, is_in_drag_);
}

void AppListView::StartAnimationForState(
    ash::mojom::AppListViewState target_state) {
  if (is_side_shelf_)
    return;

  // The close animation is handled by the delegate.
  if (target_state == ash::mojom::AppListViewState::kClosed)
    return;

  const display::Display display = GetDisplayNearestView();
  const int target_state_y = GetPreferredWidgetYForState(target_state);
  gfx::Rect target_bounds = fullscreen_widget_->GetNativeView()->bounds();
  const int original_state_y = target_bounds.origin().y();
  target_bounds.set_y(target_state_y);

  int animation_duration;
  // If animating to or from a fullscreen state, animate over 250ms, else
  // animate over 200 ms.
  if (ShortAnimationsForTesting()) {
    animation_duration = kAppListAnimationDurationTestMs;
  } else if (is_fullscreen() ||
             target_state == ash::mojom::AppListViewState::kFullscreenAllApps ||
             target_state == ash::mojom::AppListViewState::kFullscreenSearch) {
    animation_duration = kAppListAnimationDurationFromFullscreenMs;
  } else {
    animation_duration = kAppListAnimationDurationMs;
  }

  if (fullscreen_widget_->GetNativeView()->bounds().y() ==
      display.work_area().bottom()) {
    // If the animation start position is the bottom of the screen, activate the
    // fade in animation. This prevents the search box from flashing at the
    // bottom of the screen as it goes behind the shelf.
    app_list_main_view_->contents_view()->FadeInOnOpen(
        base::TimeDelta::FromMilliseconds(animation_duration));
  }

  ui::Layer* layer = fullscreen_widget_->GetLayer();
  layer->SetBounds(target_bounds);
  gfx::Transform transform;
  transform.Translate(0, original_state_y - target_state_y);
  layer->SetTransform(transform);

  ui::LayerAnimator* animator = layer->GetAnimator();
  animator->StopAnimating();
  ui::ScopedLayerAnimationSettings settings(animator);
  settings.SetTransitionDuration(
      base::TimeDelta::FromMilliseconds(animation_duration));
  settings.SetTweenType(gfx::Tween::EASE_OUT);
  settings.SetPreemptionStrategy(
      ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
  settings.SetAnimationMetricsReporter(state_animation_metrics_reporter_.get());
  settings.AddObserver(transition_animation_observer_.get());

  layer->SetTransform(gfx::Transform());

  // In transition animation, layout is only performed after it is complete,
  // which makes the child views jump. So update y positions in advance here to
  // avoid that.
  app_list_main_view_->contents_view()->UpdateYPositionAndOpacity();
}

void AppListView::StartCloseAnimation(base::TimeDelta animation_duration) {
  if (is_side_shelf_)
    return;

  // If animating from PEEKING, animate the opacity twice as fast so the
  // SearchBoxView does not flash behind the shelf.
  if (app_list_state_ == ash::mojom::AppListViewState::kPeeking ||
      app_list_state_ == ash::mojom::AppListViewState::kClosed) {
    animation_duration /= 2;
  }

  SetState(ash::mojom::AppListViewState::kClosed);
  app_list_main_view_->contents_view()->FadeOutOnClose(animation_duration);
}

void AppListView::SetStateFromSearchBoxView(bool search_box_is_empty,
                                            bool triggered_by_contents_change) {
  switch (app_list_state_) {
    case ash::mojom::AppListViewState::kPeeking:
      if (app_list_features::IsZeroStateSuggestionsEnabled()) {
        if (!search_box_is_empty || search_box_view()->is_search_box_active())
          SetState(ash::mojom::AppListViewState::kHalf);
      } else {
        if (!search_box_is_empty)
          SetState(ash::mojom::AppListViewState::kHalf);
      }
      break;
    case ash::mojom::AppListViewState::kHalf:
      if (app_list_features::IsZeroStateSuggestionsEnabled()) {
        if (search_box_is_empty && !triggered_by_contents_change)
          SetState(ash::mojom::AppListViewState::kPeeking);
      } else {
        if (search_box_is_empty)
          SetState(ash::mojom::AppListViewState::kPeeking);
      }
      break;
    case ash::mojom::AppListViewState::kFullscreenSearch:
      if (app_list_features::IsZeroStateSuggestionsEnabled()) {
        if (search_box_is_empty && !triggered_by_contents_change) {
          SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
          app_list_main_view()->contents_view()->SetActiveState(
              ash::AppListState::kStateApps);
        }
      } else {
        if (search_box_is_empty) {
          SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
          app_list_main_view()->contents_view()->SetActiveState(
              ash::AppListState::kStateApps);
        }
      }
      break;
    case ash::mojom::AppListViewState::kFullscreenAllApps:
      if (app_list_features::IsZeroStateSuggestionsEnabled()) {
        if (!search_box_is_empty ||
            (search_box_is_empty && triggered_by_contents_change))
          SetState(ash::mojom::AppListViewState::kFullscreenSearch);
      } else {
        if (!search_box_is_empty)
          SetState(ash::mojom::AppListViewState::kFullscreenSearch);
      }
      break;
    case ash::mojom::AppListViewState::kClosed:
      // We clean search on app list close.
      break;
  }
}

void AppListView::UpdateYPositionAndOpacity(int y_position_in_screen,
                                            float background_opacity) {
  DCHECK(!is_side_shelf_);
  if (app_list_state_ == ash::mojom::AppListViewState::kClosed)
    return;

  if (fullscreen_widget_->GetLayer()->GetAnimator()->IsAnimatingProperty(
          ui::LayerAnimationElement::TRANSFORM)) {
    fullscreen_widget_->GetLayer()->GetAnimator()->StopAnimatingProperty(
        ui::LayerAnimationElement::TRANSFORM);
  }

  SetIsInDrag(true);

  presentation_time_recorder_->RequestNext();

  background_opacity_in_drag_ = background_opacity;
  gfx::Rect new_widget_bounds = fullscreen_widget_->GetWindowBoundsInScreen();
  app_list_y_position_in_screen_ = std::min(
      std::max(y_position_in_screen, GetDisplayNearestView().work_area().y()),
      GetScreenBottom() - AppListConfig::instance().shelf_height());
  new_widget_bounds.set_y(app_list_y_position_in_screen_);
  gfx::NativeView native_view = fullscreen_widget_->GetNativeView();
  ::wm::ConvertRectFromScreen(native_view->parent(), &new_widget_bounds);
  native_view->SetBounds(new_widget_bounds);
  UpdateChildViewsYPositionAndOpacity();
}

void AppListView::OffsetYPositionOfAppList(int offset) {
  gfx::NativeView native_view = fullscreen_widget_->GetNativeView();
  gfx::Transform transform;
  transform.Translate(0, offset);
  native_view->SetTransform(transform);
}

PaginationModel* AppListView::GetAppsPaginationModel() {
  return GetRootAppsGridView()->pagination_model();
}

gfx::Rect AppListView::GetAppInfoDialogBounds() const {
  gfx::Rect app_info_bounds(GetDisplayNearestView().work_area());
  app_info_bounds.ClampToCenteredSize(
      gfx::Size(kAppInfoDialogWidth, kAppInfoDialogHeight));
  return app_info_bounds;
}

void AppListView::SetIsInDrag(bool is_in_drag) {
  if (!is_in_drag)
    presentation_time_recorder_.reset();

  if (app_list_state_ == ash::mojom::AppListViewState::kClosed)
    return;

  if (is_in_drag == is_in_drag_)
    return;

  if (is_in_drag) {
    DCHECK(!presentation_time_recorder_);
    if (!is_tablet_mode_) {
      presentation_time_recorder_ =
          std::make_unique<ash::PresentationTimeHistogramRecorder>(
              GetWidget()->GetCompositor(), kAppListDragInClamshellHistogram,
              kAppListDragInClamshellMaxLatencyHistogram);
    }
  }

  is_in_drag_ = is_in_drag;
  GetAppsContainerView()->UpdateControlVisibility(app_list_state_, is_in_drag_);
}

int AppListView::GetScreenBottom() const {
  return GetDisplayNearestView().bounds().bottom();
}

int AppListView::GetCurrentAppListHeight() const {
  if (!fullscreen_widget_)
    return AppListConfig::instance().shelf_height();
  return GetScreenBottom() - fullscreen_widget_->GetWindowBoundsInScreen().y();
}

float AppListView::GetAppListTransitionProgress() const {
  const float current_height = GetCurrentAppListHeight();
  const float peeking_height =
      AppListConfig::instance().peeking_app_list_height();
  if (current_height <= peeking_height) {
    // Currently transition progress is between closed and peeking state.
    // Calculate the progress of this transition.
    const float shelf_height =
        GetScreenBottom() - GetDisplayNearestView().work_area().bottom();

    // When screen is rotated, the current height might be smaller than shelf
    // height for just one moment, which results in negative progress. So force
    // the progress to be non-negative.
    return std::max(0.0f, (current_height - shelf_height) /
                              (peeking_height - shelf_height));
  }

  // Currently transition progress is between peeking and fullscreen state.
  // Calculate the progress of this transition.
  const float fullscreen_height_above_peeking =
      GetFullscreenStateHeight() - peeking_height;
  const float current_height_above_peeking = current_height - peeking_height;
  DCHECK_GT(fullscreen_height_above_peeking, 0);
  DCHECK_LE(current_height_above_peeking, fullscreen_height_above_peeking);
  return 1 + current_height_above_peeking / fullscreen_height_above_peeking;
}

int AppListView::GetFullscreenStateHeight() const {
  const display::Display display = GetDisplayNearestView();
  const gfx::Rect display_bounds = display.bounds();
  return display_bounds.height() - display.work_area().y() + display_bounds.y();
}

ash::mojom::AppListViewState AppListView::CalculateStateAfterShelfDrag(
    const ui::GestureEvent& gesture_in_screen,
    float launcher_above_shelf_bottom_amount) const {
  ash::mojom::AppListViewState app_list_state =
      ash::mojom::AppListViewState::kPeeking;
  if (gesture_in_screen.type() == ui::ET_SCROLL_FLING_START &&
      fabs(gesture_in_screen.details().velocity_y()) > kDragVelocityThreshold) {
    // If the scroll sequence terminates with a fling, show the fullscreen app
    // list if the fling was fast enough and in the correct direction, otherwise
    // close it.
    app_list_state = gesture_in_screen.details().velocity_y() < 0
                         ? ash::mojom::AppListViewState::kFullscreenAllApps
                         : ash::mojom::AppListViewState::kClosed;
  } else {
    // Snap the app list to corresponding state according to the snapping
    // thresholds.
    if (is_tablet_mode_) {
      app_list_state =
          launcher_above_shelf_bottom_amount > kDragSnapToFullscreenThreshold
              ? ash::mojom::AppListViewState::kFullscreenAllApps
              : ash::mojom::AppListViewState::kClosed;
    } else {
      if (launcher_above_shelf_bottom_amount <= kDragSnapToClosedThreshold)
        app_list_state = ash::mojom::AppListViewState::kClosed;
      else if (launcher_above_shelf_bottom_amount <=
               kDragSnapToPeekingThreshold)
        app_list_state = ash::mojom::AppListViewState::kPeeking;
      else
        app_list_state = ash::mojom::AppListViewState::kFullscreenAllApps;
    }
  }

  // Deal with the situation of dragging app list from shelf while typing in
  // the search box.
  if (app_list_state == ash::mojom::AppListViewState::kFullscreenAllApps) {
    ash::AppListState active_state =
        app_list_main_view_->contents_view()->GetActiveState();
    if (active_state == ash::AppListState::kStateSearchResults)
      app_list_state = ash::mojom::AppListViewState::kFullscreenSearch;
  }

  return app_list_state;
}

ui::AnimationMetricsReporter* AppListView::GetStateTransitionMetricsReporter() {
  state_animation_metrics_reporter_->Start(is_tablet_mode_);
  return state_animation_metrics_reporter_.get();
}

void AppListView::OnWindowDestroying(aura::Window* window) {
  DCHECK_EQ(fullscreen_widget_->GetNativeView(), window);
  window->RemoveObserver(this);
}

void AppListView::OnWindowBoundsChanged(aura::Window* window,
                                        const gfx::Rect& old_bounds,
                                        const gfx::Rect& new_bounds,
                                        ui::PropertyChangeReason reason) {
  DCHECK_EQ(fullscreen_widget_->GetNativeView(), window);

  // When the virtual keyboard shows, the AppListView is moved upward to avoid
  // the overlapping area with the virtual keyboard. As a result, its bottom
  // side may be on the display edge. Stop showing the rounded corners under
  // this circumstance.
  const bool hide_rounded_corners =
      app_list_state_ == ash::mojom::AppListViewState::kHalf &&
      new_bounds.y() == 0;

  gfx::Transform transform;
  if (hide_rounded_corners)
    transform.Translate(0, -kAppListBackgroundRadius);

  app_list_background_shield_->SetTransform(transform);
  app_list_background_shield_->SchedulePaint();
}

void AppListView::UpdateChildViewsYPositionAndOpacity() {
  if (app_list_state_ == ash::mojom::AppListViewState::kClosed)
    return;

  UpdateAppListBackgroundYPosition();

  // Update the opacity of the background shield.
  SetBackgroundShieldColor();

  search_box_view_->UpdateOpacity();
  app_list_main_view_->contents_view()->UpdateYPositionAndOpacity();
}

void AppListView::RedirectKeyEventToSearchBox(ui::KeyEvent* event) {
  if (event->handled())
    return;

  // Allow text input inside the Assistant page.
  if (app_list_main_view()->contents_view()->IsShowingEmbeddedAssistantUI())
    return;

  views::Textfield* search_box = search_box_view_->search_box();
  const bool is_search_box_focused = search_box->HasFocus();
  const bool is_folder_header_view_focused = GetAppsContainerView()
                                                 ->app_list_folder_view()
                                                 ->folder_header_view()
                                                 ->HasTextFocus();

  // Do not redirect the key event to the |search_box_| when focus is on a
  // text field.
  if (is_search_box_focused || is_folder_header_view_focused)
    return;

  // Do not redirect the arrow keys as they are are used for focus traversal and
  // app movement.
  if (IsArrowKeyEvent(*event))
    return;

  // Redirect key event to |search_box_|.
  search_box->OnKeyEvent(event);
  if (event->handled()) {
    // Set search box focused if the key event is consumed.
    search_box->RequestFocus();
    return;
  }

  // Insert it into search box if the key event is a character. Released
  // key should not be handled to prevent inserting duplicate character.
  if (event->type() == ui::ET_KEY_PRESSED)
    search_box->InsertChar(*event);
}

void AppListView::OnScreenKeyboardShown(bool shown) {
  if (onscreen_keyboard_shown_ == shown)
    return;

  onscreen_keyboard_shown_ = shown;
  if (shown && GetAppsContainerView()->IsInFolderView()) {
    // Move the app list up to prevent folders being blocked by the
    // on-screen keyboard.
    OffsetYPositionOfAppList(
        GetAppsContainerView()->app_list_folder_view()->GetYOffsetForFolder());
  } else {
    // If the keyboard is closing or a folder isn't being shown, reset
    // the app list's position
    OffsetYPositionOfAppList(0);
  }
  app_list_main_view_->contents_view()->NotifySearchBoxBoundsUpdated();
}

bool AppListView::CloseKeyboardIfVisible() {
  // TODO(ginko) abstract this function to be in |keyboard::KeyboardController|
  if (!keyboard::KeyboardController::HasInstance())
    return false;
  auto* const keyboard_controller = keyboard::KeyboardController::Get();
  if (keyboard_controller->IsKeyboardVisible()) {
    keyboard_controller->HideKeyboardByUser();
    return true;
  }
  return false;
}

void AppListView::OnParentWindowBoundsChanged() {
  // Set the |fullscreen_widget_| size to fit the new display metrics.
  fullscreen_widget_->GetNativeView()->SetBounds(
      GetPreferredWidgetBoundsForState(app_list_state_));

  // Update the |fullscreen_widget_| bounds to accomodate the new work
  // area.
  SetState(app_list_state_);
}

float AppListView::GetAppListBackgroundOpacityDuringDragging() {
  float top_of_applist = fullscreen_widget_->GetWindowBoundsInScreen().y();
  const int shelf_height = AppListConfig::instance().shelf_height();
  float dragging_height =
      std::max((GetScreenBottom() - shelf_height - top_of_applist), 0.f);
  float coefficient =
      std::min(dragging_height / (kNumOfShelfSize * shelf_height), 1.0f);
  float shield_opacity =
      is_background_blur_enabled_ ? kAppListOpacityWithBlur : kAppListOpacity;
  // Assume shelf is opaque when start to drag down the launcher.
  const float shelf_opacity = 1.0f;
  return coefficient * shield_opacity + (1 - coefficient) * shelf_opacity;
}

void AppListView::GetWallpaperProminentColors(
    AppListViewDelegate::GetWallpaperProminentColorsCallback callback) {
  delegate_->GetWallpaperProminentColors(std::move(callback));
}

void AppListView::SetBackgroundShieldColor() {
  // There is a chance when AppListView::OnWallpaperColorsChanged is called
  // from AppListViewDelegate, the |app_list_background_shield_| is not
  // initialized.
  if (!app_list_background_shield_)
    return;

  // Opacity is set on the color instead of the layer because changing opacity
  // of the layer changes opacity of the blur effect, which is not desired.
  float color_opacity = kAppListOpacity;

  if (is_tablet_mode_) {
    // The Homecher background should have an opacity of 0.
    color_opacity = 0;
  } else if (is_in_drag_) {
    // Allow a custom opacity while the AppListView is dragging to show a
    // gradual opacity change when dragging from the shelf.
    color_opacity = background_opacity_in_drag_;
  } else if (is_background_blur_enabled_) {
    color_opacity = kAppListOpacityWithBlur;
  }

  GetWallpaperProminentColors(base::BindOnce(
      [](base::WeakPtr<AppListView> self, float color_opacity,
         const std::vector<SkColor>& prominent_colors) {
        self->app_list_background_shield_->UpdateColor(
            GetBackgroundShieldColor(prominent_colors, color_opacity));
      },
      weak_ptr_factory_.GetWeakPtr(), color_opacity));
}

void AppListView::RecordFolderMetrics() {
  int number_of_apps_in_folders = 0;
  int number_of_folders = 0;
  AppListItemList* item_list =
      app_list_main_view_->model()->top_level_item_list();
  for (size_t i = 0; i < item_list->item_count(); ++i) {
    AppListItem* item = item_list->item_at(i);
    if (item->GetItemType() != AppListFolderItem::kItemType)
      continue;
    ++number_of_folders;
    AppListFolderItem* folder = static_cast<AppListFolderItem*>(item);
    if (folder->folder_type() == AppListFolderItem::FOLDER_TYPE_OEM)
      continue;  // Don't count items in OEM folders.
    number_of_apps_in_folders += folder->item_list()->item_count();
  }
  UMA_HISTOGRAM_COUNTS_100(kNumberOfFoldersHistogram, number_of_folders);
  UMA_HISTOGRAM_COUNTS_100(kNumberOfAppsInFoldersHistogram,
                           number_of_apps_in_folders);
}

bool AppListView::ShouldIgnoreScrollEvents() {
  // When the app list is doing state change animation or the apps grid view is
  // in transition, ignore the scroll events to prevent triggering extra state
  // changes or transtions.
  return fullscreen_widget_->GetLayer()->GetAnimator()->is_animating() ||
         GetRootAppsGridView()->pagination_model()->has_transition();
}

int AppListView::GetPreferredWidgetYForState(
    ash::mojom::AppListViewState state) {
  // Note that app list container fills the screen, so we can treat the
  // container's y as the top of display.
  const display::Display display = GetDisplayNearestView();
  const gfx::Rect work_area_bounds = display.work_area();
  switch (state) {
    case ash::mojom::AppListViewState::kPeeking:
      return display.bounds().height() -
             AppListConfig::instance().peeking_app_list_height();
    case ash::mojom::AppListViewState::kHalf:
      return std::max(work_area_bounds.y(),
                      display.bounds().height() - kHalfAppListHeight);
    case ash::mojom::AppListViewState::kFullscreenAllApps:
    case ash::mojom::AppListViewState::kFullscreenSearch:
      // The ChromeVox panel as well as the Docked Magnifier viewport affect the
      // workarea of the display. We need to account for that when applist is in
      // fullscreen to avoid being shown below them.
      return work_area_bounds.y() - display.bounds().y();
    case ash::mojom::AppListViewState::kClosed:
      // Align the widget y with shelf y to avoid flicker in show animation. In
      // side shelf mode, the widget y is the top of work area because the
      // widget does not animate.
      return (is_side_shelf_ ? work_area_bounds.y()
                             : work_area_bounds.bottom()) -
             display.bounds().y();
  }
}

gfx::Rect AppListView::GetPreferredWidgetBoundsForState(
    ash::mojom::AppListViewState state) {
  // Use parent's width instead of display width to avoid 1 px gap (See
  // https://crbug.com/884889).
  CHECK(fullscreen_widget_);
  aura::Window* parent = fullscreen_widget_->GetNativeView()->parent();
  CHECK(parent);
  return gfx::Rect(0, GetPreferredWidgetYForState(state),
                   parent->bounds().width(), GetFullscreenStateHeight());
}

void AppListView::UpdateAppListBackgroundYPosition() {
  // Update the y position of the background shield.
  gfx::Transform transform;
  if (is_in_drag_) {
    float app_list_transition_progress = GetAppListTransitionProgress();
    if (app_list_transition_progress >= 1 &&
        app_list_transition_progress <= 2) {
      // Translate background shield so that it ends drag at y position
      // -|kAppListBackgroundRadius| when dragging between peeking and
      // fullscreen.
      transform.Translate(
          0, -kAppListBackgroundRadius * (app_list_transition_progress - 1));
    }
  } else if (is_fullscreen()) {
    transform.Translate(0, -kAppListBackgroundRadius);
  }
  app_list_background_shield_->SetTransform(transform);
}

void AppListView::OnStateTransitionAnimationCompleted() {
  delegate_->OnStateTransitionAnimationCompleted(app_list_state_);
}

}  // namespace app_list
