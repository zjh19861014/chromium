// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/performance_manager/graph/process_node_impl.h"

#include "base/logging.h"
#include "chrome/browser/performance_manager/graph/frame_node_impl.h"
#include "chrome/browser/performance_manager/graph/page_node_impl.h"

namespace performance_manager {

ProcessNodeImpl::ProcessNodeImpl(Graph* graph)
    : CoordinationUnitInterface(graph) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

ProcessNodeImpl::~ProcessNodeImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void ProcessNodeImpl::AddFrame(FrameNodeImpl* frame_node) {
  const bool inserted = frame_nodes_.insert(frame_node).second;
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(inserted);
  if (frame_node->lifecycle_state() ==
      resource_coordinator::mojom::LifecycleState::kFrozen)
    IncrementNumFrozenFrames();
}

void ProcessNodeImpl::SetCPUUsage(double cpu_usage) {
  cpu_usage_ = cpu_usage;
}

void ProcessNodeImpl::SetExpectedTaskQueueingDuration(
    base::TimeDelta duration) {
  expected_task_queueing_duration_.SetAndNotify(this, duration);
}

void ProcessNodeImpl::SetMainThreadTaskLoadIsLow(
    bool main_thread_task_load_is_low) {
  main_thread_task_load_is_low_.SetAndMaybeNotify(this,
                                                  main_thread_task_load_is_low);
}

void ProcessNodeImpl::SetProcessExitStatus(int32_t exit_status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // This may occur as the first event seen in the case where the process
  // fails to start or suffers a startup crash.
  exit_status_ = exit_status;

  // Close the process handle to kill the zombie.
  process_.Close();
}

void ProcessNodeImpl::SetProcess(base::Process process,
                                 base::Time launch_time) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(process.IsValid());
  // Either this is the initial process associated with this process CU,
  // or it's a subsequent process. In the latter case, there must have been
  // an exit status associated with the previous process.
  DCHECK(!process_.IsValid() || exit_status_.has_value());

  base::ProcessId pid = process.Pid();
  SetProcessImpl(std::move(process), pid, launch_time);
}

void ProcessNodeImpl::OnRendererIsBloated() {
  SendEvent(resource_coordinator::mojom::Event::kRendererIsBloated);
}

const base::flat_set<FrameNodeImpl*>& ProcessNodeImpl::GetFrameNodes() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return frame_nodes_;
}

// There is currently not a direct relationship between processes and
// pages. However, frames are children of both processes and frames, so we
// find all of the pages that are reachable from the process's child
// frames.
base::flat_set<PageNodeImpl*>
ProcessNodeImpl::GetAssociatedPageCoordinationUnits() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::flat_set<PageNodeImpl*> page_nodes;
  for (auto* frame_node : frame_nodes_) {
    if (auto* page_node = frame_node->page_node())
      page_nodes.insert(page_node);
  }
  return page_nodes;
}

void ProcessNodeImpl::OnFrameLifecycleStateChanged(
    FrameNodeImpl* frame_node,
    resource_coordinator::mojom::LifecycleState old_state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(base::ContainsKey(frame_nodes_, frame_node));
  DCHECK_NE(old_state, frame_node->lifecycle_state());

  if (old_state == resource_coordinator::mojom::LifecycleState::kFrozen)
    DecrementNumFrozenFrames();
  else if (frame_node->lifecycle_state() ==
           resource_coordinator::mojom::LifecycleState::kFrozen)
    IncrementNumFrozenFrames();
}

void ProcessNodeImpl::SetProcessImpl(base::Process process,
                                     base::ProcessId new_pid,
                                     base::Time launch_time) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  graph()->BeforeProcessPidChange(this, new_pid);

  process_ = std::move(process);
  process_id_ = new_pid;
  launch_time_ = launch_time;

  // Clear the exit status for the previous process (if any).
  exit_status_.reset();

  // Also clear the measurement data (if any), as it references the previous
  // process.
  private_footprint_kb_ = 0;
  cumulative_cpu_usage_ = base::TimeDelta();
}

void ProcessNodeImpl::LeaveGraph() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  NodeBase::LeaveGraph();

  // Make as if we're transitioning to the null PID before we die to clear this
  // instance from the PID map.
  if (process_id_ != base::kNullProcessId)
    graph()->BeforeProcessPidChange(this, base::kNullProcessId);

  // All child frames should have been removed before the process is removed.
  DCHECK(frame_nodes_.empty());
}

void ProcessNodeImpl::OnEventReceived(
    resource_coordinator::mojom::Event event) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (auto& observer : observers())
    observer.OnProcessEventReceived(this, event);
}

void ProcessNodeImpl::RemoveFrame(FrameNodeImpl* frame_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(base::ContainsKey(frame_nodes_, frame_node));
  frame_nodes_.erase(frame_node);

  if (frame_node->lifecycle_state() ==
      resource_coordinator::mojom::LifecycleState::kFrozen)
    DecrementNumFrozenFrames();
}

void ProcessNodeImpl::DecrementNumFrozenFrames() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  --num_frozen_frames_;
  DCHECK_GE(num_frozen_frames_, 0);
}

void ProcessNodeImpl::IncrementNumFrozenFrames() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ++num_frozen_frames_;
  DCHECK_LE(num_frozen_frames_, static_cast<int>(frame_nodes_.size()));

  if (num_frozen_frames_ == static_cast<int>(frame_nodes_.size())) {
    for (auto& observer : observers())
      observer.OnAllFramesInProcessFrozen(this);
  }
}

}  // namespace performance_manager
