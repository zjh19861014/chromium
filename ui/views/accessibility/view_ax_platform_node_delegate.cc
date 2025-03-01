// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/accessibility/view_ax_platform_node_delegate.h"

#include <map>
#include <memory>

#include "base/bind.h"
#include "base/lazy_instance.h"
#include "base/threading/thread_task_runner_handle.h"
#include "ui/accessibility/ax_action_data.h"
#include "ui/accessibility/ax_role_properties.h"
#include "ui/accessibility/ax_tree_data.h"
#include "ui/accessibility/platform/ax_platform_node.h"
#include "ui/accessibility/platform/ax_platform_node_base.h"
#include "ui/accessibility/platform/ax_unique_id.h"
#include "ui/events/event_utils.h"
#include "ui/views/accessibility/view_accessibility_utils.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace views {

namespace {

// Information required to fire a delayed accessibility event.
struct QueuedEvent {
  QueuedEvent(ax::mojom::Event type, int32_t node_id)
      : type(type), node_id(node_id) {}

  ax::mojom::Event type;
  int32_t node_id;
};

base::LazyInstance<std::vector<QueuedEvent>>::Leaky g_event_queue =
    LAZY_INSTANCE_INITIALIZER;

bool g_is_queueing_events = false;

bool IsAccessibilityFocusableWhenEnabled(View* view) {
  return view->focus_behavior() != View::FocusBehavior::NEVER &&
         view->IsDrawn();
}

// Used to determine if a View should be ignored by accessibility clients by
// being a non-keyboard-focusable child of a keyboard-focusable ancestor. E.g.,
// LabelButtons contain Labels, but a11y should just show that there's a button.
bool IsViewUnfocusableDescendantOfFocusableAncestor(View* view) {
  if (IsAccessibilityFocusableWhenEnabled(view))
    return false;

  while (view->parent()) {
    view = view->parent();
    if (IsAccessibilityFocusableWhenEnabled(view))
      return true;
  }
  return false;
}

ui::AXPlatformNode* FromNativeWindow(gfx::NativeWindow native_window) {
  Widget* widget = Widget::GetWidgetForNativeWindow(native_window);
  if (!widget)
    return nullptr;

  View* view = widget->GetRootView();
  if (!view)
    return nullptr;

  gfx::NativeViewAccessible native_view_accessible =
      view->GetNativeViewAccessible();
  if (!native_view_accessible)
    return nullptr;

  return ui::AXPlatformNode::FromNativeViewAccessible(native_view_accessible);
}

ui::AXPlatformNode* PlatformNodeFromNodeID(int32_t id) {
  // Note: For Views, node IDs and unique IDs are the same - but that isn't
  // necessarily true for all AXPlatformNodes.
  return ui::AXPlatformNodeBase::GetFromUniqueId(id);
}

void FireEvent(QueuedEvent event) {
  ui::AXPlatformNode* node = PlatformNodeFromNodeID(event.node_id);
  if (node)
    node->NotifyAccessibilityEvent(event.type);
}

void FlushQueue() {
  DCHECK(g_is_queueing_events);
  for (QueuedEvent event : g_event_queue.Get())
    FireEvent(event);
  g_is_queueing_events = false;
  g_event_queue.Get().clear();
}

}  // namespace

struct ViewAXPlatformNodeDelegate::ChildWidgetsResult {
  std::vector<Widget*> child_widgets;

  // Set to true if, instead of populating |child_widgets| normally, a single
  // child widget was returned (e.g. a dialog that should be read instead of
  // the rest of the page contents).
  bool is_tab_modal_showing;
};

// static
int ViewAXPlatformNodeDelegate::menu_depth_ = 0;

ViewAXPlatformNodeDelegate::ViewAXPlatformNodeDelegate(View* view)
    : ViewAccessibility(view) {
  ax_platform_node_ = ui::AXPlatformNode::Create(this);
  DCHECK(ax_platform_node_);

  static bool first_time = true;
  if (first_time) {
    ui::AXPlatformNode::RegisterNativeWindowHandler(
        base::BindRepeating(&FromNativeWindow));
    first_time = false;
  }
}

