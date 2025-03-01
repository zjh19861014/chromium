// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/credentialmanager/credentials_container.h"

#include <memory>
#include <utility>

#include "base/macros.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/credentialmanager/credential_manager.mojom-blink.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_testing.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_gc_controller.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/frame_test_helpers.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/testing/gc_object_liveness_observer.h"
#include "third_party/blink/renderer/core/testing/test_document_interface_broker.h"
#include "third_party/blink/renderer/modules/credentialmanager/credential.h"
#include "third_party/blink/renderer/modules/credentialmanager/credential_manager_proxy.h"
#include "third_party/blink/renderer/modules/credentialmanager/credential_request_options.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

namespace {

class MockCredentialManager : public mojom::blink::CredentialManager {
 public:
  MockCredentialManager() : binding_(this) {}
  ~MockCredentialManager() override {}

  void Bind(::blink::mojom::blink::CredentialManagerRequest request) {
    binding_.Bind(std::move(request));
  }

  void WaitForConnectionError() {
    if (!binding_.is_bound())
      return;

    binding_.set_connection_error_handler(WTF::Bind(&test::ExitRunLoop));
    test::EnterRunLoop();
  }

  void WaitForCallToGet() {
    if (get_callback_)
      return;

    test::EnterRunLoop();
  }

  void InvokeGetCallback() {
    EXPECT_TRUE(binding_.is_bound());

    auto info = blink::mojom::blink::CredentialInfo::New();
    info->type = blink::mojom::blink::CredentialType::EMPTY;
    info->federation = SecurityOrigin::CreateUniqueOpaque();
    std::move(get_callback_)
        .Run(blink::mojom::blink::CredentialManagerError::SUCCESS,
             std::move(info));
  }

 protected:
  void Store(blink::mojom::blink::CredentialInfoPtr credential,
             StoreCallback callback) override {}
  void PreventSilentAccess(PreventSilentAccessCallback callback) override {}
  void Get(blink::mojom::blink::CredentialMediationRequirement mediation,
           bool include_passwords,
           const WTF::Vector<::blink::KURL>& federations,
           GetCallback callback) override {
    get_callback_ = std::move(callback);
    test::ExitRunLoop();
  }

 private:
  mojo::Binding<::blink::mojom::blink::CredentialManager> binding_;

  GetCallback get_callback_;

  DISALLOW_COPY_AND_ASSIGN(MockCredentialManager);
};

class MockCredentialManagerDocumentInterfaceBroker
    : public TestDocumentInterfaceBroker {
 public:
  MockCredentialManagerDocumentInterfaceBroker(
      mojom::blink::DocumentInterfaceBroker* document_interface_broker,
      mojom::blink::DocumentInterfaceBrokerRequest request,
      MockCredentialManager* mock_credential_manager)
      : TestDocumentInterfaceBroker(document_interface_broker,
                                    std::move(request)),
        mock_credential_manager_(mock_credential_manager) {}

  void GetCredentialManager(
      mojom::blink::CredentialManagerRequest request) override {
    mock_credential_manager_->Bind(std::move(request));
  }

  void GetAuthenticator(mojom::blink::AuthenticatorRequest request) override {}

 private:
  MockCredentialManager* mock_credential_manager_;
};

class CredentialManagerTestingContext {
  STACK_ALLOCATED();

 public:
  CredentialManagerTestingContext(
      MockCredentialManager* mock_credential_manager) {
    dummy_context_.GetDocument().SetSecurityOrigin(
        SecurityOrigin::CreateFromString("https://example.test"));
    dummy_context_.GetDocument().SetSecureContextStateForTesting(
        SecureContextState::kSecure);
    mojom::blink::DocumentInterfaceBrokerPtr doc;
    broker_ = std::make_unique<MockCredentialManagerDocumentInterfaceBroker>(
        &dummy_context_.GetFrame().GetDocumentInterfaceBroker(),
        mojo::MakeRequest(&doc), mock_credential_manager);
    dummy_context_.GetFrame().SetDocumentInterfaceBrokerForTesting(
        doc.PassInterface().PassHandle());
  }

  Document* GetDocument() { return &dummy_context_.GetDocument(); }
  LocalFrame* Frame() { return &dummy_context_.GetFrame(); }
  ScriptState* GetScriptState() { return dummy_context_.GetScriptState(); }

 private:
  V8TestingScope dummy_context_;
  std::unique_ptr<MockCredentialManagerDocumentInterfaceBroker> broker_;
};

}  // namespace

class MockPublicKeyCredential : public Credential {
 public:
  MockPublicKeyCredential() : Credential("test", "public-key") {}
  bool IsPublicKeyCredential() const override { return true; }
};

// The completion callbacks for pending mojom::CredentialManager calls each own
// a persistent handle to a ScriptPromiseResolver instance. Ensure that if the
// document is destored while a call is pending, it can still be freed up.
TEST(CredentialsContainerTest, PendingGetRequest_NoGCCycles) {
  MockCredentialManager mock_credential_manager;
  GCObjectLivenessObserver<Document> document_observer;

  {
    CredentialManagerTestingContext context(&mock_credential_manager);
    document_observer.Observe(context.GetDocument());
    MakeGarbageCollected<CredentialsContainer>()->get(
        context.GetScriptState(), CredentialRequestOptions::Create());
    mock_credential_manager.WaitForCallToGet();
  }

  V8GCController::CollectAllGarbageForTesting(
      v8::Isolate::GetCurrent(),
      v8::EmbedderHeapTracer::EmbedderStackState::kEmpty);
  ThreadState::Current()->CollectAllGarbageForTesting();

  ASSERT_TRUE(document_observer.WasCollected());

  mock_credential_manager.InvokeGetCallback();
  mock_credential_manager.WaitForConnectionError();
}

// If the document is detached before the request is resolved, the promise
// should be left unresolved, and there should be no crashes.
TEST(CredentialsContainerTest,
     PendingGetRequest_NoCrashOnResponseAfterDocumentShutdown) {
  MockCredentialManager mock_credential_manager;
  CredentialManagerTestingContext context(&mock_credential_manager);

  auto* proxy = CredentialManagerProxy::From(*context.GetDocument());
  auto promise = MakeGarbageCollected<CredentialsContainer>()->get(
      context.GetScriptState(), CredentialRequestOptions::Create());
  mock_credential_manager.WaitForCallToGet();

  context.GetDocument()->Shutdown();

  mock_credential_manager.InvokeGetCallback();
  proxy->FlushCredentialManagerConnectionForTesting();

  EXPECT_EQ(v8::Promise::kPending,
            promise.V8Value().As<v8::Promise>()->State());
}

TEST(CredentialsContainerTest, RejectPublicKeyCredentialStoreOperation) {
  MockCredentialManager mock_credential_manager;
  CredentialManagerTestingContext context(&mock_credential_manager);

  auto promise = MakeGarbageCollected<CredentialsContainer>()->store(
      context.GetScriptState(),
      MakeGarbageCollected<MockPublicKeyCredential>());

  EXPECT_EQ(v8::Promise::kRejected,
            promise.V8Value().As<v8::Promise>()->State());
}

}  // namespace blink
