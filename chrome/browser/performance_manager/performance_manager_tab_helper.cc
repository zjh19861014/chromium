// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/performance_manager/performance_manager_tab_helper.h"

#include <type_traits>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/stl_util.h"
#include "chrome/browser/performance_manager/graph/frame_node_impl.h"
#include "chrome/browser/performance_manager/graph/page_node_impl.h"
#include "chrome/browser/performance_manager/graph/process_node_impl.h"
#include "chrome/browser/performance_manager/performance_manager.h"
#include "chrome/browser/performance_manager/render_process_user_data.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace performance_manager {

PerformanceManagerTabHelper* PerformanceManagerTabHelper::first_ = nullptr;

// static
bool PerformanceManagerTabHelper::GetCoordinationIDForWebContents(
    content::WebContents* web_contents,
    resource_coordinator::CoordinationUnitID* id) {
  PerformanceManagerTabHelper* helper = FromWebContents(web_contents);
  if (!helper)
    return false;
  *id = helper->page_node_->id();

  return true;
}

// static
void PerformanceManagerTabHelper::DetachAndDestroyAll() {
  while (first_)
    first_->web_contents()->RemoveUserData(UserDataKey());
}

PerformanceManagerTabHelper::PerformanceManagerTabHelper(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      performance_manager_(PerformanceManager::GetInstance()),
      weak_factory_(this) {
  page_node_ = performance_manager_->CreatePageNode(weak_factory_.GetWeakPtr());

  // Make sure to set the visibility property when we create
  // |page_resource_coordinator_|.
  UpdatePageNodeVisibility(web_contents->GetVisibility());

  // Dispatch creation notifications for any pre-existing frames.
  std::vector<content::RenderFrameHost*> existing_frames =
      web_contents->GetAllFrames();
  for (content::RenderFrameHost* frame : existing_frames) {
    // Only send notifications for live frames, the non-live ones will generate
    // creation notifications when animated.
    // TODO(siggi): Add an IsRenderFrameCreated accessor to WebContents.
    if (frame->IsRenderFrameLive())
      RenderFrameCreated(frame);
  }

  // Push this instance to the list.
  next_ = first_;
  if (next_)
    next_->prev_ = this;
  prev_ = nullptr;
  first_ = this;
}

PerformanceManagerTabHelper::~PerformanceManagerTabHelper() {
  // Ship our page and frame nodes to the PerformanceManager for incineration.
  std::vector<std::unique_ptr<NodeBase>> nodes;
  nodes.push_back(std::move(page_node_));
  for (auto& kv : frames_)
    nodes.push_back(std::move(kv.second));

  frames_.clear();

  // Delete the page and its entire frame tree from the graph.
  performance_manager_->BatchDeleteNodes(std::move(nodes));

  if (first_ == this)
    first_ = next_;

  if (prev_) {
    DCHECK_EQ(prev_->next_, this);
    prev_->next_ = next_;
  }
  if (next_) {
    DCHECK_EQ(next_->prev_, this);
    next_->prev_ = prev_;
  }
}

void PerformanceManagerTabHelper::RenderFrameCreated(
    content::RenderFrameHost* render_frame_host) {
  DCHECK_NE(nullptr, render_frame_host);
  // This must not exist in the map yet.
  DCHECK(!base::ContainsKey(frames_, render_frame_host));

  content::RenderFrameHost* parent = render_frame_host->GetParent();
  FrameNodeImpl* parent_frame_node = nullptr;
  if (parent) {
    DCHECK(base::ContainsKey(frames_, parent));
    parent_frame_node = frames_[parent].get();
  }

  // Ideally this would strictly be a "Get", but it is possible in tests for
  // the the RenderProcessUserData to not have attached at this point.
  auto* process_node = RenderProcessUserData::GetOrCreateForRenderProcessHost(
                           render_frame_host->GetProcess())
                           ->process_node();

  // Create the frame node, and provide a callback that will run in the graph to
  // initialize it.
  std::unique_ptr<FrameNodeImpl> frame = performance_manager_->CreateFrameNode(
      process_node, page_node_.get(), parent_frame_node,
      render_frame_host->GetFrameTreeNodeId(),
      base::BindOnce(
          [](const GURL& url, bool is_current, FrameNodeImpl* frame_node) {
            frame_node->set_url(url);
            frame_node->SetIsCurrent(is_current);
          },
          render_frame_host->GetLastCommittedURL(),
          render_frame_host->IsCurrent()));

  frames_[render_frame_host] = std::move(frame);
}

void PerformanceManagerTabHelper::RenderFrameDeleted(
    content::RenderFrameHost* render_frame_host) {
  auto it = frames_.find(render_frame_host);
  // Apparently there exists a condition where the construction-time iteration
  // fails to turn up every frame that has been created, and for which there
  // will be an enventual deletion notification. This is because
  // IsRenderFrameLive() will return false if the associated process is dead
  // at the time of query. The process can however be later resurrected, so
  // the condition can't be tested here.
  // See https://crbug.com/948088.
  if (it != frames_.end()) {
    performance_manager_->DeleteNode(std::move(it->second));
    frames_.erase(it);
  }
}

void PerformanceManagerTabHelper::RenderFrameHostChanged(
    content::RenderFrameHost* old_host,
    content::RenderFrameHost* new_host) {
  // |old_host| is null when a new frame tree position is being created and a
  // new frame is its first occupant.
  FrameNodeImpl* old_frame = nullptr;
  if (old_host) {
    auto it = frames_.find(old_host);
    if (it != frames_.end()) {
      // This can be received for a frame that hasn't yet been created. We can
      // safely ignore this. It would be nice to track those frames too, but
      // since they're not yet "created" we'd have no guarantee of seeing a
      // corresponding delete and the frames can be leaked.
      old_frame = it->second.get();
    }
  }

  // It's entirely possible that this is the first time we're seeing this frame.
  // We'll eventually see a corresponding RenderFrameCreated if the frame ends
  // up actually being needed, so we can ignore it until that time. Artificially
  // creating the frame causes problems because we aren't guaranteed to see a
  // subsequent RenderFrameCreated call, meaning we won't see a
  // RenderFrameDeleted, and the frame node will be leaked until process tear
  // down.
  DCHECK(new_host);
  FrameNodeImpl* new_frame = nullptr;
  auto it = frames_.find(new_host);
  if (it != frames_.end())
    new_frame = it->second.get();

  // If neither frame could be looked up there's nothing to do.
  if (!old_frame && !new_frame)
    return;

  // Perform the swap in the graph.
  performance_manager_->task_runner()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](FrameNodeImpl* old_frame, FrameNodeImpl* new_frame) {
                       if (old_frame) {
                         DCHECK(old_frame->is_current());
                         old_frame->SetIsCurrent(false);
                       }
                       if (new_frame) {
                         DCHECK(!new_frame->is_current());
                         new_frame->SetIsCurrent(true);
                       }
                     },
                     base::Unretained(old_frame), base::Unretained(new_frame)));
}

