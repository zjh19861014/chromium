// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/bind.h"
#include "base/callback.h"
#include "base/run_loop.h"
#import "base/test/ios/wait_util.h"
#import "ios/web/find_in_page/find_in_page_constants.h"
#import "ios/web/public/test/web_test_with_web_state.h"
#import "ios/web/public/web_state/web_frame.h"
#import "ios/web/public/web_state/web_frame_util.h"
#import "ios/web/public/web_state/web_frames_manager.h"
#include "testing/gtest_mac.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using base::test::ios::kWaitForJSCompletionTimeout;
using base::test::ios::WaitUntilConditionOrTimeout;

namespace {

// Find strings.
const char kFindStringFoo[] = "foo";
const char kFindString12345[] = "12345";

// Pump search timeout in milliseconds.
const double kPumpSearchTimeout = 100.0;
}

namespace web {

// Calls FindInPage Javascript handlers and checks that return values are
// correct.
class FindInPageJsTest : public WebTestWithWebState {
 protected:
  // Returns WebFramesManager instance.
  WebFramesManager* frames_manager() {
    return WebFramesManager::FromWebState(web_state());
  }
  // Returns main frame for |web_state_|.
  WebFrame* main_web_frame() { return GetMainWebFrame(web_state()); }
};

// Tests that FindInPage searches in main frame containing a match and responds
// with 1 match.
TEST_F(FindInPageJsTest, FindText) {
  ASSERT_TRUE(LoadHtml("<span>foo</span>"));
  base::TimeDelta kCallJavascriptFunctionTimeout =
      base::TimeDelta::FromSeconds(kWaitForJSCompletionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return frames_manager()->GetAllWebFrames().size() == 1;
  }));

  __block bool message_received = false;
  std::vector<base::Value> params;
  params.push_back(base::Value(kFindStringFoo));
  params.push_back(base::Value(kPumpSearchTimeout));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageSearch, params, base::BindOnce(^(const base::Value* result) {
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->is_double());
        double count = result->GetDouble();
        ASSERT_EQ(1.0, count);
        message_received = true;
      }),
      kCallJavascriptFunctionTimeout);

  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return message_received;
  }));
}

// Tests that FindInPage searches in main frame for text that exists but is
// hidden and responds with 0 matches.
TEST_F(FindInPageJsTest, FindTextNoResults) {
  ASSERT_TRUE(LoadHtml("<span style='display:none'>foo</span>"));
  base::TimeDelta kCallJavascriptFunctionTimeout =
      base::TimeDelta::FromSeconds(kWaitForJSCompletionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return frames_manager()->GetAllWebFrames().size() == 1;
  }));
  __block bool message_received = false;
  std::vector<base::Value> params;
  params.push_back(base::Value(kFindStringFoo));
  params.push_back(base::Value(kPumpSearchTimeout));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageSearch, params, base::BindOnce(^(const base::Value* result) {
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->is_double());
        double count = result->GetDouble();
        ASSERT_EQ(0.0, count);
        message_received = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return message_received;
  }));
}

// Tests that FindInPage searches in child iframe and asserts that a result was
// found.
TEST_F(FindInPageJsTest, FindIFrameText) {
  ASSERT_TRUE(LoadHtml(
      "<iframe "
      "srcdoc='<html><body><span>foo</span></body></html>'></iframe>"));
  base::TimeDelta kCallJavascriptFunctionTimeout =
      base::TimeDelta::FromSeconds(kWaitForJSCompletionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return frames_manager()->GetAllWebFrames().size() == 2;
  }));
  std::set<WebFrame*> all_frames = frames_manager()->GetAllWebFrames();
  __block bool message_received = false;
  WebFrame* child_frame = nullptr;
  for (auto* frame : all_frames) {
    if (frame->IsMainFrame()) {
      continue;
    }
    child_frame = frame;
  }
  ASSERT_TRUE(child_frame);
  std::vector<base::Value> params;
  params.push_back(base::Value(kFindStringFoo));
  params.push_back(base::Value(kPumpSearchTimeout));
  child_frame->CallJavaScriptFunction(
      kFindInPageSearch, params, base::BindOnce(^(const base::Value* result) {
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->is_double());
        double count = result->GetDouble();
        ASSERT_EQ(1.0, count);
        message_received = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return message_received;
  }));
}

