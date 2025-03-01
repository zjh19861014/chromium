// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_provider_host.h"

#include <utility>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/guid.h"
#include "base/memory/ptr_util.h"
#include "base/stl_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/time/time.h"
#include "content/browser/bad_message.h"
#include "content/browser/interface_provider_filtering.h"
#include "content/browser/renderer_interface_binders.h"
#include "content/browser/service_worker/embedded_worker_status.h"
#include "content/browser/service_worker/service_worker_consts.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/service_worker/service_worker_controllee_request_handler.h"
#include "content/browser/service_worker/service_worker_registration_object_host.h"
#include "content/browser/service_worker/service_worker_type_converters.h"
#include "content/browser/service_worker/service_worker_version.h"
#include "content/browser/url_loader_factory_getter.h"
#include "content/browser/web_contents/web_contents_getter_registry.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/common/service_worker/service_worker_types.h"
#include "content/common/service_worker/service_worker_utils.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/child_process_host.h"
#include "content/public/common/content_client.h"
#include "content/public/common/origin_util.h"
#include "mojo/public/cpp/bindings/message.h"
#include "net/base/url_util.h"
#include "services/network/public/cpp/features.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "storage/browser/blob/blob_storage_context.h"
#include "third_party/blink/public/common/messaging/message_port_channel.h"
#include "third_party/blink/public/common/service_worker/service_worker_utils.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_client.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_object.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"

namespace content {

namespace {

// This function provides the next ServiceWorkerProviderHost ID.
int NextProviderId() {
  static int g_next_provider_id = 0;
  return g_next_provider_id++;
}

// A request handler derivative used to handle navigation requests when
// skip_service_worker flag is set. It tracks the document URL and sets the url
// to the provider host.
class ServiceWorkerURLTrackingRequestHandler
    : public ServiceWorkerRequestHandler {
 public:
  ServiceWorkerURLTrackingRequestHandler(
      base::WeakPtr<ServiceWorkerContextCore> context,
      base::WeakPtr<ServiceWorkerProviderHost> provider_host,
      base::WeakPtr<storage::BlobStorageContext> blob_storage_context,
      ResourceType resource_type)
      : ServiceWorkerRequestHandler(context,
                                    provider_host,
                                    blob_storage_context,
                                    resource_type) {}
  ~ServiceWorkerURLTrackingRequestHandler() override {}

  void MaybeCreateLoader(
      const network::ResourceRequest& tentative_resource_request,
      ResourceContext*,
      LoaderCallback callback,
      FallbackCallback fallback_callback) override {
    // |provider_host_| may have been deleted when the request is resumed.
    if (!provider_host_)
      return;
    const GURL stripped_url =
        net::SimplifyUrlForRequest(tentative_resource_request.url);
    provider_host_->UpdateUrls(stripped_url,
                               tentative_resource_request.site_for_cookies);
    // Fall back to network.
    std::move(callback).Run({});
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ServiceWorkerURLTrackingRequestHandler);
};

void GetInterfaceImpl(const std::string& interface_name,
                      mojo::ScopedMessagePipeHandle interface_pipe,
                      const url::Origin& origin,
                      int process_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  auto* process = RenderProcessHost::FromID(process_id);
  if (!process)
    return;

  BindWorkerInterface(interface_name, std::move(interface_pipe), process,
                      origin);
}

ServiceWorkerMetrics::EventType PurposeToEventType(
    blink::mojom::ControllerServiceWorkerPurpose purpose) {
  switch (purpose) {
    case blink::mojom::ControllerServiceWorkerPurpose::FETCH_SUB_RESOURCE:
      return ServiceWorkerMetrics::EventType::FETCH_SUB_RESOURCE;
  }
  NOTREACHED();
  return ServiceWorkerMetrics::EventType::UNKNOWN;
}

void RunCallbacks(
    std::vector<ServiceWorkerProviderHost::ExecutionReadyCallback> callbacks) {
  for (auto& callback : callbacks) {
    std::move(callback).Run();
  }
}

}  // anonymous namespace

// RAII helper class for keeping track of versions waiting for an update hint
// from the renderer.
//
// This class is move-only.
class ServiceWorkerProviderHost::PendingUpdateVersion {
 public:
  explicit PendingUpdateVersion(scoped_refptr<ServiceWorkerVersion> version)
      : version_(std::move(version)) {
    version_->IncrementPendingUpdateHintCount();
  }

  PendingUpdateVersion(PendingUpdateVersion&& other) {
    version_ = std::move(other.version_);
  }

  ~PendingUpdateVersion() {
    if (version_)
      version_->DecrementPendingUpdateHintCount();
  }

  PendingUpdateVersion& operator=(PendingUpdateVersion&& other) {
    version_ = std::move(other.version_);
    return *this;
  }

  // Needed for base::flat_set.
  bool operator<(const PendingUpdateVersion& other) const {
    return version_ < other.version_;
  }