void PerformanceManagerTabHelper::DidStartLoading() {
  PostToGraph(FROM_HERE, &PageNodeImpl::SetIsLoading, page_node_.get(), true);
}

void PerformanceManagerTabHelper::DidStopLoading() {
  PostToGraph(FROM_HERE, &PageNodeImpl::SetIsLoading, page_node_.get(), false);
}

void PerformanceManagerTabHelper::OnVisibilityChanged(
    content::Visibility visibility) {
  UpdatePageNodeVisibility(visibility);
}

void PerformanceManagerTabHelper::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->HasCommitted())
    return;

  // Grab the current time up front, as this is as close as we'll get to the
  // original commit time.
  base::TimeTicks navigation_committed_time = base::TimeTicks::Now();

  // Find the associated frame node.
  content::RenderFrameHost* render_frame_host =
      navigation_handle->GetRenderFrameHost();
  auto frame_it = frames_.find(render_frame_host);
  // TODO(siggi): Ideally this would be a DCHECK, but it seems it's possible
  //     to get a DidFinishNavigation notification for a deleted frame with
  //     the network service.
  if (frame_it == frames_.end())
    return;
  auto* frame_node = frame_it->second.get();

  // Notify the frame of the committed URL.
  GURL url = navigation_handle->GetURL();
  PostToGraph(FROM_HERE, &FrameNodeImpl::set_url, frame_node, url);

  if (navigation_handle->IsSameDocument() ||
      !navigation_handle->IsInMainFrame()) {
    return;
  }

  // Make sure the hierarchical structure is constructed before sending signal
  // to the performance manager.
  OnMainFrameNavigation(navigation_handle->GetNavigationId());
  PostToGraph(FROM_HERE, &PageNodeImpl::OnMainFrameNavigationCommitted,
              page_node_.get(), navigation_committed_time,
              navigation_handle->GetNavigationId(), url);
}

