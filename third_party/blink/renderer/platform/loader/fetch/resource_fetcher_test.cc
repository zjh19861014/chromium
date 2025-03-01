/*
 * Copyright (c) 2013, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h"

#include <memory>
#include "base/optional.h"
#include "build/build_config.h"
#include "services/network/public/mojom/request_context_frame_type.mojom-shared.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom-shared.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_url_loader.h"
#include "third_party/blink/public/platform/web_url_loader_mock_factory.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/renderer/platform/exported/wrapped_resource_response.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/heap/heap_allocator.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/loader/fetch/console_logger.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_initiator_info.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_initiator_type_names.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_parameters.h"
#include "third_party/blink/renderer/platform/loader/fetch/memory_cache.h"
#include "third_party/blink/renderer/platform/loader/fetch/null_resource_fetcher_properties.h"
#include "third_party/blink/renderer/platform/loader/fetch/raw_resource.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_error.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_load_observer.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_loader.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_loader_options.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_request.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_timing_info.h"
#include "third_party/blink/renderer/platform/loader/fetch/unique_identifier.h"
#include "third_party/blink/renderer/platform/loader/testing/fetch_testing_platform_support.h"
#include "third_party/blink/renderer/platform/loader/testing/mock_fetch_context.h"
#include "third_party/blink/renderer/platform/loader/testing/mock_resource.h"
#include "third_party/blink/renderer/platform/loader/testing/mock_resource_client.h"
#include "third_party/blink/renderer/platform/loader/testing/test_loader_factory.h"
#include "third_party/blink/renderer/platform/loader/testing/test_resource_fetcher_properties.h"
#include "third_party/blink/renderer/platform/testing/testing_platform_support.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/url_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/weburl_loader_mock.h"
#include "third_party/blink/renderer/platform/testing/weburl_loader_mock_factory_impl.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/allocator.h"
#include "third_party/blink/renderer/platform/wtf/time.h"

namespace blink {

namespace {

constexpr char kTestResourceFilename[] = "white-1x1.png";
constexpr char kTestResourceMimeType[] = "image/png";
constexpr uint32_t kTestResourceSize = 103;  // size of white-1x1.png

void RegisterMockedURLLoadWithCustomResponse(const KURL& url,
                                             const ResourceResponse& response) {
  url_test_helpers::RegisterMockedURLLoadWithCustomResponse(
      url, test::PlatformTestDataPath(kTestResourceFilename),
      WrappedResourceResponse(response));
}

void RegisterMockedURLLoad(const KURL& url) {
  url_test_helpers::RegisterMockedURLLoad(
      url, test::PlatformTestDataPath(kTestResourceFilename),
      kTestResourceMimeType);
}

}  // namespace

class ResourceFetcherTest : public testing::Test {
 public:
  ResourceFetcherTest() = default;
  ~ResourceFetcherTest() override { GetMemoryCache()->EvictResources(); }

  class TestResourceLoadObserver final : public ResourceLoadObserver {
   public:
    // ResourceLoadObserver implementation.
    void WillSendRequest(uint64_t identifier,
                         const ResourceRequest& request,
                         const ResourceResponse& redirect_response,
                         ResourceType,
                         const FetchInitiatorInfo&) override {
      request_ = request;
    }
    void DidReceiveResponse(uint64_t identifier,
                            const ResourceRequest& request,
                            const ResourceResponse& response,
                            const Resource* resource,
                            ResponseSource source) override {}
    void DidReceiveData(uint64_t identifier,
                        base::span<const char> chunk) override {}
    void DidReceiveTransferSizeUpdate(uint64_t identifier,
                                      int transfer_size_diff) override {}
    void DidDownloadToBlob(uint64_t identifier, BlobDataHandle*) override {}
    void DidFinishLoading(uint64_t identifier,
                          TimeTicks finish_time,
                          int64_t encoded_data_length,
                          int64_t decoded_body_length,
                          bool should_report_corb_blocking,
                          ResponseSource) override {}
    void DidFailLoading(const KURL&,
                        uint64_t identifier,
                        const ResourceError&,
                        int64_t encoded_data_length,
                        bool is_internal_request) override {}

    const base::Optional<ResourceRequest>& GetLastRequest() const {
      return request_;
    }

   private:
    base::Optional<ResourceRequest> request_;
  };

 protected:
  scoped_refptr<scheduler::FakeTaskRunner> CreateTaskRunner() {
    return base::MakeRefCounted<scheduler::FakeTaskRunner>();
  }
  ResourceFetcher* CreateFetcher(const ResourceFetcherProperties& properties,
                                 FetchContext* context) {
    return MakeGarbageCollected<ResourceFetcher>(
        ResourceFetcherInit(properties, context, CreateTaskRunner(),
                            MakeGarbageCollected<TestLoaderFactory>(),
                            *MakeGarbageCollected<NullConsoleLogger>()));
  }
  ResourceFetcher* CreateFetcher(const ResourceFetcherProperties& properties) {
    return CreateFetcher(properties, MakeGarbageCollected<MockFetchContext>());
  }
  ResourceFetcher* CreateFetcher() {
    return CreateFetcher(
        *MakeGarbageCollected<TestResourceFetcherProperties>());
  }
  void AddResourceToMemoryCache(Resource* resource) {
    GetMemoryCache()->Add(resource);
  }

  ScopedTestingPlatformSupport<FetchTestingPlatformSupport> platform_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ResourceFetcherTest);
};

TEST_F(ResourceFetcherTest, StartLoadAfterFrameDetach) {
  KURL secure_url("https://secureorigin.test/image.png");
  // Try to request a url. The request should fail, and a resource in an error
  // state should be returned, and no resource should be present in the cache.
  auto* fetcher =
      CreateFetcher(*MakeGarbageCollected<NullResourceFetcherProperties>(),
                    &FetchContext::NullInstance());
  ResourceRequest resource_request(secure_url);
  resource_request.SetRequestContext(mojom::RequestContextType::INTERNAL);
  FetchParameters fetch_params(resource_request);
  Resource* resource = RawResource::Fetch(fetch_params, fetcher, nullptr);
  ASSERT_TRUE(resource);
  EXPECT_TRUE(resource->ErrorOccurred());
  EXPECT_TRUE(resource->GetResourceError().IsAccessCheck());
  EXPECT_FALSE(GetMemoryCache()->ResourceForURL(secure_url));

  // Start by calling StartLoad() directly, rather than via RequestResource().
  // This shouldn't crash.
  fetcher->StartLoad(RawResource::CreateForTest(
      secure_url, SecurityOrigin::CreateUniqueOpaque(), ResourceType::kRaw));
}

TEST_F(ResourceFetcherTest, UseExistingResource) {
  auto* fetcher = CreateFetcher();

  KURL url("http://127.0.0.1:8000/foo.html");
  ResourceResponse response(url);
  response.SetHttpStatusCode(200);
  response.SetHttpHeaderField(http_names::kCacheControl, "max-age=3600");
  RegisterMockedURLLoadWithCustomResponse(url, response);

  FetchParameters fetch_params{ResourceRequest(url)};
  Resource* resource = MockResource::Fetch(fetch_params, fetcher, nullptr);
  ASSERT_TRUE(resource);
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();
  EXPECT_TRUE(resource->IsLoaded());
  EXPECT_TRUE(GetMemoryCache()->Contains(resource));

  Resource* new_resource = MockResource::Fetch(fetch_params, fetcher, nullptr);
  EXPECT_EQ(resource, new_resource);
}

// Verify that the ad bit is copied to WillSendRequest's request when the
// response is served from the memory cache.
TEST_F(ResourceFetcherTest, WillSendRequestAdBit) {
  // Add a resource to the memory cache.
  scoped_refptr<const SecurityOrigin> source_origin =
      SecurityOrigin::CreateUniqueOpaque();
  auto* properties =
      MakeGarbageCollected<TestResourceFetcherProperties>(source_origin);
  MockFetchContext* context = MakeGarbageCollected<MockFetchContext>();
  KURL url("http://127.0.0.1:8000/foo.html");
  Resource* resource =
      RawResource::CreateForTest(url, source_origin, ResourceType::kRaw);
  AddResourceToMemoryCache(resource);
  ResourceResponse response(url);
  response.SetHttpStatusCode(200);
  response.SetHttpHeaderField(http_names::kCacheControl, "max-age=3600");
  resource->ResponseReceived(response);
  resource->FinishForTest();

  auto* observer = MakeGarbageCollected<TestResourceLoadObserver>();
  // Fetch the cached resource. The request to DispatchWillSendRequest should
  // preserve the ad bit.
  auto* fetcher = CreateFetcher(*properties, context);
  fetcher->SetResourceLoadObserver(observer);
  ResourceRequest resource_request(url);
  resource_request.SetIsAdResource();
  resource_request.SetRequestContext(mojom::RequestContextType::INTERNAL);
  FetchParameters fetch_params(resource_request);
  platform_->GetURLLoaderMockFactory()->RegisterURL(url, WebURLResponse(), "");
  Resource* new_resource = RawResource::Fetch(fetch_params, fetcher, nullptr);

  EXPECT_EQ(resource, new_resource);
  base::Optional<ResourceRequest> new_request = observer->GetLastRequest();
  EXPECT_TRUE(new_request.has_value());
  EXPECT_TRUE(new_request.value().IsAdResource());
}

TEST_F(ResourceFetcherTest, Vary) {
  scoped_refptr<const SecurityOrigin> source_origin =
      SecurityOrigin::CreateUniqueOpaque();
  KURL url("http://127.0.0.1:8000/foo.html");
  Resource* resource =
      RawResource::CreateForTest(url, source_origin, ResourceType::kRaw);
  AddResourceToMemoryCache(resource);

  ResourceResponse response(url);
  response.SetHttpStatusCode(200);
  response.SetHttpHeaderField(http_names::kCacheControl, "max-age=3600");
  response.SetHttpHeaderField(http_names::kVary, "*");
  resource->ResponseReceived(response);
  resource->FinishForTest();
  ASSERT_TRUE(resource->MustReloadDueToVaryHeader(ResourceRequest(url)));

  auto* fetcher = CreateFetcher(
      *MakeGarbageCollected<TestResourceFetcherProperties>(source_origin));
  ResourceRequest resource_request(url);
  resource_request.SetRequestContext(mojom::RequestContextType::INTERNAL);
  FetchParameters fetch_params(resource_request);
  platform_->GetURLLoaderMockFactory()->RegisterURL(url, WebURLResponse(), "");
  Resource* new_resource = RawResource::Fetch(fetch_params, fetcher, nullptr);
  EXPECT_NE(resource, new_resource);
  new_resource->Loader()->Cancel();
}

TEST_F(ResourceFetcherTest, ResourceTimingInfo) {
  auto info = ResourceTimingInfo::Create(fetch_initiator_type_names::kDocument,
                                         CurrentTimeTicks());
  info->AddFinalTransferSize(5);
  EXPECT_EQ(info->TransferSize(), static_cast<uint64_t>(5));
  ResourceResponse redirect_response(KURL("https://example.com/original"));
  redirect_response.SetHttpStatusCode(200);
  redirect_response.SetEncodedDataLength(7);
  info->AddRedirect(redirect_response, KURL("https://example.com/redirect"));
  EXPECT_EQ(info->TransferSize(), static_cast<uint64_t>(12));
}

TEST_F(ResourceFetcherTest, VaryOnBack) {
  scoped_refptr<const SecurityOrigin> source_origin =
      SecurityOrigin::CreateUniqueOpaque();
  auto* fetcher = CreateFetcher(
      *MakeGarbageCollected<TestResourceFetcherProperties>(source_origin));

  KURL url("http://127.0.0.1:8000/foo.html");
  Resource* resource =
      RawResource::CreateForTest(url, source_origin, ResourceType::kRaw);
  AddResourceToMemoryCache(resource);

  ResourceResponse response(url);
  response.SetHttpStatusCode(200);
  response.SetHttpHeaderField(http_names::kCacheControl, "max-age=3600");
  response.SetHttpHeaderField(http_names::kVary, "*");
  resource->ResponseReceived(response);
  resource->FinishForTest();
  ASSERT_TRUE(resource->MustReloadDueToVaryHeader(ResourceRequest(url)));

  ResourceRequest resource_request(url);
  resource_request.SetCacheMode(mojom::FetchCacheMode::kForceCache);
  resource_request.SetRequestContext(mojom::RequestContextType::INTERNAL);
  FetchParameters fetch_params(resource_request);
  Resource* new_resource = RawResource::Fetch(fetch_params, fetcher, nullptr);
  EXPECT_EQ(resource, new_resource);
}

TEST_F(ResourceFetcherTest, VaryResource) {
  auto* fetcher = CreateFetcher();

  KURL url("http://127.0.0.1:8000/foo.html");
  ResourceResponse response(url);
  response.SetHttpStatusCode(200);
  response.SetHttpHeaderField(http_names::kCacheControl, "max-age=3600");
  response.SetHttpHeaderField(http_names::kVary, "*");
  RegisterMockedURLLoadWithCustomResponse(url, response);

  FetchParameters fetch_params_original{ResourceRequest(url)};
  Resource* resource =
      MockResource::Fetch(fetch_params_original, fetcher, nullptr);
  ASSERT_TRUE(resource);
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();
  ASSERT_TRUE(resource->MustReloadDueToVaryHeader(ResourceRequest(url)));

  FetchParameters fetch_params{ResourceRequest(url)};
  Resource* new_resource = MockResource::Fetch(fetch_params, fetcher, nullptr);
  EXPECT_EQ(resource, new_resource);
}

class RequestSameResourceOnComplete
    : public GarbageCollectedFinalized<RequestSameResourceOnComplete>,
      public RawResourceClient {
  USING_GARBAGE_COLLECTED_MIXIN(RequestSameResourceOnComplete);

 public:
  explicit RequestSameResourceOnComplete(FetchParameters& params,
                                         ResourceFetcher* fetcher)
      : notify_finished_called_(false),
        source_origin_(fetcher->GetProperties()
                           .GetFetchClientSettingsObject()
                           .GetSecurityOrigin()) {
    MockResource::Fetch(params, fetcher, this);
  }

  void NotifyFinished(Resource* resource) override {
    EXPECT_EQ(GetResource(), resource);
    auto* properties =
        MakeGarbageCollected<TestResourceFetcherProperties>(source_origin_);
    MockFetchContext* context = MakeGarbageCollected<MockFetchContext>();
    auto* fetcher2 = MakeGarbageCollected<ResourceFetcher>(ResourceFetcherInit(
        *properties, context, base::MakeRefCounted<scheduler::FakeTaskRunner>(),
        MakeGarbageCollected<TestLoaderFactory>()));
    ResourceRequest resource_request2(GetResource()->Url());
    resource_request2.SetCacheMode(mojom::FetchCacheMode::kValidateCache);
    FetchParameters fetch_params2(resource_request2);
    Resource* resource2 = MockResource::Fetch(fetch_params2, fetcher2, nullptr);
    EXPECT_EQ(GetResource(), resource2);
    notify_finished_called_ = true;
    ClearResource();
  }
  bool NotifyFinishedCalled() const { return notify_finished_called_; }

  void Trace(blink::Visitor* visitor) override {
    RawResourceClient::Trace(visitor);
  }

  String DebugName() const override { return "RequestSameResourceOnComplete"; }

 private:
  bool notify_finished_called_;
  scoped_refptr<const SecurityOrigin> source_origin_;
};

TEST_F(ResourceFetcherTest, RevalidateWhileFinishingLoading) {
  scoped_refptr<const SecurityOrigin> source_origin =
      SecurityOrigin::CreateUniqueOpaque();
  KURL url("http://127.0.0.1:8000/foo.png");

  ResourceResponse response(url);
  response.SetHttpStatusCode(200);
  response.SetHttpHeaderField(http_names::kCacheControl, "max-age=3600");
  response.SetHttpHeaderField(http_names::kETag, "1234567890");
  RegisterMockedURLLoadWithCustomResponse(url, response);

  ResourceFetcher* fetcher1 = CreateFetcher(
      *MakeGarbageCollected<TestResourceFetcherProperties>(source_origin));
  ResourceRequest request1(url);
  request1.SetHttpHeaderField(http_names::kCacheControl, "no-cache");
  FetchParameters fetch_params1(request1);
  Persistent<RequestSameResourceOnComplete> client =
      MakeGarbageCollected<RequestSameResourceOnComplete>(fetch_params1,
                                                          fetcher1);
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();
  EXPECT_TRUE(client->NotifyFinishedCalled());
}

// TODO(crbug.com/850785): Reenable this.
#if defined(OS_ANDROID)
#define MAYBE_DontReuseMediaDataUrl DISABLED_DontReuseMediaDataUrl
#else
#define MAYBE_DontReuseMediaDataUrl DontReuseMediaDataUrl
#endif
TEST_F(ResourceFetcherTest, MAYBE_DontReuseMediaDataUrl) {
  auto* fetcher = CreateFetcher();
  ResourceRequest request(KURL("data:text/html,foo"));
  request.SetRequestContext(mojom::RequestContextType::VIDEO);
  ResourceLoaderOptions options;
  options.data_buffering_policy = kDoNotBufferData;
  options.initiator_info.name = fetch_initiator_type_names::kInternal;
  FetchParameters fetch_params(request, options);
  Resource* resource1 = RawResource::FetchMedia(fetch_params, fetcher, nullptr);
  Resource* resource2 = RawResource::FetchMedia(fetch_params, fetcher, nullptr);
  EXPECT_NE(resource1, resource2);
}

class ServeRequestsOnCompleteClient final
    : public GarbageCollectedFinalized<ServeRequestsOnCompleteClient>,
      public RawResourceClient {
  USING_GARBAGE_COLLECTED_MIXIN(ServeRequestsOnCompleteClient);

 public:
  void NotifyFinished(Resource*) override {
    Platform::Current()->GetURLLoaderMockFactory()->ServeAsynchronousRequests();
    ClearResource();
  }

  // No callbacks should be received except for the NotifyFinished() triggered
  // by ResourceLoader::Cancel().
  void DataSent(Resource*, uint64_t, uint64_t) override { ASSERT_TRUE(false); }
  void ResponseReceived(Resource*, const ResourceResponse&) override {
    ASSERT_TRUE(false);
  }
  void SetSerializedCachedMetadata(Resource*, const uint8_t*, size_t) override {
    ASSERT_TRUE(false);
  }
  void DataReceived(Resource*, const char*, size_t) override {
    ASSERT_TRUE(false);
  }
  bool RedirectReceived(Resource*,
                        const ResourceRequest&,
                        const ResourceResponse&) override {
    ADD_FAILURE();
    return true;
  }
  void DataDownloaded(Resource*, uint64_t) override { ASSERT_TRUE(false); }
  void DidReceiveResourceTiming(Resource*, const ResourceTimingInfo&) override {
    ASSERT_TRUE(false);
  }

  void Trace(blink::Visitor* visitor) override {
    RawResourceClient::Trace(visitor);
  }

  String DebugName() const override { return "ServeRequestsOnCompleteClient"; }
};

// Regression test for http://crbug.com/594072.
// This emulates a modal dialog triggering a nested run loop inside
// ResourceLoader::Cancel(). If the ResourceLoader doesn't promptly cancel its
// WebURLLoader before notifying its clients, a nested run loop  may send a
// network response, leading to an invalid state transition in ResourceLoader.
TEST_F(ResourceFetcherTest, ResponseOnCancel) {
  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  auto* fetcher = CreateFetcher();
  ResourceRequest resource_request(url);
  resource_request.SetRequestContext(mojom::RequestContextType::INTERNAL);
  FetchParameters fetch_params(resource_request);
  Persistent<ServeRequestsOnCompleteClient> client =
      MakeGarbageCollected<ServeRequestsOnCompleteClient>();
  Resource* resource = RawResource::Fetch(fetch_params, fetcher, client);
  resource->Loader()->Cancel();
}

class ScopedMockRedirectRequester {
  STACK_ALLOCATED();

 public:
  ScopedMockRedirectRequester(
      MockFetchContext* context,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : context_(context), task_runner_(std::move(task_runner)) {}

  void RegisterRedirect(const WebString& from_url, const WebString& to_url) {
    KURL redirect_url(from_url);
    WebURLResponse redirect_response;
    redirect_response.SetCurrentRequestUrl(redirect_url);
    redirect_response.SetHttpStatusCode(301);
    redirect_response.SetHttpHeaderField(http_names::kLocation, to_url);
    redirect_response.SetEncodedDataLength(kRedirectResponseOverheadBytes);
    Platform::Current()->GetURLLoaderMockFactory()->RegisterURL(
        redirect_url, redirect_response, "");
  }

  void RegisterFinalResource(const WebString& url) {
    KURL final_url(url);
    RegisterMockedURLLoad(final_url);
  }

  void Request(const WebString& url) {
    auto* properties = MakeGarbageCollected<TestResourceFetcherProperties>();
    auto* fetcher = MakeGarbageCollected<ResourceFetcher>(
        ResourceFetcherInit(*properties, context_, task_runner_,
                            MakeGarbageCollected<TestLoaderFactory>()));
    ResourceRequest resource_request(url);
    resource_request.SetRequestContext(mojom::RequestContextType::INTERNAL);
    FetchParameters fetch_params(resource_request);
    RawResource::Fetch(fetch_params, fetcher, nullptr);
    Platform::Current()->GetURLLoaderMockFactory()->ServeAsynchronousRequests();
  }

 private:
  Member<MockFetchContext> context_;
  const scoped_refptr<base::SingleThreadTaskRunner> task_runner_;

  DISALLOW_COPY_AND_ASSIGN(ScopedMockRedirectRequester);
};

TEST_F(ResourceFetcherTest, SameOriginRedirect) {
  const char kRedirectURL[] = "http://127.0.0.1:8000/redirect.html";
  const char kFinalURL[] = "http://127.0.0.1:8000/final.html";
  MockFetchContext* context = MakeGarbageCollected<MockFetchContext>();
  ScopedMockRedirectRequester requester(context, CreateTaskRunner());
  requester.RegisterRedirect(kRedirectURL, kFinalURL);
  requester.RegisterFinalResource(kFinalURL);
  requester.Request(kRedirectURL);

  EXPECT_EQ(kRedirectResponseOverheadBytes + kTestResourceSize,
            context->GetTransferSize());
}

TEST_F(ResourceFetcherTest, CrossOriginRedirect) {
  const char kRedirectURL[] = "http://otherorigin.test/redirect.html";
  const char kFinalURL[] = "http://127.0.0.1:8000/final.html";
  MockFetchContext* context = MakeGarbageCollected<MockFetchContext>();
  ScopedMockRedirectRequester requester(context, CreateTaskRunner());
  requester.RegisterRedirect(kRedirectURL, kFinalURL);
  requester.RegisterFinalResource(kFinalURL);
  requester.Request(kRedirectURL);

  EXPECT_EQ(kTestResourceSize, context->GetTransferSize());
}

TEST_F(ResourceFetcherTest, ComplexCrossOriginRedirect) {
  const char kRedirectURL1[] = "http://127.0.0.1:8000/redirect1.html";
  const char kRedirectURL2[] = "http://otherorigin.test/redirect2.html";
  const char kRedirectURL3[] = "http://127.0.0.1:8000/redirect3.html";
  const char kFinalURL[] = "http://127.0.0.1:8000/final.html";
  MockFetchContext* context = MakeGarbageCollected<MockFetchContext>();
  ScopedMockRedirectRequester requester(context, CreateTaskRunner());
  requester.RegisterRedirect(kRedirectURL1, kRedirectURL2);
  requester.RegisterRedirect(kRedirectURL2, kRedirectURL3);
  requester.RegisterRedirect(kRedirectURL3, kFinalURL);
  requester.RegisterFinalResource(kFinalURL);
  requester.Request(kRedirectURL1);

  EXPECT_EQ(kTestResourceSize, context->GetTransferSize());
}

TEST_F(ResourceFetcherTest, SynchronousRequest) {
  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  auto* fetcher = CreateFetcher();
  ResourceRequest resource_request(url);
  resource_request.SetRequestContext(mojom::RequestContextType::INTERNAL);
  FetchParameters fetch_params(resource_request);
  fetch_params.MakeSynchronous();
  Resource* resource = RawResource::Fetch(fetch_params, fetcher, nullptr);
  EXPECT_TRUE(resource->IsLoaded());
  EXPECT_EQ(ResourceLoadPriority::kHighest,
            resource->GetResourceRequest().Priority());
}

TEST_F(ResourceFetcherTest, PingPriority) {
  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  auto* fetcher = CreateFetcher();
  ResourceRequest resource_request(url);
  resource_request.SetRequestContext(mojom::RequestContextType::PING);
  FetchParameters fetch_params(resource_request);
  Resource* resource = RawResource::Fetch(fetch_params, fetcher, nullptr);
  EXPECT_EQ(ResourceLoadPriority::kVeryLow,
            resource->GetResourceRequest().Priority());
}

TEST_F(ResourceFetcherTest, PreloadResourceTwice) {
  auto* fetcher = CreateFetcher();

  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  FetchParameters fetch_params_original{ResourceRequest(url)};
  fetch_params_original.SetLinkPreload(true);
  Resource* resource =
      MockResource::Fetch(fetch_params_original, fetcher, nullptr);
  ASSERT_TRUE(resource);
  EXPECT_TRUE(resource->IsLinkPreload());
  EXPECT_TRUE(fetcher->ContainsAsPreload(resource));
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();

  FetchParameters fetch_params{ResourceRequest(url)};
  fetch_params.SetLinkPreload(true);
  Resource* new_resource = MockResource::Fetch(fetch_params, fetcher, nullptr);
  EXPECT_EQ(resource, new_resource);
  EXPECT_TRUE(fetcher->ContainsAsPreload(resource));

  fetcher->ClearPreloads(ResourceFetcher::kClearAllPreloads);
  EXPECT_FALSE(fetcher->ContainsAsPreload(resource));
  EXPECT_FALSE(GetMemoryCache()->Contains(resource));
  EXPECT_TRUE(resource->IsUnusedPreload());
}

TEST_F(ResourceFetcherTest, LinkPreloadResourceAndUse) {
  auto* fetcher = CreateFetcher();

  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  // Link preload preload scanner
  FetchParameters fetch_params_original{ResourceRequest(url)};
  fetch_params_original.SetLinkPreload(true);
  Resource* resource =
      MockResource::Fetch(fetch_params_original, fetcher, nullptr);
  ASSERT_TRUE(resource);
  EXPECT_TRUE(resource->IsLinkPreload());
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();

  // Resource created by preload scanner
  FetchParameters fetch_params_preload_scanner{ResourceRequest(url)};
  Resource* preload_scanner_resource =
      MockResource::Fetch(fetch_params_preload_scanner, fetcher, nullptr);
  EXPECT_EQ(resource, preload_scanner_resource);
  EXPECT_FALSE(resource->IsLinkPreload());

  // Resource created by parser
  FetchParameters fetch_params{ResourceRequest(url)};
  Persistent<MockResourceClient> client =
      MakeGarbageCollected<MockResourceClient>();
  Resource* new_resource = MockResource::Fetch(fetch_params, fetcher, client);
  EXPECT_EQ(resource, new_resource);
  EXPECT_FALSE(resource->IsLinkPreload());

  // DCL reached
  fetcher->ClearPreloads(ResourceFetcher::kClearSpeculativeMarkupPreloads);
  EXPECT_TRUE(GetMemoryCache()->Contains(resource));
  EXPECT_FALSE(resource->IsUnusedPreload());
}

TEST_F(ResourceFetcherTest, PreloadMatchWithBypassingCache) {
  auto* fetcher = CreateFetcher();
  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  FetchParameters fetch_params_original{ResourceRequest(url)};
  fetch_params_original.SetLinkPreload(true);
  Resource* resource =
      MockResource::Fetch(fetch_params_original, fetcher, nullptr);
  ASSERT_TRUE(resource);
  EXPECT_TRUE(resource->IsLinkPreload());
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();

  FetchParameters fetch_params_second{ResourceRequest(url)};
  fetch_params_second.MutableResourceRequest().SetCacheMode(
      mojom::FetchCacheMode::kBypassCache);
  Resource* second_resource =
      MockResource::Fetch(fetch_params_second, fetcher, nullptr);
  EXPECT_EQ(resource, second_resource);
  EXPECT_FALSE(resource->IsLinkPreload());
}

TEST_F(ResourceFetcherTest, CrossFramePreloadMatchIsNotAllowed) {
  auto* fetcher = CreateFetcher();
  auto* fetcher2 = CreateFetcher();

  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  FetchParameters fetch_params_original{ResourceRequest(url)};
  fetch_params_original.SetLinkPreload(true);
  Resource* resource =
      MockResource::Fetch(fetch_params_original, fetcher, nullptr);
  ASSERT_TRUE(resource);
  EXPECT_TRUE(resource->IsLinkPreload());
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();

  FetchParameters fetch_params_second{ResourceRequest(url)};
  fetch_params_second.MutableResourceRequest().SetCacheMode(
      mojom::FetchCacheMode::kBypassCache);
  Resource* second_resource =
      MockResource::Fetch(fetch_params_second, fetcher2, nullptr);

  EXPECT_NE(resource, second_resource);
  EXPECT_TRUE(resource->IsLinkPreload());
}

TEST_F(ResourceFetcherTest, RepetitiveLinkPreloadShouldBeMerged) {
  auto* fetcher = CreateFetcher();

  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  FetchParameters fetch_params_for_request{ResourceRequest(url)};
  FetchParameters fetch_params_for_preload{ResourceRequest(url)};
  fetch_params_for_preload.SetLinkPreload(true);

  Resource* resource1 =
      MockResource::Fetch(fetch_params_for_preload, fetcher, nullptr);
  ASSERT_TRUE(resource1);
  EXPECT_TRUE(resource1->IsUnusedPreload());
  EXPECT_TRUE(fetcher->ContainsAsPreload(resource1));
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();

  // The second preload fetch returns the first preload.
  Resource* resource2 =
      MockResource::Fetch(fetch_params_for_preload, fetcher, nullptr);
  EXPECT_TRUE(fetcher->ContainsAsPreload(resource1));
  EXPECT_TRUE(resource1->IsUnusedPreload());
  EXPECT_EQ(resource1, resource2);

  // preload matching
  Resource* resource3 =
      MockResource::Fetch(fetch_params_for_request, fetcher, nullptr);
  EXPECT_EQ(resource1, resource3);
  EXPECT_FALSE(fetcher->ContainsAsPreload(resource1));
  EXPECT_FALSE(resource1->IsUnusedPreload());
}

TEST_F(ResourceFetcherTest, RepetitiveSpeculativePreloadShouldBeMerged) {
  auto* fetcher = CreateFetcher();

  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  FetchParameters fetch_params_for_request{ResourceRequest(url)};
  FetchParameters fetch_params_for_preload{ResourceRequest(url)};
  fetch_params_for_preload.SetSpeculativePreloadType(
      FetchParameters::SpeculativePreloadType::kInDocument);

  Resource* resource1 =
      MockResource::Fetch(fetch_params_for_preload, fetcher, nullptr);
  ASSERT_TRUE(resource1);
  EXPECT_TRUE(resource1->IsUnusedPreload());
  EXPECT_TRUE(fetcher->ContainsAsPreload(resource1));
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();

  // The second preload fetch returns the first preload.
  Resource* resource2 =
      MockResource::Fetch(fetch_params_for_preload, fetcher, nullptr);
  EXPECT_TRUE(fetcher->ContainsAsPreload(resource1));
  EXPECT_TRUE(resource1->IsUnusedPreload());
  EXPECT_EQ(resource1, resource2);

  // preload matching
  Resource* resource3 =
      MockResource::Fetch(fetch_params_for_request, fetcher, nullptr);
  EXPECT_EQ(resource1, resource3);
  EXPECT_FALSE(fetcher->ContainsAsPreload(resource1));
  EXPECT_FALSE(resource1->IsUnusedPreload());
}

TEST_F(ResourceFetcherTest, SpeculativePreloadShouldBePromotedToLinkePreload) {
  auto* fetcher = CreateFetcher();

  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  FetchParameters fetch_params_for_request{ResourceRequest(url)};
  FetchParameters fetch_params_for_speculative_preload{ResourceRequest(url)};
  fetch_params_for_speculative_preload.SetSpeculativePreloadType(
      FetchParameters::SpeculativePreloadType::kInDocument);
  FetchParameters fetch_params_for_link_preload{ResourceRequest(url)};
  fetch_params_for_link_preload.SetLinkPreload(true);

  Resource* resource1 = MockResource::Fetch(
      fetch_params_for_speculative_preload, fetcher, nullptr);
  ASSERT_TRUE(resource1);
  EXPECT_TRUE(resource1->IsUnusedPreload());
  EXPECT_FALSE(resource1->IsLinkPreload());
  EXPECT_TRUE(fetcher->ContainsAsPreload(resource1));
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();

  // The second preload fetch returns the first preload.
  Resource* resource2 =
      MockResource::Fetch(fetch_params_for_link_preload, fetcher, nullptr);
  EXPECT_TRUE(fetcher->ContainsAsPreload(resource1));
  EXPECT_TRUE(resource1->IsUnusedPreload());
  EXPECT_TRUE(resource1->IsLinkPreload());
  EXPECT_EQ(resource1, resource2);

  // preload matching
  Resource* resource3 =
      MockResource::Fetch(fetch_params_for_request, fetcher, nullptr);
  EXPECT_EQ(resource1, resource3);
  EXPECT_FALSE(fetcher->ContainsAsPreload(resource1));
  EXPECT_FALSE(resource1->IsUnusedPreload());
  EXPECT_FALSE(resource1->IsLinkPreload());
}

TEST_F(ResourceFetcherTest, Revalidate304) {
  scoped_refptr<const SecurityOrigin> source_origin =
      SecurityOrigin::CreateUniqueOpaque();

  KURL url("http://127.0.0.1:8000/foo.html");
  Resource* resource =
      RawResource::CreateForTest(url, source_origin, ResourceType::kRaw);
  AddResourceToMemoryCache(resource);

  ResourceResponse response(url);
  response.SetHttpStatusCode(304);
  response.SetHttpHeaderField("etag", "1234567890");
  resource->ResponseReceived(response);
  resource->FinishForTest();

  auto* fetcher = CreateFetcher(
      *MakeGarbageCollected<TestResourceFetcherProperties>(source_origin));
  ResourceRequest resource_request(url);
  resource_request.SetRequestContext(mojom::RequestContextType::INTERNAL);
  FetchParameters fetch_params(resource_request);
  platform_->GetURLLoaderMockFactory()->RegisterURL(url, WebURLResponse(), "");
  Resource* new_resource = RawResource::Fetch(fetch_params, fetcher, nullptr);
  fetcher->StopFetching();

  EXPECT_NE(resource, new_resource);
}

TEST_F(ResourceFetcherTest, LinkPreloadResourceMultipleFetchersAndMove) {
  auto* fetcher = CreateFetcher();
  auto* fetcher2 = CreateFetcher();

  KURL url("http://127.0.0.1:8000/foo.png");
  RegisterMockedURLLoad(url);

  FetchParameters fetch_params_original{ResourceRequest(url)};
  fetch_params_original.SetLinkPreload(true);
  Resource* resource =
      MockResource::Fetch(fetch_params_original, fetcher, nullptr);
  ASSERT_TRUE(resource);
  EXPECT_TRUE(resource->IsLinkPreload());
  EXPECT_EQ(0, fetcher->BlockingRequestCount());

  // Resource created by parser on the second fetcher
  FetchParameters fetch_params2{ResourceRequest(url)};
  Persistent<MockResourceClient> client2 =
      MakeGarbageCollected<MockResourceClient>();
  Resource* new_resource2 =
      MockResource::Fetch(fetch_params2, fetcher2, client2);
  EXPECT_NE(resource, new_resource2);
  EXPECT_EQ(0, fetcher2->BlockingRequestCount());
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();
}

// TODO(crbug.com/850785): Reenable this.
#if defined(OS_ANDROID)
#define MAYBE_ContentTypeDataURL DISABLED_ContentTypeDataURL
#else
#define MAYBE_ContentTypeDataURL ContentTypeDataURL
#endif
TEST_F(ResourceFetcherTest, MAYBE_ContentTypeDataURL) {
  auto* fetcher = CreateFetcher();
  FetchParameters fetch_params{ResourceRequest("data:text/testmimetype,foo")};
  Resource* resource = MockResource::Fetch(fetch_params, fetcher, nullptr);
  ASSERT_TRUE(resource);
  EXPECT_EQ(ResourceStatus::kCached, resource->GetStatus());
  EXPECT_EQ("text/testmimetype", resource->GetResponse().MimeType());
  EXPECT_EQ("text/testmimetype", resource->GetResponse().HttpContentType());
}

// Request with the Content-ID scheme must not be canceled, even if there is no
// MHTMLArchive to serve them.
// Note: Not blocking it is important because there are some embedders of
// Android WebView that are intercepting Content-ID URLs and serve their own
// resources. Please see https://crbug.com/739658.
TEST_F(ResourceFetcherTest, ContentIdURL) {
  KURL url("cid:0123456789@example.com");
  ResourceResponse response(url);
  response.SetHttpStatusCode(200);
  RegisterMockedURLLoadWithCustomResponse(url, response);

  auto* fetcher = CreateFetcher();

  // Subresource case.
  {
    ResourceRequest resource_request(url);
    resource_request.SetRequestContext(mojom::RequestContextType::VIDEO);
    FetchParameters fetch_params(resource_request);
    RawResource* resource =
        RawResource::FetchMedia(fetch_params, fetcher, nullptr);
    ASSERT_NE(nullptr, resource);
    EXPECT_FALSE(resource->ErrorOccurred());
  }
}

TEST_F(ResourceFetcherTest, StaleWhileRevalidate) {
  scoped_refptr<const SecurityOrigin> source_origin =
      SecurityOrigin::CreateUniqueOpaque();
  auto* observer = MakeGarbageCollected<TestResourceLoadObserver>();
  MockFetchContext* context = MakeGarbageCollected<MockFetchContext>();
  auto* fetcher = CreateFetcher(
      *MakeGarbageCollected<TestResourceFetcherProperties>(source_origin),
      context);
  fetcher->SetResourceLoadObserver(observer);

  KURL url("http://127.0.0.1:8000/foo.html");
  FetchParameters fetch_params{ResourceRequest(url)};

  ResourceResponse response(url);
  response.SetHttpStatusCode(200);
  response.SetHttpHeaderField(http_names::kCacheControl,
                              "max-age=0, stale-while-revalidate=40");

  RegisterMockedURLLoadWithCustomResponse(url, response);
  Resource* resource = MockResource::Fetch(fetch_params, fetcher, nullptr);
  ASSERT_TRUE(resource);

  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();
  EXPECT_TRUE(resource->IsLoaded());
  EXPECT_TRUE(GetMemoryCache()->Contains(resource));

  fetcher->SetStaleWhileRevalidateEnabled(true);
  ResourceRequest resource_request(url);
  resource_request.SetRequestContext(mojom::RequestContextType::INTERNAL);
  fetch_params = FetchParameters(resource_request);
  Resource* new_resource = MockResource::Fetch(fetch_params, fetcher, nullptr);
  EXPECT_EQ(resource, new_resource);
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();
  EXPECT_TRUE(resource->IsLoaded());

  // Advance the clock, make sure the original resource gets removed from the
  // memory cache after the revalidation completes.
  platform_->AdvanceClockSeconds(1);
  ResourceResponse revalidate_response(url);
  revalidate_response.SetHttpStatusCode(200);
  platform_->GetURLLoaderMockFactory()->UnregisterURL(url);
  RegisterMockedURLLoadWithCustomResponse(url, revalidate_response);
  new_resource = MockResource::Fetch(fetch_params, fetcher, nullptr);
  EXPECT_EQ(resource, new_resource);
  EXPECT_TRUE(GetMemoryCache()->Contains(resource));
  static_cast<scheduler::FakeTaskRunner*>(fetcher->GetTaskRunner().get())
      ->RunUntilIdle();
  base::Optional<ResourceRequest> swr_request = observer->GetLastRequest();
  ASSERT_TRUE(swr_request.has_value());
  EXPECT_EQ(ResourceLoadPriority::kVeryLow, swr_request->Priority());
  platform_->GetURLLoaderMockFactory()->ServeAsynchronousRequests();
  EXPECT_FALSE(GetMemoryCache()->Contains(resource));
}

TEST_F(ResourceFetcherTest, CachedResourceShouldNotCrashByNullURL) {
  auto* fetcher = CreateFetcher();

  // Make sure |cached_resources_map_| is not empty, so that HashMap lookup
  // won't take a fast path.
  KURL url("http://127.0.0.1:8000/foo.html");
  ResourceResponse response(url);
  response.SetHttpStatusCode(200);
  RegisterMockedURLLoadWithCustomResponse(url, response);
  FetchParameters fetch_params{ResourceRequest(url)};
  MockResource::Fetch(fetch_params, fetcher, nullptr);
  ASSERT_NE(fetcher->CachedResource(url), nullptr);

  ASSERT_EQ(fetcher->CachedResource(KURL()), nullptr);
}

TEST_F(ResourceFetcherTest, DetachedPropertiesWithDefaultValues) {
  const auto& original_client_settings_object =
      *MakeGarbageCollected<FetchClientSettingsObjectSnapshot>(
          KURL("https://example.com/foo.html"),
          KURL("https://example.com/foo.html"),
          SecurityOrigin::Create(KURL("https://example.com/")),
          network::mojom::ReferrerPolicy::kDefault,
          "https://example.com/foo.html", HttpsState::kModern,
          AllowedByNosniff::MimeTypeCheck::kStrict,
          mojom::IPAddressSpace::kPublic);
  const auto& original_properties =
      *MakeGarbageCollected<TestResourceFetcherProperties>(
          original_client_settings_object);
  auto* const fetcher = CreateFetcher(original_properties);
  const auto& properties = fetcher->GetProperties();

  EXPECT_NE(&original_properties, &properties);

  const auto& client_settings_object =
      properties.GetFetchClientSettingsObject();
  EXPECT_EQ(&original_client_settings_object, &client_settings_object);
  EXPECT_FALSE(properties.IsMainFrame());
  EXPECT_EQ(properties.GetControllerServiceWorkerMode(),
            mojom::ControllerServiceWorkerMode::kNoController);
  // We cannot call ServiceWorkerId as the service worker mode is kNoController.
  EXPECT_FALSE(properties.IsPaused());
  EXPECT_FALSE(properties.IsDetached());
  EXPECT_FALSE(properties.IsLoadComplete());
  EXPECT_FALSE(properties.ShouldBlockLoadingSubResource());
  EXPECT_EQ(scheduler::FrameStatus::kNone, properties.GetFrameStatus());

  fetcher->ClearContext();
  // ResourceFetcher::GetProperties always returns the same object.
  EXPECT_EQ(&properties, &fetcher->GetProperties());

  EXPECT_NE(&client_settings_object,
            &properties.GetFetchClientSettingsObject());
  EXPECT_EQ(properties.GetFetchClientSettingsObject().BaseUrl(),
            KURL("https://example.com/foo.html"));
  EXPECT_FALSE(properties.IsMainFrame());
  EXPECT_EQ(properties.GetControllerServiceWorkerMode(),
            mojom::ControllerServiceWorkerMode::kNoController);
  // We cannot call ServiceWorkerId as the service worker mode is kNoController.
  EXPECT_FALSE(properties.IsPaused());
  EXPECT_TRUE(properties.IsDetached());
  EXPECT_FALSE(properties.IsLoadComplete());
  EXPECT_TRUE(properties.ShouldBlockLoadingSubResource());
  EXPECT_EQ(scheduler::FrameStatus::kNone, properties.GetFrameStatus());
}

TEST_F(ResourceFetcherTest, DetachedPropertiesWithNonDefaultValues) {
  const auto& original_client_settings_object =
      *MakeGarbageCollected<FetchClientSettingsObjectSnapshot>(
          KURL("https://example.com/foo.html"),
          KURL("https://example.com/foo.html"),
          SecurityOrigin::Create(KURL("https://example.com/")),
          network::mojom::ReferrerPolicy::kDefault,
          "https://example.com/foo.html", HttpsState::kModern,
          AllowedByNosniff::MimeTypeCheck::kStrict,
          mojom::IPAddressSpace::kPublic);
  auto& original_properties =
      *MakeGarbageCollected<TestResourceFetcherProperties>(
          original_client_settings_object);
  auto* const fetcher = CreateFetcher(original_properties);
  const auto& properties = fetcher->GetProperties();

  EXPECT_NE(&original_properties, &properties);

  original_properties.SetIsMainFrame(true);
  original_properties.SetControllerServiceWorkerMode(
      mojom::ControllerServiceWorkerMode::kControlled);
  original_properties.SetServiceWorkerId(133);
  original_properties.SetIsPaused(true);
  original_properties.SetIsLoadComplete(true);
  original_properties.SetShouldBlockLoadingSubResource(true);
  original_properties.SetFrameStatus(scheduler::FrameStatus::kMainFrameVisible);

  const auto& client_settings_object =
      properties.GetFetchClientSettingsObject();
  EXPECT_EQ(&original_client_settings_object, &client_settings_object);
  EXPECT_TRUE(properties.IsMainFrame());
  EXPECT_EQ(properties.GetControllerServiceWorkerMode(),
            mojom::ControllerServiceWorkerMode::kControlled);
  EXPECT_EQ(properties.ServiceWorkerId(), 133);
  EXPECT_TRUE(properties.IsPaused());
  EXPECT_FALSE(properties.IsDetached());
  EXPECT_TRUE(properties.IsLoadComplete());
  EXPECT_TRUE(properties.ShouldBlockLoadingSubResource());
  EXPECT_EQ(scheduler::FrameStatus::kMainFrameVisible,
            properties.GetFrameStatus());

  fetcher->ClearContext();
  // ResourceFetcher::GetProperties always returns the same object.
  EXPECT_EQ(&properties, &fetcher->GetProperties());

  EXPECT_NE(&client_settings_object,
            &properties.GetFetchClientSettingsObject());
  EXPECT_EQ(properties.GetFetchClientSettingsObject().BaseUrl(),
            KURL("https://example.com/foo.html"));
  EXPECT_TRUE(properties.IsMainFrame());
  EXPECT_EQ(properties.GetControllerServiceWorkerMode(),
            mojom::ControllerServiceWorkerMode::kNoController);
  // We cannot call ServiceWorkerId as the service worker mode is kNoController.
  EXPECT_TRUE(properties.IsPaused());
  EXPECT_TRUE(properties.IsDetached());
  EXPECT_TRUE(properties.IsLoadComplete());
  EXPECT_TRUE(properties.ShouldBlockLoadingSubResource());
  EXPECT_EQ(scheduler::FrameStatus::kNone, properties.GetFrameStatus());
}

}  // namespace blink