// Tests that FindInPage works when searching for white space.
TEST_F(FindInPageJsTest, FindWhiteSpace) {
  ASSERT_TRUE(LoadHtml("<span> </span>"));
  base::TimeDelta kCallJavascriptFunctionTimeout =
      base::TimeDelta::FromSeconds(kWaitForJSCompletionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return frames_manager()->GetAllWebFrames().size() == 1;
  }));
  __block bool message_received = false;
  std::vector<base::Value> params;
  params.push_back(base::Value(" "));
  params.push_back(base::Value(kPumpSearchTimeout));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageSearch, params, base::BindOnce(^(const base::Value* result) {
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->is_double());
        double count = result->GetDouble();
        ASSERT_EQ(1.0, count);
        message_received = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return message_received;
  }));
}

// Tests that FindInPage works when match results cover multiple HTML Nodes.
TEST_F(FindInPageJsTest, FindAcrossMultipleNodes) {
  ASSERT_TRUE(
      LoadHtml("<p>xx1<span>2</span>3<a>4512345xxx12</a>34<a>5xxx12345xx</p>"));
  base::TimeDelta kCallJavascriptFunctionTimeout =
      base::TimeDelta::FromSeconds(kWaitForJSCompletionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return frames_manager()->GetAllWebFrames().size() == 1;
  }));
  __block bool message_received = false;
  std::vector<base::Value> params;
  params.push_back(base::Value(kFindString12345));
  params.push_back(base::Value(kPumpSearchTimeout));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageSearch, params, base::BindOnce(^(const base::Value* result) {
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->is_double());
        double count = result->GetDouble();
        ASSERT_EQ(4.0, count);
        message_received = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return message_received;
  }));
}

// Tests that a FindInPage match can be highlighted.
TEST_F(FindInPageJsTest, FindHighlightMatch) {
  ASSERT_TRUE(LoadHtml("<span>foo</span>"));
  base::TimeDelta kCallJavascriptFunctionTimeout =
      base::TimeDelta::FromSeconds(kWaitForJSCompletionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return frames_manager()->GetAllWebFrames().size() == 1;
  }));

  __block bool message_received = false;
  std::vector<base::Value> params;
  params.push_back(base::Value(kFindStringFoo));
  params.push_back(base::Value(kPumpSearchTimeout));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageSearch, params, base::BindOnce(^(const base::Value* result) {
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->is_double());
        double count = result->GetDouble();
        ASSERT_EQ(1.0, count);
        message_received = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return message_received;
  }));

  __block bool highlight_done = false;
  std::vector<base::Value> highlight_params;
  highlight_params.push_back(base::Value(0));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageHighlightMatch, highlight_params,
      base::BindOnce(^(const base::Value* result) {
        highlight_done = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return highlight_done;
  }));

  EXPECT_NSEQ(@1,
              ExecuteJavaScript(
                  @"document.getElementsByClassName('find_selected').length"));
}