void PerformanceManagerTabHelper::TitleWasSet(content::NavigationEntry* entry) {
  // TODO(siggi): This logic belongs in the policy layer rather than here.
  if (!first_time_title_set_) {
    first_time_title_set_ = true;
    return;
  }
  PostToGraph(FROM_HERE, &PageNodeImpl::OnTitleUpdated, page_node_.get());
}

void PerformanceManagerTabHelper::DidUpdateFaviconURL(
    const std::vector<content::FaviconURL>& candidates) {
  // TODO(siggi): This logic belongs in the policy layer rather than here.
  if (!first_time_favicon_set_) {
    first_time_favicon_set_ = true;
    return;
  }
  PostToGraph(FROM_HERE, &PageNodeImpl::OnFaviconUpdated, page_node_.get());
}

void PerformanceManagerTabHelper::OnInterfaceRequestFromFrame(
    content::RenderFrameHost* render_frame_host,
    const std::string& interface_name,
    mojo::ScopedMessagePipeHandle* interface_pipe) {
  if (interface_name !=
      resource_coordinator::mojom::FrameCoordinationUnit::Name_)
    return;

  auto it = frames_.find(render_frame_host);
  DCHECK(it != frames_.end());
  PostToGraph(FROM_HERE, &FrameNodeImpl::AddBinding, it->second.get(),
              resource_coordinator::mojom::FrameCoordinationUnitRequest(
                  std::move(*interface_pipe)));
}

content::WebContents* PerformanceManagerTabHelper::GetWebContents() const {
  return web_contents();
}

template <typename Functor, typename NodeType, typename... Args>
void PerformanceManagerTabHelper::PostToGraph(const base::Location& from_here,
                                              Functor&& functor,
                                              NodeType* node,
                                              Args&&... args) {
  static_assert(std::is_base_of<NodeBase, NodeType>::value,
                "NodeType must be descended from NodeBase");
  performance_manager_->task_runner()->PostTask(
      from_here, base::BindOnce(functor, base::Unretained(node),
                                std::forward<Args>(args)...));
}

void PerformanceManagerTabHelper::OnMainFrameNavigation(int64_t navigation_id) {
  ukm_source_id_ =
      ukm::ConvertToSourceId(navigation_id, ukm::SourceIdType::NAVIGATION_ID);
  PostToGraph(FROM_HERE, &PageNodeImpl::SetUkmSourceId, page_node_.get(),
              ukm_source_id_);

  first_time_title_set_ = false;
  first_time_favicon_set_ = false;
}

void PerformanceManagerTabHelper::UpdatePageNodeVisibility(
    content::Visibility visibility) {
  // TODO(fdoray): An OCCLUDED tab should not be considered visible.
  const bool is_visible = visibility != content::Visibility::HIDDEN;
  PostToGraph(FROM_HERE, &PageNodeImpl::SetIsVisible, page_node_.get(),
              is_visible);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(PerformanceManagerTabHelper)

}  // namespace performance_manager