ViewAXPlatformNodeDelegate::~ViewAXPlatformNodeDelegate() {
  if (ui::AXPlatformNode::GetPopupFocusOverride() == GetNativeObject())
    ui::AXPlatformNode::SetPopupFocusOverride(nullptr);
  ax_platform_node_->Destroy();
}

gfx::NativeViewAccessible ViewAXPlatformNodeDelegate::GetNativeObject() {
  DCHECK(ax_platform_node_);
  return ax_platform_node_->GetNativeViewAccessible();
}

void ViewAXPlatformNodeDelegate::NotifyAccessibilityEvent(
    ax::mojom::Event event_type) {
  DCHECK(ax_platform_node_);
  if (g_is_queueing_events) {
    g_event_queue.Get().emplace_back(event_type, GetUniqueId());
    return;
  }

  // Some events have special handling.
  switch (event_type) {
    case ax::mojom::Event::kMenuStart:
      OnMenuStart();
      break;
    case ax::mojom::Event::kMenuEnd:
      OnMenuEnd();
      break;
    case ax::mojom::Event::kSelection:
      if (menu_depth_ && ui::IsMenuItem(GetData().role))
        OnMenuItemActive();
      break;
    case ax::mojom::Event::kFocusContext: {
      // A focus context event is intended to send a focus event and a delay
      // before the next focus event. It makes sense to delay the entire next
      // synchronous batch of next events so that ordering remains the same.
      // Begin queueing subsequent events and flush queue asynchronously.
      g_is_queueing_events = true;
      base::OnceCallback<void()> cb = base::BindOnce(&FlushQueue);
      base::ThreadTaskRunnerHandle::Get()->PostTask(FROM_HERE, std::move(cb));
      break;
    }
    default:
      break;
  }

  // Fire events here now that our internal state is up-to-date.
  ax_platform_node_->NotifyAccessibilityEvent(event_type);
}

#if defined(OS_MACOSX)
void ViewAXPlatformNodeDelegate::AnnounceText(base::string16& text) {
  ax_platform_node_->AnnounceText(text);
}
#endif

void ViewAXPlatformNodeDelegate::OnMenuItemActive() {
  // When a native menu is shown and has an item selected, treat it and the
  // currently selected item as focused, even though the actual focus is in the
  // browser's currently focused textfield.
  ui::AXPlatformNode::SetPopupFocusOverride(
      ax_platform_node_->GetNativeViewAccessible());
}

void ViewAXPlatformNodeDelegate::OnMenuStart() {
  ++menu_depth_;
}

void ViewAXPlatformNodeDelegate::OnMenuEnd() {
  // When a native menu is hidden, restore accessibility focus to the current
  // focus in the document.
  if (menu_depth_ >= 1)
    --menu_depth_;
  if (menu_depth_ == 0)
    ui::AXPlatformNode::SetPopupFocusOverride(nullptr);
}

// ui::AXPlatformNodeDelegate

const ui::AXNodeData& ViewAXPlatformNodeDelegate::GetData() const {
  // Clear the data, then populate it.
  data_ = ui::AXNodeData();
  GetAccessibleNodeData(&data_);

  // View::IsDrawn is true if a View is visible and all of its ancestors are
  // visible too, since invisibility inherits.
  //
  // TODO(dmazzoni): Maybe consider moving this to ViewAccessibility?
  // This will require ensuring that Chrome OS invalidates the whole
  // subtree when a View changes its visibility state.
  if (!view()->IsDrawn())
    data_.AddState(ax::mojom::State::kInvisible);

  // Make sure this element is excluded from the a11y tree if there's a
  // focusable parent. All keyboard focusable elements should be leaf nodes.
  // Exceptions to this rule will themselves be accessibility focusable.
  //
  // TODO(dmazzoni): this code was added to support MacViews acccessibility,
  // because we needed a way to mark a View as a leaf node in the
  // accessibility tree. We need to replace this with a cross-platform
  // solution that works for ChromeVox, too, and move it to ViewAccessibility.
  if (IsViewUnfocusableDescendantOfFocusableAncestor(view()))
    data_.role = ax::mojom::Role::kIgnored;

  return data_;
}