// Tests that a FindInPage match can be highlighted and that a previous
// highlight is removed when another match is highlighted.
TEST_F(FindInPageJsTest, FindHighlightSeparateMatches) {
  ASSERT_TRUE(LoadHtml("<span>foo foo</span>"));
  base::TimeDelta kCallJavascriptFunctionTimeout =
      base::TimeDelta::FromSeconds(kWaitForJSCompletionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return frames_manager()->GetAllWebFrames().size() == 1;
  }));

  __block bool message_received = false;
  std::vector<base::Value> params;
  params.push_back(base::Value(kFindStringFoo));
  params.push_back(base::Value(kPumpSearchTimeout));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageSearch, params, base::BindOnce(^(const base::Value* result) {
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->is_double());
        double count = result->GetDouble();
        ASSERT_EQ(2.0, count);
        message_received = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return message_received;
  }));

  __block bool highlight_done = false;
  std::vector<base::Value> highlight_params;
  highlight_params.push_back(base::Value(0));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageHighlightMatch, highlight_params,
      base::BindOnce(^(const base::Value* result) {
        highlight_done = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return highlight_done;
  }));

  EXPECT_NSEQ(@1,
              ExecuteJavaScript(
                  @"document.getElementsByClassName('find_selected').length"));

  highlight_done = false;
  std::vector<base::Value> highlight_second_params;
  highlight_second_params.push_back(base::Value(1));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageHighlightMatch, highlight_second_params,
      base::BindOnce(^(const base::Value* result) {
        highlight_done = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return highlight_done;
  }));

  id inner_html = ExecuteJavaScript(@"document.body.innerHTML");
  ASSERT_TRUE([inner_html isKindOfClass:[NSString class]]);
  EXPECT_TRUE([inner_html
      containsString:@"<chrome_find class=\"find_in_page\">foo</chrome_find> "
                     @"<chrome_find class=\"find_in_page "
                     @"find_selected\">foo</chrome_find>"]);
  EXPECT_TRUE(
      [inner_html containsString:@"find_selected{background-color:#ff9632"]);
}

// Tests that FindInPage does not highlight any matches given an invalid index.
TEST_F(FindInPageJsTest, FindHighlightMatchAtInvalidIndex) {
  ASSERT_TRUE(LoadHtml("<span>invalid </span>"));
  base::TimeDelta kCallJavascriptFunctionTimeout =
      base::TimeDelta::FromSeconds(kWaitForJSCompletionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return frames_manager()->GetAllWebFrames().size() == 1;
  }));

  __block bool message_received = false;
  std::vector<base::Value> params;
  params.push_back(base::Value(kFindStringFoo));
  params.push_back(base::Value(kPumpSearchTimeout));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageSearch, params, base::BindOnce(^(const base::Value* result) {
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->is_double());
        double count = result->GetDouble();
        ASSERT_TRUE(count == 0.0);
        message_received = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return message_received;
  }));

  __block bool highlight_done = false;
  std::vector<base::Value> highlight_params;
  highlight_params.push_back(base::Value(0));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageHighlightMatch, highlight_params,
      base::BindOnce(^(const base::Value* result) {
        highlight_done = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return highlight_done;
  }));

  EXPECT_NSEQ(@0,
              ExecuteJavaScript(
                  @"document.getElementsByClassName('find_selected').length"));
}

// Tests that FindInPage works when searching for strings with non-ascii
// characters.
TEST_F(FindInPageJsTest, SearchForNonAscii) {
  ASSERT_TRUE(LoadHtml("<span>école francais</span>"));
  base::TimeDelta kCallJavascriptFunctionTimeout =
      base::TimeDelta::FromSeconds(kWaitForJSCompletionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return frames_manager()->GetAllWebFrames().size() == 1;
  }));
  __block bool message_received = false;
  std::vector<base::Value> params;
  params.push_back(base::Value("école"));
  params.push_back(base::Value(kPumpSearchTimeout));
  main_web_frame()->CallJavaScriptFunction(
      kFindInPageSearch, params, base::BindOnce(^(const base::Value* result) {
        ASSERT_TRUE(result);
        ASSERT_TRUE(result->is_double());
        double count = result->GetDouble();
        ASSERT_EQ(1.0, count);
        message_received = true;
      }),
      kCallJavascriptFunctionTimeout);
  ASSERT_TRUE(WaitUntilConditionOrTimeout(kWaitForJSCompletionTimeout, ^{
    return message_received;
  }));
}

}  // namespace web