 private:
  scoped_refptr<ServiceWorkerVersion> version_;
  DISALLOW_COPY_AND_ASSIGN(PendingUpdateVersion);
};

// static
base::WeakPtr<ServiceWorkerProviderHost>
ServiceWorkerProviderHost::PreCreateNavigationHost(
    base::WeakPtr<ServiceWorkerContextCore> context,
    bool are_ancestors_secure,
    WebContentsGetter web_contents_getter,
    blink::mojom::ServiceWorkerProviderInfoForWindowPtr* out_provider_info) {
  DCHECK(context);
  blink::mojom::ServiceWorkerContainerAssociatedPtrInfo client_ptr_info;
  (*out_provider_info)->client_request = mojo::MakeRequest(&client_ptr_info);
  auto host = base::WrapUnique(new ServiceWorkerProviderHost(
      blink::mojom::ServiceWorkerProviderType::kForWindow, are_ancestors_secure,
      mojo::MakeRequest(&((*out_provider_info)->host_ptr_info)),
      std::move(client_ptr_info), context));
  host->web_contents_getter_ = std::move(web_contents_getter);

  (*out_provider_info)->provider_id = host->provider_id();
  auto weak_ptr = host->AsWeakPtr();
  RegisterToContextCore(context, std::move(host));
  return weak_ptr;
}

// static
base::WeakPtr<ServiceWorkerProviderHost>
ServiceWorkerProviderHost::PreCreateForController(
    base::WeakPtr<ServiceWorkerContextCore> context,
    scoped_refptr<ServiceWorkerVersion> version,
    blink::mojom::ServiceWorkerProviderInfoForStartWorkerPtr*
        out_provider_info) {
  blink::mojom::ServiceWorkerContainerAssociatedPtrInfo client_ptr_info;
  (*out_provider_info)->client_request = mojo::MakeRequest(&client_ptr_info);
  auto host = base::WrapUnique(new ServiceWorkerProviderHost(
      blink::mojom::ServiceWorkerProviderType::kForServiceWorker,
      true /* is_parent_frame_secure */,
      mojo::MakeRequest(&((*out_provider_info)->host_ptr_info)),
      std::move(client_ptr_info), context));
  host->running_hosted_version_ = std::move(version);

  (*out_provider_info)->provider_id = host->provider_id();
  auto weak_ptr = host->AsWeakPtr();
  RegisterToContextCore(context, std::move(host));
  return weak_ptr;
}

// static
base::WeakPtr<ServiceWorkerProviderHost>
ServiceWorkerProviderHost::PreCreateForSharedWorker(
    base::WeakPtr<ServiceWorkerContextCore> context,
    int process_id,
    blink::mojom::ServiceWorkerProviderInfoForWorkerPtr* out_provider_info) {
  blink::mojom::ServiceWorkerContainerAssociatedPtrInfo client_ptr_info;
  (*out_provider_info)->client_request = mojo::MakeRequest(&client_ptr_info);
  auto host = base::WrapUnique(new ServiceWorkerProviderHost(
      blink::mojom::ServiceWorkerProviderType::kForSharedWorker,
      true /* is_parent_frame_secure */,
      mojo::MakeRequest(&((*out_provider_info)->host_ptr_info)),
      std::move(client_ptr_info), context));
  host->SetRenderProcessId(process_id);

  (*out_provider_info)->provider_id = host->provider_id();
  auto weak_ptr = host->AsWeakPtr();
  RegisterToContextCore(context, std::move(host));
  return weak_ptr;
}

// static
void ServiceWorkerProviderHost::RegisterToContextCore(
    base::WeakPtr<ServiceWorkerContextCore> context,
    std::unique_ptr<ServiceWorkerProviderHost> host) {
  DCHECK(host->binding_.is_bound());
  host->binding_.set_connection_error_handler(
      base::BindOnce(&ServiceWorkerContextCore::RemoveProviderHost, context,
                     host->provider_id()));
  context->AddProviderHost(std::move(host));
}

ServiceWorkerProviderHost::ServiceWorkerProviderHost(
    blink::mojom::ServiceWorkerProviderType type,
    bool is_parent_frame_secure,
    blink::mojom::ServiceWorkerContainerHostAssociatedRequest host_request,
    blink::mojom::ServiceWorkerContainerAssociatedPtrInfo client_ptr_info,
    base::WeakPtr<ServiceWorkerContextCore> context)
    : provider_id_(NextProviderId()),
      type_(type),
      client_uuid_(base::GenerateGUID()),
      create_time_(base::TimeTicks::Now()),
      render_process_id_(ChildProcessHost::kInvalidUniqueID),
      render_thread_id_(kDocumentMainThreadId),
      frame_id_(MSG_ROUTING_NONE),
      is_parent_frame_secure_(is_parent_frame_secure),
      context_(context),
      binding_(this),
      interface_provider_binding_(this) {
  DCHECK_NE(blink::mojom::ServiceWorkerProviderType::kUnknown, type_);

  if (type_ == blink::mojom::ServiceWorkerProviderType::kForServiceWorker) {
    // Actual |render_process_id_| will be set after choosing a process for the
    // controller, and |render_thread_id_| will be set when the service worker
    // context gets started.
    render_thread_id_ = kInvalidEmbeddedWorkerThreadId;
  }

  context_->RegisterProviderHostByClientID(client_uuid_, this);

  DCHECK(client_ptr_info.is_valid() && host_request.is_pending());
  container_.Bind(std::move(client_ptr_info));
  binding_.Bind(std::move(host_request));
}

ServiceWorkerProviderHost::~ServiceWorkerProviderHost() {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (context_)
    context_->UnregisterProviderHostByClientID(client_uuid_);
  if (controller_)
    controller_->RemoveControllee(client_uuid_);
  if (fetch_request_window_id_)
    WebContentsGetterRegistry::GetInstance()->Remove(fetch_request_window_id_);

  // Remove |this| as an observer of ServiceWorkerRegistrations.
  // TODO(falken): Use ScopedObserver instead of this explicit call.
  controller_.reset();
  controller_registration_.reset();
  RemoveAllMatchingRegistrations();

  // Explicitly destroy the ServiceWorkerObjectHosts and
  // ServiceWorkerRegistrationObjectHosts owned by |this|. Otherwise, this
  // destructor can trigger their Mojo connection error handlers, which would
  // call back into halfway destroyed |this|. This is because they are
  // associated with the ServiceWorker interface, which can be destroyed while
  // in this destructor (|running_hosted_version_|'s |event_dispatcher_|). See
  // https://crbug.com/854993.
  service_worker_object_hosts_.clear();
  registration_object_hosts_.clear();

  // Ensure callbacks awaiting execution ready are notified.
  RunExecutionReadyCallbacks();
}

bool ServiceWorkerProviderHost::IsContextSecureForServiceWorker() const {
  DCHECK(IsProviderForClient());

  if (!url_.is_valid())
    return false;
  if (!OriginCanAccessServiceWorkers(url_))
    return false;

  if (is_parent_frame_secure())
    return true;

  std::set<std::string> schemes;
  GetContentClient()->browser()->GetSchemesBypassingSecureContextCheckWhitelist(
      &schemes);
  return schemes.find(url_.scheme()) != schemes.end();
}

blink::mojom::ControllerServiceWorkerMode
ServiceWorkerProviderHost::GetControllerMode() const {
  if (!controller_)
    return blink::mojom::ControllerServiceWorkerMode::kNoController;
  switch (controller_->fetch_handler_existence()) {
    case ServiceWorkerVersion::FetchHandlerExistence::DOES_NOT_EXIST:
      return blink::mojom::ControllerServiceWorkerMode::kNoFetchEventHandler;
    case ServiceWorkerVersion::FetchHandlerExistence::EXISTS:
      return blink::mojom::ControllerServiceWorkerMode::kControlled;
    case ServiceWorkerVersion::FetchHandlerExistence::UNKNOWN:
      // UNKNOWN means the controller is still installing. It's not possible to
      // have a controller that hasn't finished installing.
      NOTREACHED();
  }
  NOTREACHED();
  return blink::mojom::ControllerServiceWorkerMode::kNoController;
}

void ServiceWorkerProviderHost::OnVersionAttributesChanged(
    ServiceWorkerRegistration* registration,
    blink::mojom::ChangedServiceWorkerObjectsMaskPtr changed_mask,
    const ServiceWorkerRegistrationInfo& /* info */) {
  if (!get_ready_callback_ || get_ready_callback_->is_null())
    return;
  if (changed_mask->active && registration->active_version()) {
    // Wait until the state change so we don't send the get for ready
    // registration complete message before set version attributes message.
    registration->active_version()->RegisterStatusChangeCallback(base::BindOnce(
        &ServiceWorkerProviderHost::ReturnRegistrationForReadyIfNeeded,
        AsWeakPtr()));
  }
}

void ServiceWorkerProviderHost::OnRegistrationFailed(
    ServiceWorkerRegistration* registration) {
  RemoveMatchingRegistration(registration);
}

void ServiceWorkerProviderHost::OnRegistrationFinishedUninstalling(
    ServiceWorkerRegistration* registration) {
  RemoveMatchingRegistration(registration);
}

void ServiceWorkerProviderHost::OnSkippedWaiting(
    ServiceWorkerRegistration* registration) {
  if (controller_registration_ != registration)
    return;

  DCHECK(controller());
  ServiceWorkerVersion* active = controller_registration_->active_version();
  DCHECK(active);
  DCHECK_NE(active, controller());
  DCHECK_EQ(active->status(), ServiceWorkerVersion::ACTIVATING);
  UpdateController(true /* notify_controllerchange */);
}

blink::mojom::ControllerServiceWorkerPtr
ServiceWorkerProviderHost::GetControllerServiceWorkerPtr() {
  DCHECK(controller_);
  if (controller_->fetch_handler_existence() ==
      ServiceWorkerVersion::FetchHandlerExistence::DOES_NOT_EXIST) {
    return nullptr;
  }
  blink::mojom::ControllerServiceWorkerPtr controller_ptr;
  controller_->controller()->Clone(mojo::MakeRequest(&controller_ptr));
  return controller_ptr;
}

void ServiceWorkerProviderHost::UpdateUrls(const GURL& url,
                                           const GURL& site_for_cookies) {
  DCHECK(IsProviderForClient());
  DCHECK(!url.has_ref());
  DCHECK(!controller());

  GURL previous_url = url_;
  url_ = url;
  site_for_cookies_ = site_for_cookies;
  if (previous_url != url) {
    // Revoke the token on URL change since any service worker holding the token
    // may no longer be the potential controller of this frame and shouldn't
    // have the power to display SSL dialogs for it.
    if (type_ == blink::mojom::ServiceWorkerProviderType::kForWindow) {
      auto* registry = WebContentsGetterRegistry::GetInstance();
      registry->Remove(fetch_request_window_id_);
      fetch_request_window_id_ = base::UnguessableToken::Create();
      registry->Add(fetch_request_window_id_, web_contents_getter());
    }
  }

  auto previous_origin = url::Origin::Create(previous_url);
  auto new_origin = url::Origin::Create(url_);
  // Update client id on cross origin redirects. This corresponds to the HTML
  // standard's "process a navigation fetch" algorithm's step for discarding
  // |reservedEnvironment|.
  // https://html.spec.whatwg.org/multipage/browsing-the-web.html#process-a-navigate-fetch
  // "If |reservedEnvironment| is not null and |currentURL|'s origin is not the
  // same as |reservedEnvironment|'s creation URL's origin, then:
  //    1. Run the environment discarding steps for |reservedEnvironment|.
  //    2. Set |reservedEnvironment| to null."
  if (previous_url.is_valid() &&
      !new_origin.IsSameOriginWith(previous_origin)) {
    // Remove old controller since we know the controller is definitely
    // changed. We need to remove |this| from |controller_|'s controllee before
    // updating UUID since ServiceWorkerVersion has a map from uuid to provider
    // hosts.
    SetControllerRegistration(nullptr, false /* notify_controllerchange */);
    // Set UUID to the new one.
    context_->UnregisterProviderHostByClientID(client_uuid_);
    client_uuid_ = base::GenerateGUID();
    context_->RegisterProviderHostByClientID(client_uuid_, this);
  }

  SyncMatchingRegistrations();
}

const GURL& ServiceWorkerProviderHost::url() const {
  if (IsProviderForClient())
    return url_;
  return running_hosted_version_->script_url();
}

const GURL& ServiceWorkerProviderHost::site_for_cookies() const {
  if (IsProviderForClient())
    return site_for_cookies_;
  return running_hosted_version_->script_url();
}

void ServiceWorkerProviderHost::UpdateController(bool notify_controllerchange) {
  ServiceWorkerVersion* version =
      controller_registration_ ? controller_registration_->active_version()
                               : nullptr;
  CHECK(!version || IsContextSecureForServiceWorker());
  if (version == controller_.get())
    return;

  scoped_refptr<ServiceWorkerVersion> previous_version = controller_;
  controller_ = version;

  if (version)
    version->AddControllee(this);
  if (previous_version)
    previous_version->RemoveControllee(client_uuid_);

  // SetController message should be sent only for clients.
  DCHECK(IsProviderForClient());

  if (!IsControllerDecided())
    return;

  SendSetControllerServiceWorker(notify_controllerchange);
}

bool ServiceWorkerProviderHost::IsProviderForServiceWorker() const {
  return type_ == blink::mojom::ServiceWorkerProviderType::kForServiceWorker;
}

bool ServiceWorkerProviderHost::IsProviderForClient() const {
  switch (type_) {
    case blink::mojom::ServiceWorkerProviderType::kForWindow:
    case blink::mojom::ServiceWorkerProviderType::kForSharedWorker:
      return true;
    case blink::mojom::ServiceWorkerProviderType::kForServiceWorker:
      return false;
    case blink::mojom::ServiceWorkerProviderType::kUnknown:
      break;
  }
  NOTREACHED() << type_;
  return false;
}

blink::mojom::ServiceWorkerClientType ServiceWorkerProviderHost::client_type()
    const {
  switch (type_) {
    case blink::mojom::ServiceWorkerProviderType::kForWindow:
      return blink::mojom::ServiceWorkerClientType::kWindow;
    case blink::mojom::ServiceWorkerProviderType::kForSharedWorker:
      return blink::mojom::ServiceWorkerClientType::kSharedWorker;
    case blink::mojom::ServiceWorkerProviderType::kForServiceWorker:
    case blink::mojom::ServiceWorkerProviderType::kUnknown:
      break;
  }
  NOTREACHED() << type_;
  return blink::mojom::ServiceWorkerClientType::kWindow;
}

void ServiceWorkerProviderHost::SetControllerRegistration(
    scoped_refptr<ServiceWorkerRegistration> controller_registration,
    bool notify_controllerchange) {
  DCHECK(IsProviderForClient());

  if (controller_registration) {
    CHECK(IsContextSecureForServiceWorker());
    DCHECK(controller_registration->active_version());
#if DCHECK_IS_ON()
    DCHECK(IsMatchingRegistration(controller_registration.get()));
#endif  // DCHECK_IS_ON()
  }

  controller_registration_ = controller_registration;
  UpdateController(notify_controllerchange);
}

void ServiceWorkerProviderHost::AddMatchingRegistration(
    ServiceWorkerRegistration* registration) {
  DCHECK(ServiceWorkerUtils::ScopeMatches(registration->scope(), url_));
  if (!IsContextSecureForServiceWorker())
    return;
  size_t key = registration->scope().spec().size();
  if (base::ContainsKey(matching_registrations_, key))
    return;
  registration->AddListener(this);
  matching_registrations_[key] = registration;
  ReturnRegistrationForReadyIfNeeded();
}

void ServiceWorkerProviderHost::RemoveMatchingRegistration(
    ServiceWorkerRegistration* registration) {
  DCHECK_NE(controller_registration_, registration);
#if DCHECK_IS_ON()
  DCHECK(IsMatchingRegistration(registration));
#endif  // DCHECK_IS_ON()

  registration->RemoveListener(this);
  size_t key = registration->scope().spec().size();
  matching_registrations_.erase(key);
}

ServiceWorkerRegistration*
ServiceWorkerProviderHost::MatchRegistration() const {
  auto it = matching_registrations_.rbegin();
  for (; it != matching_registrations_.rend(); ++it) {
    if (it->second->is_uninstalled())
      continue;
    if (it->second->is_uninstalling())
      return nullptr;
    return it->second.get();
  }
  return nullptr;
}

void ServiceWorkerProviderHost::RemoveServiceWorkerRegistrationObjectHost(
    int64_t registration_id) {
  DCHECK(base::ContainsKey(registration_object_hosts_, registration_id));
  registration_object_hosts_.erase(registration_id);
}

void ServiceWorkerProviderHost::RemoveServiceWorkerObjectHost(
    int64_t version_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK(base::ContainsKey(service_worker_object_hosts_, version_id));
  service_worker_object_hosts_.erase(version_id);
}

bool ServiceWorkerProviderHost::AllowServiceWorker(const GURL& scope) {
  DCHECK(IsContextAlive());
  return GetContentClient()->browser()->AllowServiceWorker(
      scope, site_for_cookies(), context_->wrapper()->resource_context(),
      base::BindRepeating(&WebContentsImpl::FromRenderFrameHostID,
                          render_process_id_, frame_id()));
}

void ServiceWorkerProviderHost::NotifyControllerLost() {
  SetControllerRegistration(nullptr, true /* notify_controllerchange */);
}

void ServiceWorkerProviderHost::AddServiceWorkerToUpdate(
    scoped_refptr<ServiceWorkerVersion> version) {
  // This is only called for windows now, but it should be called for all
  // clients someday.
  DCHECK_EQ(provider_type(),
            blink::mojom::ServiceWorkerProviderType::kForWindow);

  versions_to_update_.emplace(std::move(version));
}

std::unique_ptr<ServiceWorkerRequestHandler>
ServiceWorkerProviderHost::CreateRequestHandler(
    network::mojom::FetchRequestMode request_mode,
    network::mojom::FetchCredentialsMode credentials_mode,
    network::mojom::FetchRedirectMode redirect_mode,
    const std::string& integrity,
    bool keepalive,
    ResourceType resource_type,
    blink::mojom::RequestContextType request_context_type,
    network::mojom::RequestContextFrameType frame_type,
    base::WeakPtr<storage::BlobStorageContext> blob_storage_context,
    scoped_refptr<network::ResourceRequestBody> body,
    bool skip_service_worker) {
  // We only get here for main resource requests for service worker clients
  // (navigations, dedicated workers, and shared workers).
  DCHECK(!IsProviderForServiceWorker());
  DCHECK(ServiceWorkerUtils::IsMainResourceType(resource_type));

  if (skip_service_worker) {
    // Use a request handler that just observes redirects so the resulting
    // provider host can have the correct URL.
    // TODO(falken): Just pass |skip_service_worker| to
    // ServiceWorkerControlleeRequestHandler.
    return std::make_unique<ServiceWorkerURLTrackingRequestHandler>(
        context_, AsWeakPtr(), blob_storage_context, resource_type);
  }

  return std::make_unique<ServiceWorkerControlleeRequestHandler>(
      context_, AsWeakPtr(), blob_storage_context, request_mode,
      credentials_mode, redirect_mode, integrity, keepalive, resource_type,
      request_context_type, frame_type, body);
}

base::WeakPtr<ServiceWorkerObjectHost>
ServiceWorkerProviderHost::GetOrCreateServiceWorkerObjectHost(
    scoped_refptr<ServiceWorkerVersion> version) {
  if (!context_ || !version)
    return nullptr;

  const int64_t version_id = version->version_id();
  auto existing_object_host = service_worker_object_hosts_.find(version_id);
  if (existing_object_host != service_worker_object_hosts_.end())
    return existing_object_host->second->AsWeakPtr();

  service_worker_object_hosts_[version_id] =
      std::make_unique<ServiceWorkerObjectHost>(context_, this,
                                                std::move(version));
  return service_worker_object_hosts_[version_id]->AsWeakPtr();
}

void ServiceWorkerProviderHost::PostMessageToClient(
    ServiceWorkerVersion* version,
    blink::TransferableMessage message) {
  DCHECK(IsProviderForClient());

  blink::mojom::ServiceWorkerObjectInfoPtr info;
  base::WeakPtr<ServiceWorkerObjectHost> object_host =
      GetOrCreateServiceWorkerObjectHost(version);
  if (object_host)
    info = object_host->CreateCompleteObjectInfoToSend();
  container_->PostMessageToClient(std::move(info), std::move(message));
}

void ServiceWorkerProviderHost::CountFeature(blink::mojom::WebFeature feature) {
  // CountFeature is a message about the client's controller. It should be sent
  // only for clients.
  DCHECK(IsProviderForClient());

  // And only when loading finished so the controller is really settled.
  if (!IsControllerDecided())
    return;

  container_->CountFeature(feature);
}

void ServiceWorkerProviderHost::ClaimedByRegistration(
    scoped_refptr<ServiceWorkerRegistration> registration) {
  DCHECK(registration->active_version());
  // TODO(falken): This should just early return, or DCHECK. claim() should have
  // no effect on a page that's already using the registration.
  if (registration == controller_registration_) {
    UpdateController(true /* notify_controllerchange */);
    return;
  }

  // TODO(crbug.com/866353): It shouldn't be necesary to check
  // |allow_set_controller_registration_|. See the comment for
  // AllowSetControllerRegistration().
  if (allow_set_controller_registration_)
    SetControllerRegistration(registration, true /* notify_controllerchange */);
}

void ServiceWorkerProviderHost::OnBeginNavigationCommit(int render_process_id,
                                                        int render_frame_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK_EQ(blink::mojom::ServiceWorkerProviderType::kForWindow, type_);

  DCHECK_EQ(ChildProcessHost::kInvalidUniqueID, render_process_id_);
  DCHECK_NE(ChildProcessHost::kInvalidUniqueID, render_process_id);
  SetRenderProcessId(render_process_id);

  DCHECK_EQ(MSG_ROUTING_NONE, frame_id_);
  DCHECK_NE(MSG_ROUTING_NONE, render_frame_id);
  frame_id_ = render_frame_id;

  TransitionToClientPhase(ClientPhase::kResponseCommitted);
}

void ServiceWorkerProviderHost::CompleteStartWorkerPreparation(
    int process_id,
    service_manager::mojom::InterfaceProviderRequest
        interface_provider_request) {
  DCHECK(context_);
  DCHECK_EQ(kInvalidEmbeddedWorkerThreadId, render_thread_id_);
  DCHECK_EQ(ChildProcessHost::kInvalidUniqueID, render_process_id_);
  DCHECK_NE(ChildProcessHost::kInvalidUniqueID, process_id);
  DCHECK_EQ(blink::mojom::ServiceWorkerProviderType::kForServiceWorker,
            provider_type());
  SetRenderProcessId(process_id);

  interface_provider_binding_.Bind(FilterRendererExposedInterfaces(
      blink::mojom::kNavigation_ServiceWorkerSpec, process_id,
      std::move(interface_provider_request)));
}

void ServiceWorkerProviderHost::CompleteSharedWorkerPreparation() {
  DCHECK_EQ(blink::mojom::ServiceWorkerProviderType::kForSharedWorker,
            provider_type());
  TransitionToClientPhase(ClientPhase::kResponseCommitted);
  SetExecutionReady();
}

void ServiceWorkerProviderHost::SyncMatchingRegistrations() {
  DCHECK(context_);
  DCHECK(!controller_registration());

  RemoveAllMatchingRegistrations();
  const auto& registrations = context_->GetLiveRegistrations();
  for (const auto& key_registration : registrations) {
    ServiceWorkerRegistration* registration = key_registration.second;
    if (!registration->is_uninstalled() &&
        ServiceWorkerUtils::ScopeMatches(registration->scope(), url_)) {
      AddMatchingRegistration(registration);
    }
  }
}

#if DCHECK_IS_ON()
bool ServiceWorkerProviderHost::IsMatchingRegistration(
    ServiceWorkerRegistration* registration) const {
  std::string spec = registration->scope().spec();
  size_t key = spec.size();

  auto iter = matching_registrations_.find(key);
  if (iter == matching_registrations_.end())
    return false;
  if (iter->second.get() != registration)
    return false;
  return true;
}
#endif  // DCHECK_IS_ON()

void ServiceWorkerProviderHost::RemoveAllMatchingRegistrations() {
  DCHECK(!controller_registration());
  for (const auto& it : matching_registrations_) {
    ServiceWorkerRegistration* registration = it.second.get();
    registration->RemoveListener(this);
  }
  matching_registrations_.clear();
}

void ServiceWorkerProviderHost::ReturnRegistrationForReadyIfNeeded() {
  if (!get_ready_callback_ || get_ready_callback_->is_null())
    return;
  ServiceWorkerRegistration* registration = MatchRegistration();
  if (!registration || !registration->active_version())
    return;
  TRACE_EVENT_ASYNC_END1("ServiceWorker",
                         "ServiceWorkerProviderHost::GetRegistrationForReady",
                         this, "Registration ID", registration->id());
  if (!IsContextAlive()) {
    // Here no need to run or destroy |get_ready_callback_|, which will destroy
    // together with |binding_| when |this| destroys.
    return;
  }

  std::move(*get_ready_callback_)
      .Run(CreateServiceWorkerRegistrationObjectInfo(
          scoped_refptr<ServiceWorkerRegistration>(registration)));
}

bool ServiceWorkerProviderHost::IsContextAlive() {
  return context_ != nullptr;
}

void ServiceWorkerProviderHost::SendSetControllerServiceWorker(
    bool notify_controllerchange) {
  DCHECK(IsProviderForClient());

  auto controller_info = blink::mojom::ControllerServiceWorkerInfo::New();
  controller_info->client_id = client_uuid();
  if (fetch_request_window_id_) {
    controller_info->fetch_request_window_id =
        base::make_optional(fetch_request_window_id_);
  }

  if (!controller_) {
    container_->SetController(std::move(controller_info),
                              notify_controllerchange);
    return;
  }

  DCHECK(controller_registration());
  DCHECK_EQ(controller_registration_->active_version(), controller_.get());

  controller_info->mode = GetControllerMode();

  // Pass an endpoint for the client to talk to this controller.
  controller_info->endpoint = GetControllerServiceWorkerPtr().PassInterface();

  // Set the info for the JavaScript ServiceWorkerContainer#controller object.
  base::WeakPtr<ServiceWorkerObjectHost> object_host =
      GetOrCreateServiceWorkerObjectHost(controller_);
  if (object_host)
    controller_info->object_info =
        object_host->CreateCompleteObjectInfoToSend();

  // Populate used features for UseCounter purposes.
  for (const blink::mojom::WebFeature feature : controller_->used_features())
    controller_info->used_features.push_back(feature);

  container_->SetController(std::move(controller_info),
                            notify_controllerchange);
}

bool ServiceWorkerProviderHost::IsControllerDecided() const {
  DCHECK(IsProviderForClient());

  if (is_execution_ready())
    return true;

  // TODO(falken): This function just becomes |is_execution_ready()|
  // when NetworkService is enabled, so remove/simplify it when
  // non-NetworkService code is removed.

  switch (client_type()) {
    case blink::mojom::ServiceWorkerClientType::kWindow:
      // |this| is hosting a reserved client undergoing navigation. Don't send
      // the controller since it can be changed again before the final
      // response. The controller will be sent on navigation commit. See
      // CommitNavigation in frame.mojom.
      return false;
    case blink::mojom::ServiceWorkerClientType::kSharedWorker:
      // NetworkService (PlzWorker):
      if (base::FeatureList::IsEnabled(network::features::kNetworkService)) {
        // When PlzWorker is enabled, the controller will be sent when the
        // response is committed to the renderer at SharedWorkerHost::Start().
        return false;
      }
      return true;
    case blink::mojom::ServiceWorkerClientType::kAll:
      NOTREACHED();
  }

  NOTREACHED();
  return true;
}

#if DCHECK_IS_ON()
void ServiceWorkerProviderHost::CheckControllerConsistency() const {
  if (!controller_) {
    DCHECK(!controller_registration_);
    return;
  }
  DCHECK(IsProviderForClient());
  DCHECK(controller_registration_);
  DCHECK_EQ(controller_->registration_id(), controller_registration_->id());
}
#endif

void ServiceWorkerProviderHost::Register(
    const GURL& script_url,
    blink::mojom::ServiceWorkerRegistrationOptionsPtr options,
    RegisterCallback callback) {
  if (!CanServeContainerHostMethods(&callback, options->scope,
                                    kServiceWorkerRegisterErrorPrefix,
                                    nullptr)) {
    return;
  }
  if (client_type() != blink::mojom::ServiceWorkerClientType::kWindow) {
    mojo::ReportBadMessage(ServiceWorkerConsts::kBadMessageFromNonWindow);
    std::move(callback).Run(blink::mojom::ServiceWorkerErrorType::kUnknown,
                            std::string(), nullptr);
    return;
  }
  std::vector<GURL> urls = {url(), options->scope, script_url};
  if (!ServiceWorkerUtils::AllOriginsMatchAndCanAccessServiceWorkers(urls)) {
    mojo::ReportBadMessage(ServiceWorkerConsts::kBadMessageImproperOrigins);
    // ReportBadMessage() will kill the renderer process, but Mojo complains if
    // the callback is not run. Just run it with nonsense arguments.
    std::move(callback).Run(blink::mojom::ServiceWorkerErrorType::kUnknown,
                            std::string(), nullptr);
    return;
  }
  int64_t trace_id = base::TimeTicks::Now().since_origin().InMicroseconds();
  TRACE_EVENT_ASYNC_BEGIN2(
      "ServiceWorker", "ServiceWorkerProviderHost::Register", trace_id, "Scope",
      options->scope.spec(), "Script URL", script_url.spec());
  context_->RegisterServiceWorker(
      script_url, *options,
      base::BindOnce(&ServiceWorkerProviderHost::RegistrationComplete,
                     AsWeakPtr(), std::move(callback), trace_id,
                     mojo::GetBadMessageCallback()));
}

void ServiceWorkerProviderHost::RegistrationComplete(
    RegisterCallback callback,
    int64_t trace_id,
    mojo::ReportBadMessageCallback bad_message_callback,
    blink::ServiceWorkerStatusCode status,
    const std::string& status_message,
    int64_t registration_id) {
  TRACE_EVENT_ASYNC_END2("ServiceWorker", "ServiceWorkerProviderHost::Register",
                         trace_id, "Status",
                         blink::ServiceWorkerStatusToString(status),
                         "Registration ID", registration_id);
  // kErrorInvalidArguments means the renderer gave unexpectedly bad arguments,
  // so terminate it.
  if (status == blink::ServiceWorkerStatusCode::kErrorInvalidArguments) {
    std::move(bad_message_callback).Run(status_message);
    // |bad_message_callback| will kill the renderer process, but Mojo complains
    // if the callback is not run. Just run it with nonsense arguments.
    std::move(callback).Run(blink::mojom::ServiceWorkerErrorType::kUnknown,
                            std::string(), nullptr);
    return;
  }
  if (!IsContextAlive()) {
    std::move(callback).Run(
        blink::mojom::ServiceWorkerErrorType::kAbort,
        std::string(kServiceWorkerRegisterErrorPrefix) +
            std::string(ServiceWorkerConsts::kShutdownErrorMessage),
        nullptr);
    return;
  }

  if (status != blink::ServiceWorkerStatusCode::kOk) {
    std::string error_message;
    blink::mojom::ServiceWorkerErrorType error_type;
    GetServiceWorkerErrorTypeForRegistration(status, status_message,
                                             &error_type, &error_message);
    std::move(callback).Run(
        error_type, kServiceWorkerRegisterErrorPrefix + error_message, nullptr);
    return;
  }

  ServiceWorkerRegistration* registration =
      context_->GetLiveRegistration(registration_id);
  // ServiceWorkerRegisterJob calls its completion callback, which results in
  // this function being called, while the registration is live.
  DCHECK(registration);

  std::move(callback).Run(
      blink::mojom::ServiceWorkerErrorType::kNone, base::nullopt,
      CreateServiceWorkerRegistrationObjectInfo(
          scoped_refptr<ServiceWorkerRegistration>(registration)));
}

void ServiceWorkerProviderHost::GetRegistration(
    const GURL& client_url,
    GetRegistrationCallback callback) {
  if (!CanServeContainerHostMethods(&callback, url(),
                                    kServiceWorkerGetRegistrationErrorPrefix,
                                    nullptr)) {
    return;
  }

  std::string error_message;
  if (!IsValidGetRegistrationMessage(client_url, &error_message)) {
    mojo::ReportBadMessage(error_message);
    // ReportBadMessage() will kill the renderer process, but Mojo complains if
    // the callback is not run. Just run it with nonsense arguments.
    std::move(callback).Run(blink::mojom::ServiceWorkerErrorType::kUnknown,
                            std::string(), nullptr);
    return;
  }

  int64_t trace_id = base::TimeTicks::Now().since_origin().InMicroseconds();
  TRACE_EVENT_ASYNC_BEGIN1("ServiceWorker",
                           "ServiceWorkerProviderHost::GetRegistration",
                           trace_id, "Client URL", client_url.spec());
  context_->storage()->FindRegistrationForDocument(
      client_url, base::AdaptCallbackForRepeating(base::BindOnce(
                      &ServiceWorkerProviderHost::GetRegistrationComplete,
                      AsWeakPtr(), std::move(callback), trace_id)));
}

void ServiceWorkerProviderHost::GetRegistrations(
    GetRegistrationsCallback callback) {
  if (!CanServeContainerHostMethods(&callback, url(),
                                    kServiceWorkerGetRegistrationsErrorPrefix,
                                    base::nullopt)) {
    return;
  }

  std::string error_message;
  if (!IsValidGetRegistrationsMessage(&error_message)) {
    mojo::ReportBadMessage(error_message);
    // ReportBadMessage() will kill the renderer process, but Mojo complains if
    // the callback is not run. Just run it with nonsense arguments.
    std::move(callback).Run(blink::mojom::ServiceWorkerErrorType::kUnknown,
                            std::string(), base::nullopt);
    return;
  }

  int64_t trace_id = base::TimeTicks::Now().since_origin().InMicroseconds();
  TRACE_EVENT_ASYNC_BEGIN0(
      "ServiceWorker", "ServiceWorkerProviderHost::GetRegistrations", trace_id);
  context_->storage()->GetRegistrationsForOrigin(
      url().GetOrigin(),
      base::AdaptCallbackForRepeating(
          base::BindOnce(&ServiceWorkerProviderHost::GetRegistrationsComplete,
                         AsWeakPtr(), std::move(callback), trace_id)));
}

void ServiceWorkerProviderHost::GetRegistrationComplete(
    GetRegistrationCallback callback,
    int64_t trace_id,
    blink::ServiceWorkerStatusCode status,
    scoped_refptr<ServiceWorkerRegistration> registration) {
  TRACE_EVENT_ASYNC_END2(
      "ServiceWorker", "ServiceWorkerProviderHost::GetRegistration", trace_id,
      "Status", blink::ServiceWorkerStatusToString(status), "Registration ID",
      registration ? registration->id()
                   : blink::mojom::kInvalidServiceWorkerRegistrationId);
  if (!IsContextAlive()) {
    std::move(callback).Run(
        blink::mojom::ServiceWorkerErrorType::kAbort,
        std::string(kServiceWorkerGetRegistrationErrorPrefix) +
            std::string(ServiceWorkerConsts::kShutdownErrorMessage),
        nullptr);
    return;
  }

  if (status != blink::ServiceWorkerStatusCode::kOk &&
      status != blink::ServiceWorkerStatusCode::kErrorNotFound) {
    std::string error_message;
    blink::mojom::ServiceWorkerErrorType error_type;
    GetServiceWorkerErrorTypeForRegistration(status, std::string(), &error_type,
                                             &error_message);
    std::move(callback).Run(
        error_type, kServiceWorkerGetRegistrationErrorPrefix + error_message,
        nullptr);
    return;
  }

  DCHECK(status != blink::ServiceWorkerStatusCode::kOk || registration);
  blink::mojom::ServiceWorkerRegistrationObjectInfoPtr info;
  if (status == blink::ServiceWorkerStatusCode::kOk &&
      !registration->is_uninstalling())
    info = CreateServiceWorkerRegistrationObjectInfo(std::move(registration));

  std::move(callback).Run(blink::mojom::ServiceWorkerErrorType::kNone,
                          base::nullopt, std::move(info));
}

void ServiceWorkerProviderHost::GetRegistrationsComplete(
    GetRegistrationsCallback callback,
    int64_t trace_id,
    blink::ServiceWorkerStatusCode status,
    const std::vector<scoped_refptr<ServiceWorkerRegistration>>&
        registrations) {
  TRACE_EVENT_ASYNC_END1(
      "ServiceWorker", "ServiceWorkerProviderHost::GetRegistrations", trace_id,
      "Status", blink::ServiceWorkerStatusToString(status));
  if (!IsContextAlive()) {
    std::move(callback).Run(
        blink::mojom::ServiceWorkerErrorType::kAbort,
        std::string(kServiceWorkerGetRegistrationsErrorPrefix) +
            std::string(ServiceWorkerConsts::kShutdownErrorMessage),
        base::nullopt);
    return;
  }

  if (status != blink::ServiceWorkerStatusCode::kOk) {
    std::string error_message;
    blink::mojom::ServiceWorkerErrorType error_type;
    GetServiceWorkerErrorTypeForRegistration(status, std::string(), &error_type,
                                             &error_message);
    std::move(callback).Run(
        error_type, kServiceWorkerGetRegistrationsErrorPrefix + error_message,
        base::nullopt);
    return;
  }

  std::vector<blink::mojom::ServiceWorkerRegistrationObjectInfoPtr>
      object_infos;

  for (const auto& registration : registrations) {
    DCHECK(registration.get());
    if (!registration->is_uninstalling()) {
      object_infos.push_back(
          CreateServiceWorkerRegistrationObjectInfo(std::move(registration)));
    }
  }

  std::move(callback).Run(blink::mojom::ServiceWorkerErrorType::kNone,
                          base::nullopt, std::move(object_infos));
}

void ServiceWorkerProviderHost::GetRegistrationForReady(
    GetRegistrationForReadyCallback callback) {
  std::string error_message;
  if (!IsValidGetRegistrationForReadyMessage(&error_message)) {
    mojo::ReportBadMessage(error_message);
    // ReportBadMessage() will kill the renderer process, but Mojo complains if
    // the callback is not run. Just run it with nonsense arguments.
    std::move(callback).Run(nullptr);
    return;
  }

  TRACE_EVENT_ASYNC_BEGIN0("ServiceWorker",
                           "ServiceWorkerProviderHost::GetRegistrationForReady",
                           this);
  DCHECK(!get_ready_callback_);
  get_ready_callback_ =
      std::make_unique<GetRegistrationForReadyCallback>(std::move(callback));
  ReturnRegistrationForReadyIfNeeded();
}

void ServiceWorkerProviderHost::StartControllerComplete(
    blink::mojom::ControllerServiceWorkerRequest controller_request,
    blink::ServiceWorkerStatusCode status) {
  if (status == blink::ServiceWorkerStatusCode::kOk)
    controller_->controller()->Clone(std::move(controller_request));
}

void ServiceWorkerProviderHost::EnsureControllerServiceWorker(
    blink::mojom::ControllerServiceWorkerRequest controller_request,
    blink::mojom::ControllerServiceWorkerPurpose purpose) {
  // TODO(kinuko): Log the reasons we drop the request.
  if (!IsContextAlive() || !controller_)
    return;

  controller_->RunAfterStartWorker(
      PurposeToEventType(purpose),
      base::BindOnce(&ServiceWorkerProviderHost::StartControllerComplete,
                     AsWeakPtr(), std::move(controller_request)));
}

void ServiceWorkerProviderHost::CloneContainerHost(
    blink::mojom::ServiceWorkerContainerHostRequest container_host_request) {
  additional_bindings_.AddBinding(this, std::move(container_host_request));
}

void ServiceWorkerProviderHost::Ping(PingCallback callback) {
  std::move(callback).Run();
}

void ServiceWorkerProviderHost::HintToUpdateServiceWorker() {
  if (!IsProviderForClient()) {
    mojo::ReportBadMessage("SWPH_HTUSW_NOT_CLIENT");
    return;
  }

  // The destructors notify the ServiceWorkerVersions to update.
  versions_to_update_.clear();
}

void ServiceWorkerProviderHost::OnExecutionReady() {
  if (!IsProviderForClient()) {
    mojo::ReportBadMessage("SWPH_OER_NOT_CLIENT");
    return;
  }

  if (is_execution_ready()) {
    mojo::ReportBadMessage("SWPH_OER_ALREADY_READY");
    return;
  }

  // The controller was sent on navigation commit but we must send it again here
  // because 1) the controller might have changed since navigation commit due to
  // skipWaiting(), and 2) the UseCounter might have changed since navigation
  // commit, in such cases the updated information was prevented being sent due
  // to false IsControllerDecided().
  // TODO(leonhsl): Create some layout tests covering the above case 1), in
  // which case we may also need to set |notify_controllerchange| correctly.
  SendSetControllerServiceWorker(false /* notify_controllerchange */);

  SetExecutionReady();
}

bool ServiceWorkerProviderHost::IsValidGetRegistrationMessage(
    const GURL& client_url,
    std::string* out_error) const {
  if (client_type() != blink::mojom::ServiceWorkerClientType::kWindow) {
    *out_error = ServiceWorkerConsts::kBadMessageFromNonWindow;
    return false;
  }
  if (!client_url.is_valid()) {
    *out_error = ServiceWorkerConsts::kBadMessageInvalidURL;
    return false;
  }
  std::vector<GURL> urls = {url(), client_url};
  if (!ServiceWorkerUtils::AllOriginsMatchAndCanAccessServiceWorkers(urls)) {
    *out_error = ServiceWorkerConsts::kBadMessageImproperOrigins;
    return false;
  }

  return true;
}

bool ServiceWorkerProviderHost::IsValidGetRegistrationsMessage(
    std::string* out_error) const {
  if (client_type() != blink::mojom::ServiceWorkerClientType::kWindow) {
    *out_error = ServiceWorkerConsts::kBadMessageFromNonWindow;
    return false;
  }
  if (!OriginCanAccessServiceWorkers(url())) {
    *out_error = ServiceWorkerConsts::kBadMessageImproperOrigins;
    return false;
  }

  return true;
}

bool ServiceWorkerProviderHost::IsValidGetRegistrationForReadyMessage(
    std::string* out_error) const {
  if (client_type() != blink::mojom::ServiceWorkerClientType::kWindow) {
    *out_error = ServiceWorkerConsts::kBadMessageFromNonWindow;
    return false;
  }

  if (get_ready_callback_) {
    *out_error =
        ServiceWorkerConsts::kBadMessageGetRegistrationForReadyDuplicated;
    return false;
  }

  return true;
}

void ServiceWorkerProviderHost::GetInterface(
    const std::string& interface_name,
    mojo::ScopedMessagePipeHandle interface_pipe) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  DCHECK_NE(kDocumentMainThreadId, render_thread_id_);
  DCHECK(IsProviderForServiceWorker());
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::UI},
      base::BindOnce(
          &GetInterfaceImpl, interface_name, std::move(interface_pipe),
          running_hosted_version_->script_origin(), render_process_id_));
}

