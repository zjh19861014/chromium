// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/web/shell/test/earl_grey/shell_actions.h"

#import <EarlGrey/EarlGrey.h>

#import "ios/web/public/test/earl_grey/web_view_actions.h"
#include "ios/web/public/test/element_selector.h"
#import "ios/web/shell/test/app/web_shell_test_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace web {

id<GREYAction> LongPressElementForContextMenu(ElementSelector* selector) {
  return WebViewLongPressElementForContextMenu(
      shell_test_util::GetCurrentWebState(), std::move(selector), true);
}

}  // namespace web