int ViewAXPlatformNodeDelegate::GetChildCount() {
  if (IsLeaf())
    return 0;

  if (virtual_child_count())
    return virtual_child_count();

  const auto child_widgets_result = GetChildWidgets();
  if (child_widgets_result.is_tab_modal_showing) {
    DCHECK_EQ(child_widgets_result.child_widgets.size(), 1U);
    return 1;
  }
  return static_cast<int>(view()->children().size() +
                          child_widgets_result.child_widgets.size());
}

gfx::NativeViewAccessible ViewAXPlatformNodeDelegate::ChildAtIndex(int index) {
  DCHECK_GE(index, 0) << "Child indices should be greater or equal to 0.";
  DCHECK_LT(index, GetChildCount())
      << "Child indices should be less than the child count.";
  if (IsLeaf())
    return nullptr;

  if (virtual_child_count())
    return virtual_child_at(index)->GetNativeObject();

  // If this is a root view, our widget might have child widgets. Include
  const auto child_widgets_result = GetChildWidgets();
  const auto& child_widgets = child_widgets_result.child_widgets;

  // If a visible tab modal dialog is present, ignore |index| and return the
  // dialog.
  if (child_widgets_result.is_tab_modal_showing) {
    DCHECK_EQ(child_widgets.size(), 1U);
    return child_widgets[0]->GetRootView()->GetNativeViewAccessible();
  }

  size_t child_index = size_t{index};
  if (child_index < view()->children().size())
    return view()->children()[child_index]->GetNativeViewAccessible();

  child_index -= view()->children().size();
  if (child_index < child_widgets_result.child_widgets.size())
    return child_widgets[child_index]->GetRootView()->GetNativeViewAccessible();

  return nullptr;
}

gfx::NativeViewAccessible ViewAXPlatformNodeDelegate::GetNSWindow() {
  NOTREACHED();
  return nullptr;
}

gfx::NativeViewAccessible ViewAXPlatformNodeDelegate::GetParent() {
  if (view()->parent())
    return view()->parent()->GetNativeViewAccessible();

  if (Widget* widget = view()->GetWidget()) {
    Widget* top_widget = widget->GetTopLevelWidget();
    if (top_widget && widget != top_widget && top_widget->GetRootView())
      return top_widget->GetRootView()->GetNativeViewAccessible();
  }

  return nullptr;
}

gfx::Rect ViewAXPlatformNodeDelegate::GetBoundsRect(
    const ui::AXCoordinateSystem coordinate_system,
    const ui::AXClippingBehavior clipping_behavior,
    ui::AXOffscreenResult* offscreen_result) const {
  switch (coordinate_system) {
    case ui::AXCoordinateSystem::kScreen:
      // We could optionally add clipping here if ever needed.
      return view()->GetBoundsInScreen();
    case ui::AXCoordinateSystem::kRootFrame:
    case ui::AXCoordinateSystem::kFrame:
      NOTIMPLEMENTED();
      return gfx::Rect();
  }
}

