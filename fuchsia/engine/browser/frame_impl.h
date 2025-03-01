// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FUCHSIA_ENGINE_BROWSER_FRAME_IMPL_H_
#define FUCHSIA_ENGINE_BROWSER_FRAME_IMPL_H_

#include <fuchsia/web/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/zx/channel.h>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/macros.h"
#include "base/memory/platform_shared_memory_region.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "fuchsia/engine/browser/discarding_event_filter.h"
#include "fuchsia/engine/on_load_script_injector.mojom.h"
#include "ui/aura/window_tree_host.h"
#include "ui/wm/core/focus_controller.h"
#include "url/gurl.h"

namespace aura {
class WindowTreeHost;
}  // namespace aura

namespace content {
class WebContents;
}  // namespace content

class ContextImpl;

// Implementation of fuchsia.web.Frame based on content::WebContents.
class FrameImpl : public fuchsia::web::Frame,
                  public fuchsia::web::NavigationController,
                  public content::WebContentsObserver,
                  public content::WebContentsDelegate {
 public:
  FrameImpl(std::unique_ptr<content::WebContents> web_contents,
            ContextImpl* context,
            fidl::InterfaceRequest<fuchsia::web::Frame> frame_request);
  ~FrameImpl() override;

  zx::unowned_channel GetBindingChannelForTest() const;
  content::WebContents* web_contents_for_test() { return web_contents_.get(); }
  bool has_view_for_test() { return window_tree_host_ != nullptr; }
  void set_javascript_console_message_hook_for_test(
      base::RepeatingCallback<void(base::StringPiece)> hook) {
    console_log_message_hook_ = std::move(hook);
  }

 private:
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, DelayedNavigationEventAck);
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, NavigationObserverDisconnected);
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, NoNavigationObserverAttached);
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, ReloadFrame);
  FRIEND_TEST_ALL_PREFIXES(FrameImplTest, Stop);

  // fuchsia::web::Frame implementation.
  void CreateView(fuchsia::ui::views::ViewToken view_token) override;
  void GetNavigationController(
      fidl::InterfaceRequest<fuchsia::web::NavigationController> controller)
      override;
  void ExecuteJavaScriptNoResult(
      std::vector<std::string> origins,
      fuchsia::mem::Buffer script,
      ExecuteJavaScriptNoResultCallback callback) override;
  void AddBeforeLoadJavaScript(
      uint64_t id,
      std::vector<std::string> origins,
      fuchsia::mem::Buffer script,
      AddBeforeLoadJavaScriptCallback callback) override;
  void RemoveBeforeLoadJavaScript(uint64_t id) override;
  void PostMessage(std::string origin,
                   fuchsia::web::WebMessage message,
                   fuchsia::web::Frame::PostMessageCallback callback) override;
  void SetNavigationEventListener(
      fidl::InterfaceHandle<fuchsia::web::NavigationEventListener> listener)
      override;
  void SetJavaScriptLogLevel(fuchsia::web::ConsoleLogLevel level) override;
  void SetEnableInput(bool enable_input) override;

  class OriginScopedScript {
   public:
    OriginScopedScript();
    OriginScopedScript(std::vector<std::string> origins,
                       base::ReadOnlySharedMemoryRegion script);
    OriginScopedScript& operator=(OriginScopedScript&& other);
    ~OriginScopedScript();

    const std::vector<std::string>& origins() const { return origins_; }
    const base::ReadOnlySharedMemoryRegion& script() const { return script_; }

   private:
    std::vector<std::string> origins_;

    // A shared memory buffer containing the script, encoded as UTF16.
    base::ReadOnlySharedMemoryRegion script_;

    DISALLOW_COPY_AND_ASSIGN(OriginScopedScript);
  };

  aura::Window* root_window() const { return window_tree_host_->window(); }

  // Release the resources associated with the View, if one is active.
  void TearDownView();

  // Processes the most recent changes to the browser's navigation state and
  // triggers the publishing of change events.
  void OnNavigationEntryChanged(content::NavigationEntry* entry);

  // Sends |pending_navigation_event_| to the observer if there are any changes
  // to be reported.
  void MaybeSendNavigationEvent();

  // fuchsia::web::NavigationController implementation.
  void LoadUrl(std::string url,
               fuchsia::web::LoadUrlParams params,
               LoadUrlCallback callback) override;
  void GoBack() override;
  void GoForward() override;
  void Stop() override;
  void Reload(fuchsia::web::ReloadType type) override;
  void GetVisibleEntry(GetVisibleEntryCallback callback) override;

  // content::WebContentsDelegate implementation.
  bool ShouldCreateWebContents(
      content::WebContents* web_contents,
      content::RenderFrameHost* opener,
      content::SiteInstance* source_site_instance,
      int32_t route_id,
      int32_t main_frame_route_id,
      int32_t main_frame_widget_route_id,
      content::mojom::WindowContainerType window_container_type,
      const GURL& opener_url,
      const std::string& frame_name,
      const GURL& target_url,
      const std::string& partition_id,
      content::SessionStorageNamespace* session_storage_namespace) override;
  bool DidAddMessageToConsole(content::WebContents* source,
                              int32_t level,
                              const base::string16& message,
                              int32_t line_no,
                              const base::string16& source_id) override;

  // content::WebContentsObserver implementation.
  void DidFinishLoad(content::RenderFrameHost* render_frame_host,
                     const GURL& validated_url) override;
  void ReadyToCommitNavigation(
      content::NavigationHandle* navigation_handle) override;
  void TitleWasSet(content::NavigationEntry* entry) override;

  std::unique_ptr<aura::WindowTreeHost> window_tree_host_;
  std::unique_ptr<content::WebContents> web_contents_;
  std::unique_ptr<wm::FocusController> focus_controller_;

  DiscardingEventFilter discarding_event_filter_;
  fuchsia::web::NavigationEventListenerPtr navigation_listener_;
  fuchsia::web::NavigationState cached_navigation_state_;
  fuchsia::web::NavigationState pending_navigation_event_;
  bool waiting_for_navigation_event_ack_;
  bool pending_navigation_event_is_dirty_;
  logging::LogSeverity log_level_;
  std::map<uint64_t, OriginScopedScript> before_load_scripts_;
  std::vector<uint64_t> before_load_scripts_order_;
  ContextImpl* context_ = nullptr;
  base::RepeatingCallback<void(base::StringPiece)> console_log_message_hook_;

  fidl::Binding<fuchsia::web::Frame> binding_;
  fidl::BindingSet<fuchsia::web::NavigationController> controller_bindings_;

  base::WeakPtrFactory<FrameImpl> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(FrameImpl);
};

#endif  // FUCHSIA_ENGINE_BROWSER_FRAME_IMPL_H_