blink::mojom::ServiceWorkerRegistrationObjectInfoPtr
ServiceWorkerProviderHost::CreateServiceWorkerRegistrationObjectInfo(
    scoped_refptr<ServiceWorkerRegistration> registration) {
  int64_t registration_id = registration->id();
  auto existing_host = registration_object_hosts_.find(registration_id);
  if (existing_host != registration_object_hosts_.end()) {
    return existing_host->second->CreateObjectInfo();
  }
  registration_object_hosts_[registration_id] =
      std::make_unique<ServiceWorkerRegistrationObjectHost>(
          context_, this, std::move(registration));
  return registration_object_hosts_[registration_id]->CreateObjectInfo();
}

template <typename CallbackType, typename... Args>
bool ServiceWorkerProviderHost::CanServeContainerHostMethods(
    CallbackType* callback,
    const GURL& scope,
    const char* error_prefix,
    Args... args) {
  if (!IsContextAlive()) {
    std::move(*callback).Run(
        blink::mojom::ServiceWorkerErrorType::kAbort,
        std::string(error_prefix) +
            std::string(ServiceWorkerConsts::kShutdownErrorMessage),
        args...);
    return false;
  }

  // TODO(falken): This check can be removed once crbug.com/439697 is fixed.
  // (Also see crbug.com/776408)
  if (url().is_empty()) {
    std::move(*callback).Run(
        blink::mojom::ServiceWorkerErrorType::kSecurity,
        std::string(error_prefix) +
            std::string(ServiceWorkerConsts::kNoDocumentURLErrorMessage),
        args...);
    return false;
  }

  if (!AllowServiceWorker(scope)) {
    std::move(*callback).Run(
        blink::mojom::ServiceWorkerErrorType::kDisabled,
        std::string(error_prefix) +
            std::string(ServiceWorkerConsts::kUserDeniedPermissionMessage),
        args...);
    return false;
  }

  return true;
}

