// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WEBAUTHN_AUTHENTICATOR_REQUEST_SCHEDULER_H_
#define CHROME_BROWSER_WEBAUTHN_AUTHENTICATOR_REQUEST_SCHEDULER_H_

#include <memory>

#include "base/macros.h"

class ChromeAuthenticatorRequestDelegate;

namespace content {
class RenderFrameHost;
class WebContents;
}

// Responsible for scheduling simultaneous Web Authentication API requests
// coming from RenderFrameHosts in various WebContents and BrowserContexts, so
// that the UI flow can be shown in a reasonable manner.
//
// For now, the logic is very simple: at most one active request is allowed in
// each WebContents at any time (additional requests are cancelled).
class AuthenticatorRequestScheduler {
 public:
  AuthenticatorRequestScheduler() = default;
  ~AuthenticatorRequestScheduler() = default;

  // Returns a nullptr delegate if there is already an ongoing request in the
  // same WebContents.
  static std::unique_ptr<ChromeAuthenticatorRequestDelegate>
  CreateRequestDelegate(content::RenderFrameHost* render_frame_host);

  // Returns the current request delegate associated to the |web_contents| or
  // nullptr if there is none.
  static ChromeAuthenticatorRequestDelegate* GetRequestDelegateForTest(
      content::WebContents* web_contents);

 private:
  DISALLOW_COPY_AND_ASSIGN(AuthenticatorRequestScheduler);
};

#endif  // CHROME_BROWSER_WEBAUTHN_AUTHENTICATOR_REQUEST_SCHEDULER_H_
