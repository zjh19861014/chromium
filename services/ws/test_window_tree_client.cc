// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/ws/test_window_tree_client.h"

#include <utility>

#include "services/ws/window_tree.h"
#include "services/ws/window_tree_test_helper.h"
#include "ui/aura/window.h"
#include "ui/gfx/geometry/rect.h"

namespace ws {

TestWindowTreeClient::InputEvent::InputEvent() = default;

TestWindowTreeClient::InputEvent::InputEvent(InputEvent&& other) = default;

TestWindowTreeClient::InputEvent::~InputEvent() = default;

TestWindowTreeClient::TestWindowTreeClient() {
  tracker_.set_delegate(this);
}

TestWindowTreeClient::~TestWindowTreeClient() = default;

TestWindowTreeClient::InputEvent TestWindowTreeClient::PopInputEvent() {
  if (input_events_.empty())
    return InputEvent();

  InputEvent event = std::move(input_events_.front());
  input_events_.pop();
  return event;
}

void TestWindowTreeClient::ClearInputEvents() {
  input_events_ = std::queue<InputEvent>();
}

std::unique_ptr<ui::Event> TestWindowTreeClient::PopObservedEvent() {
  if (observed_events_.empty())
    return nullptr;

  std::unique_ptr<ui::Event> event = std::move(observed_events_.front());
  observed_events_.pop();
  return event;
}

void TestWindowTreeClient::SetWindowTree(mojom::WindowTreePtr tree) {
  DCHECK(!tree_);
  tree_ = std::move(tree);
}

bool TestWindowTreeClient::AckFirstEvent(WindowTree* tree,
                                         mojom::EventResult result) {
  if (input_events_.empty())
    return false;
  InputEvent input_event = PopInputEvent();
  WindowTreeTestHelper(tree).OnWindowInputEventAck(input_event.event_id,
                                                   result);
  return true;
}

void TestWindowTreeClient::OnChangeAdded() {}

void TestWindowTreeClient::OnClientId(uint32_t client_id) {}

void TestWindowTreeClient::OnEmbed(
    mojom::WindowDataPtr root,
    mojom::WindowTreePtr tree,
    int64_t display_id,
    Id focused_window_id,
    bool drawn,
    const base::Optional<viz::LocalSurfaceIdAllocation>&
        local_surface_id_allocation) {
  root_window_id_ = root->window_id;
  tree_ = std::move(tree);
  tracker_.OnEmbed(std::move(root), drawn);
}

void TestWindowTreeClient::OnEmbedFromToken(
    const base::UnguessableToken& token,
    mojom::WindowDataPtr root,
    int64_t display_id,
    const base::Optional<viz::LocalSurfaceIdAllocation>&
        local_surface_id_allocation) {
  tracker_.OnEmbedFromToken(
      std::move(root), display_id,
      local_surface_id_allocation
          ? local_surface_id_allocation->local_surface_id()
          : viz::LocalSurfaceId());
}

void TestWindowTreeClient::OnEmbeddedAppDisconnected(Id window_id) {
  tracker_.OnEmbeddedAppDisconnected(window_id);
}

void TestWindowTreeClient::OnUnembed(Id window_id) {
  tracker_.OnUnembed(window_id);
}

void TestWindowTreeClient::OnCaptureChanged(Id new_capture_window_id,
                                            Id old_capture_window_id) {
  tracker_.OnCaptureChanged(new_capture_window_id, old_capture_window_id);
}

void TestWindowTreeClient::OnFrameSinkIdAllocated(
    Id window_id,
    const viz::FrameSinkId& frame_sink_id) {
  tracker_.OnFrameSinkIdAllocated(window_id, frame_sink_id);
}

void TestWindowTreeClient::OnTopLevelCreated(
    uint32_t change_id,
    mojom::WindowDataPtr data,
    int64_t display_id,
    bool drawn,
    const viz::LocalSurfaceIdAllocation& local_surface_id_allocation) {
  tracker_.OnTopLevelCreated(change_id, std::move(data), display_id, drawn,
                             local_surface_id_allocation);
}

void TestWindowTreeClient::OnWindowBoundsChanged(
    Id window_id,
    const gfx::Rect& new_bounds,
    const base::Optional<viz::LocalSurfaceIdAllocation>&
        local_surface_id_allocation) {
  // The bounds of the root may change during startup on Android at random
  // times. As this doesn't matter, and shouldn't impact test exepctations,
  // it is ignored.
  if (window_id == root_window_id_ && !track_root_bounds_changes_)
    return;
  tracker_.OnWindowBoundsChanged(window_id, new_bounds,
                                 local_surface_id_allocation);
}

void TestWindowTreeClient::OnWindowTransformChanged(
    Id window_id,
    const gfx::Transform& new_transform) {
  tracker_.OnWindowTransformChanged(window_id);
}

void TestWindowTreeClient::OnTransientWindowAdded(Id window_id,
                                                  Id transient_window_id) {
  tracker_.OnTransientWindowAdded(window_id, transient_window_id);
}

void TestWindowTreeClient::OnTransientWindowRemoved(Id window_id,
                                                    Id transient_window_id) {
  tracker_.OnTransientWindowRemoved(window_id, transient_window_id);
}

void TestWindowTreeClient::OnWindowHierarchyChanged(
    Id window,
    Id old_parent,
    Id new_parent,
    std::vector<mojom::WindowDataPtr> windows) {
  tracker_.OnWindowHierarchyChanged(window, old_parent, new_parent,
                                    std::move(windows));
}

void TestWindowTreeClient::OnWindowReordered(Id window_id,
                                             Id relative_window_id,
                                             mojom::OrderDirection direction) {
  tracker_.OnWindowReordered(window_id, relative_window_id, direction);
}

void TestWindowTreeClient::OnWindowDeleted(Id window) {
  tracker_.OnWindowDeleted(window);
}

void TestWindowTreeClient::OnWindowVisibilityChanged(Id window, bool visible) {
  tracker_.OnWindowVisibilityChanged(window, visible);
}

void TestWindowTreeClient::OnWindowDisplayChanged(Id window_id,
                                                  int64_t display_id) {
  tracker_.OnWindowDisplayChanged(window_id, display_id);
}

void TestWindowTreeClient::OnWindowParentDrawnStateChanged(Id window,
                                                           bool drawn) {
  tracker_.OnWindowParentDrawnStateChanged(window, drawn);
}

void TestWindowTreeClient::OnWindowInputEvent(uint32_t event_id,
                                              Id window_id,
                                              int64_t display_id,
                                              std::unique_ptr<ui::Event> event,
                                              bool matches_event_observer) {
  tracker_.OnWindowInputEvent(window_id, *event, display_id,
                              matches_event_observer);

  InputEvent input_event;
  input_event.event_id = event_id;
  input_event.window_id = window_id;
  input_event.display_id = display_id;
  input_event.event = std::move(event);
  input_event.matches_event_observer = matches_event_observer;
  input_events_.push(std::move(input_event));

  if (tree_)
    tree_->OnWindowInputEventAck(event_id, mojom::EventResult::HANDLED);
}

void TestWindowTreeClient::OnObservedInputEvent(
    std::unique_ptr<ui::Event> event) {
  tracker_.OnObservedInputEvent(*event);
  observed_events_.push(std::move(event));
}

void TestWindowTreeClient::OnWindowSharedPropertyChanged(
    Id window,
    const std::string& name,
    const base::Optional<std::vector<uint8_t>>& new_data) {
  tracker_.OnWindowSharedPropertyChanged(window, name, new_data);
}

void TestWindowTreeClient::OnWindowFocused(Id focused_window_id) {
  tracker_.OnWindowFocused(focused_window_id);
}

void TestWindowTreeClient::OnWindowCursorChanged(Id window_id,
                                                 ui::Cursor cursor) {
  tracker_.OnWindowCursorChanged(window_id, cursor);
}

void TestWindowTreeClient::OnDragDropStart(
    const base::flat_map<std::string, std::vector<uint8_t>>& drag_data) {
  tracker_.OnDragDropStart(drag_data);
}

void TestWindowTreeClient::OnDragEnter(Id window,
                                       uint32_t key_state,
                                       const gfx::PointF& location_in_root,
                                       const gfx::PointF& location,
                                       uint32_t effect_bitmask,
                                       OnDragEnterCallback callback) {
  tracker_.OnDragEnter(window);
}

void TestWindowTreeClient::OnDragOver(Id window,
                                      uint32_t key_state,
                                      const gfx::PointF& location_in_root,
                                      const gfx::PointF& location,
                                      uint32_t effect_bitmask,
                                      OnDragOverCallback callback) {
  tracker_.OnDragOver(window);
}

void TestWindowTreeClient::OnDragLeave(Id window) {
  tracker_.OnDragLeave(window);
}

void TestWindowTreeClient::OnCompleteDrop(Id window,
                                          uint32_t key_state,
                                          const gfx::PointF& location_in_root,
                                          const gfx::PointF& location,
                                          uint32_t effect_bitmask,
                                          OnCompleteDropCallback callback) {
  tracker_.OnCompleteDrop(window);
}

void TestWindowTreeClient::OnPerformDragDropCompleted(uint32_t change_id,
                                                      bool success,
                                                      uint32_t action_taken) {
  tracker_.OnPerformDragDropCompleted(change_id, success, action_taken);
}

void TestWindowTreeClient::OnDragDropDone() {
  tracker_.OnDragDropDone();
}

void TestWindowTreeClient::OnTopmostWindowChanged(
    const std::vector<Id>& topmost_ids) {
  tracker_.OnTopmostWindowChanged(topmost_ids);
}

void TestWindowTreeClient::OnChangeCompleted(uint32_t change_id, bool success) {
  tracker_.OnChangeCompleted(change_id, success);
}

void TestWindowTreeClient::RequestClose(Id window_id) {
  tracker_.RequestClose(window_id);
}

void TestWindowTreeClient::GetScreenProviderObserver(
    mojom::ScreenProviderObserverAssociatedRequest observer) {
  screen_provider_observer_binding_.Bind(std::move(observer));
}

void TestWindowTreeClient::OnOcclusionStatesChanged(
    const base::flat_map<Id, mojom::OcclusionState>& occlusion_changes) {
  tracker_.OnOcclusionStatesChanged(occlusion_changes);
}

void TestWindowTreeClient::CleanupGestureState(Id window_id) {}

}  // namespace ws
