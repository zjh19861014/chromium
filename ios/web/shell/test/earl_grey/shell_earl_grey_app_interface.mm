// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/web/shell/test/earl_grey/shell_earl_grey_app_interface.h"

#import <EarlGrey/EarlGrey.h>

#import "base/strings/sys_string_conversions.h"
#import "base/test/ios/wait_util.h"
#import "ios/web/public/test/earl_grey/js_test_util.h"
#import "ios/web/public/test/navigation_test_util.h"
#import "ios/web/public/test/web_view_content_test_util.h"
#import "ios/web/public/web_state/web_state.h"
#import "ios/web/shell/test/app/web_shell_test_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using web::shell_test_util::GetCurrentWebState;

@implementation ShellEarlGreyAppInterface

+ (void)loadURL:(NSString*)spec {
  web::test::LoadUrl(GetCurrentWebState(), GURL(base::SysNSStringToUTF8(spec)));
}

+ (BOOL)isCurrentWebStateLoading {
  return GetCurrentWebState()->IsLoading();
}

+ (BOOL)waitForWindowIDInjectedInCurrentWebState {
  return web::WaitUntilWindowIdInjected(GetCurrentWebState());
}

+ (BOOL)currentWebStateContainsText:(NSString*)text {
  return web::test::IsWebViewContainingText(GetCurrentWebState(),
                                            base::SysNSStringToUTF8(text));
}

@end