void ServiceWorkerProviderHost::AddExecutionReadyCallback(
    ExecutionReadyCallback callback) {
  DCHECK(!is_execution_ready());
  execution_ready_callbacks_.push_back(std::move(callback));
}

bool ServiceWorkerProviderHost::is_response_committed() const {
  DCHECK(IsProviderForClient());
  switch (client_phase_) {
    case ClientPhase::kInitial:
      return false;
    case ClientPhase::kResponseCommitted:
    case ClientPhase::kExecutionReady:
      return true;
  }
  NOTREACHED();
  return false;
}

bool ServiceWorkerProviderHost::is_execution_ready() const {
  DCHECK(IsProviderForClient());
  return client_phase_ == ClientPhase::kExecutionReady;
}

void ServiceWorkerProviderHost::SetExecutionReady() {
  DCHECK(!is_execution_ready());
  TransitionToClientPhase(ClientPhase::kExecutionReady);
  RunExecutionReadyCallbacks();
}

void ServiceWorkerProviderHost::RunExecutionReadyCallbacks() {
  std::vector<ExecutionReadyCallback> callbacks;
  execution_ready_callbacks_.swap(callbacks);
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(&RunCallbacks, std::move(callbacks)));
}

void ServiceWorkerProviderHost::TransitionToClientPhase(ClientPhase new_phase) {
  if (client_phase_ == new_phase)
    return;
  switch (client_phase_) {
    case ClientPhase::kInitial:
      DCHECK_EQ(new_phase, ClientPhase::kResponseCommitted);
      break;
    case ClientPhase::kResponseCommitted:
      DCHECK_EQ(new_phase, ClientPhase::kExecutionReady);
      break;
    case ClientPhase::kExecutionReady:
      NOTREACHED();
      break;
  }
  client_phase_ = new_phase;
}

void ServiceWorkerProviderHost::SetRenderProcessId(int process_id) {
  render_process_id_ = process_id;
  if (controller_)
    controller_->UpdateForegroundPriority();
}

}  // namespace content