gfx::NativeViewAccessible ViewAXPlatformNodeDelegate::HitTestSync(int x,
                                                                  int y) {
  if (!view() || !view()->GetWidget())
    return nullptr;

  if (IsLeaf())
    return GetNativeObject();

  // Search child widgets first, since they're on top in the z-order.
  for (Widget* child_widget : GetChildWidgets().child_widgets) {
    View* child_root_view = child_widget->GetRootView();
    gfx::Point point(x, y);
    View::ConvertPointFromScreen(child_root_view, &point);
    if (child_root_view->HitTestPoint(point))
      return child_root_view->GetNativeViewAccessible();
  }

  gfx::Point point(x, y);
  View::ConvertPointFromScreen(view(), &point);
  if (!view()->HitTestPoint(point))
    return nullptr;

  // Check if the point is within any of the immediate children of this
  // view. We don't have to search further because AXPlatformNode will
  // do a recursive hit test if we return anything other than |this| or NULL.
  View* v = view();
  const auto is_point_in_child = [point, v](View* child) {
    if (!child->visible())
      return false;
    gfx::Point point_in_child_coords = point;
    v->ConvertPointToTarget(v, child, &point_in_child_coords);
    return child->HitTestPoint(point_in_child_coords);
  };
  const auto i = std::find_if(v->children().rbegin(), v->children().rend(),
                              is_point_in_child);
  // If it's not inside any of our children, it's inside this view.
  return (i == v->children().rend()) ? GetNativeObject()
                                     : (*i)->GetNativeViewAccessible();
}

gfx::NativeViewAccessible ViewAXPlatformNodeDelegate::GetFocus() {
  gfx::NativeViewAccessible focus_override =
      ui::AXPlatformNode::GetPopupFocusOverride();
  if (focus_override)
    return focus_override;

  FocusManager* focus_manager = view()->GetFocusManager();
  View* focused_view =
      focus_manager ? focus_manager->GetFocusedView() : nullptr;

  if (!focused_view)
    return nullptr;

  // The accessibility focus will be either on the |focused_view| or on one of
  // its virtual children.
  return focused_view->GetViewAccessibility().GetFocusedDescendant();
}

ui::AXPlatformNode* ViewAXPlatformNodeDelegate::GetFromNodeID(int32_t id) {
  return PlatformNodeFromNodeID(id);
}

bool ViewAXPlatformNodeDelegate::AccessibilityPerformAction(
    const ui::AXActionData& data) {
  return view()->HandleAccessibleAction(data);
}

bool ViewAXPlatformNodeDelegate::ShouldIgnoreHoveredStateForTesting() {
  return false;
}

bool ViewAXPlatformNodeDelegate::IsOffscreen() const {
  // TODO: need to implement.
  return false;
}

const ui::AXUniqueId& ViewAXPlatformNodeDelegate::GetUniqueId() const {
  return ViewAccessibility::GetUniqueId();
}

ViewAXPlatformNodeDelegate::ChildWidgetsResult
ViewAXPlatformNodeDelegate::GetChildWidgets() const {
  // Only attach child widgets to the root view.
  Widget* widget = view()->GetWidget();
  // Note that during window close, a Widget may exist in a state where it has
  // no NativeView, but hasn't yet torn down its view hierarchy.
  if (!widget || !widget->GetNativeView() || widget->GetRootView() != view())
    return {{}, false};

  std::set<Widget*> owned_widgets;
  Widget::GetAllOwnedWidgets(widget->GetNativeView(), &owned_widgets);

  std::vector<Widget*> visible_widgets;
  const auto visible = [widget](const Widget* child) {
    return child->IsVisible() &&
           // TODO(dmazzoni): Shouldn't this be |child|?
           !widget->GetNativeWindowProperty(kWidgetNativeViewHostKey);
  };
  std::copy_if(owned_widgets.cbegin(), owned_widgets.cend(),
               std::back_inserter(visible_widgets), visible);

  // Focused child widgets should take the place of the web page they cover in
  // the accessibility tree.
  const FocusManager* focus_manager = view()->GetFocusManager();
  const View* focused_view =
      focus_manager ? focus_manager->GetFocusedView() : nullptr;
  const auto is_focused_child = [focused_view](Widget* child) {
    return ViewAccessibilityUtils::IsFocusedChildWidget(child, focused_view);
  };
  const auto i = std::find_if(visible_widgets.cbegin(), visible_widgets.cend(),
                              is_focused_child);
  if (i != visible_widgets.cend())
    return {{*i}, true};

  return {visible_widgets, false};
}

}  // namespace views
