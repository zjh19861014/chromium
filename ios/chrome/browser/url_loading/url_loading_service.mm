// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/url_loading/url_loading_service.h"

#include "base/strings/string_number_conversions.h"
#include "ios/chrome/browser/browser_state/chrome_browser_state.h"
#include "ios/chrome/browser/chrome_url_constants.h"
#import "ios/chrome/browser/main/browser.h"
#import "ios/chrome/browser/prerender/prerender_service.h"
#import "ios/chrome/browser/prerender/prerender_service_factory.h"
#import "ios/chrome/browser/tabs/tab.h"
#import "ios/chrome/browser/tabs/tab_model.h"
#import "ios/chrome/browser/ui/chrome_load_params.h"
#import "ios/chrome/browser/ui/commands/open_new_tab_command.h"
#import "ios/chrome/browser/ui/ntp/ntp_util.h"
#import "ios/chrome/browser/url_loading/app_url_loading_service.h"
#import "ios/chrome/browser/url_loading/url_loading_notifier.h"
#import "ios/chrome/browser/url_loading/url_loading_params.h"
#import "ios/chrome/browser/url_loading/url_loading_util.h"
#import "ios/chrome/browser/web/load_timing_tab_helper.h"
#import "ios/chrome/browser/web_state_list/web_state_list.h"
#include "net/base/url_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace {
// Helper method for inducing intentional freezes and crashes, in a separate
// function so it will show up in stack traces.
// If a delay parameter is present, the main thread will be frozen for that
// number of seconds.
// If a crash parameter is "true" (which is the default value), the browser will
// crash after this delay. Any other value will not trigger a crash.
void InduceBrowserCrash(const GURL& url) {
  int delay = 0;
  std::string delay_string;
  if (net::GetValueForKeyInQuery(url, "delay", &delay_string)) {
    base::StringToInt(delay_string, &delay);
  }
  if (delay > 0) {
    sleep(delay);
  }

  bool crash = true;
  std::string crash_string;
  if (net::GetValueForKeyInQuery(url, "crash", &crash_string)) {
    crash = crash_string == "" || crash_string == "true";
  }

  if (crash) {
    // Induce an intentional crash in the browser process.
    CHECK(false);
    // Call another function, so that the above CHECK can't be tail-call
    // optimized. This ensures that this method's name will show up in the stack
    // for easier identification.
    CHECK(true);
  }
}
}

@interface UrlLoadingServiceUrlLoader : NSObject <UrlLoader>
- (instancetype)initWithUrlLoadingService:(UrlLoadingService*)service;
@end

@implementation UrlLoadingServiceUrlLoader {
  UrlLoadingService* service_;
}

- (instancetype)initWithUrlLoadingService:(UrlLoadingService*)service {
  DCHECK(service);
  if (self = [super init]) {
    service_ = service;
  }
  return self;
}

- (void)loadURLWithParams:(const ChromeLoadParams&)chromeParams {
  web::NavigationManager::WebLoadParams params = chromeParams.web_params;
  if (chromeParams.disposition == WindowOpenDisposition::SWITCH_TO_TAB) {
    service_->Load(UrlLoadParams::SwitchToTab(chromeParams.web_params));
  } else {
    service_->Load(UrlLoadParams::InCurrentTab(chromeParams.web_params));
  }
}

- (void)webPageOrderedOpen:(OpenNewTabCommand*)command {
  UrlLoadParams params =
      UrlLoadParams::InNewTab(command.URL, command.virtualURL);
  params.SetInBackground(command.inBackground);
  params.web_params.referrer = command.referrer;
  params.in_incognito = command.inIncognito;
  params.append_to = command.appendTo;
  params.origin_point = command.originPoint;
  params.from_chrome = command.fromChrome;
  params.user_initiated = command.userInitiated;
  params.should_focus_omnibox = command.shouldFocusOmnibox;
  service_->Load(params);
}

@end

UrlLoadingService::UrlLoadingService(UrlLoadingNotifier* notifier)
    : notifier_(notifier) {}

void UrlLoadingService::SetAppService(AppUrlLoadingService* app_service) {
  app_service_ = app_service;
}

void UrlLoadingService::SetDelegate(id<URLLoadingServiceDelegate> delegate) {
  delegate_ = delegate;
}

void UrlLoadingService::SetBrowser(Browser* browser) {
  browser_ = browser;
}

void UrlLoadingService::Load(const UrlLoadParams& params) {
  // First apply any override load strategy.
  switch (params.load_strategy) {
    case UrlLoadStrategy::ALWAYS_NEW_FOREGROUND_TAB: {
      UrlLoadParams fixed_params = params;
      fixed_params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
      Dispatch(fixed_params);
      break;
    }
    default: {
      Dispatch(params);
      break;
    }
  }
}

void UrlLoadingService::Dispatch(const UrlLoadParams& params) {
  // Then dispatch.
  switch (params.disposition) {
    case WindowOpenDisposition::NEW_BACKGROUND_TAB:
    case WindowOpenDisposition::NEW_FOREGROUND_TAB:
      LoadUrlInNewTab(params);
      break;
    case WindowOpenDisposition::CURRENT_TAB:
      LoadUrlInCurrentTab(params);
      break;
    case WindowOpenDisposition::SWITCH_TO_TAB:
      SwitchToTab(params);
      break;
    default:
      DCHECK(false) << "Unhandled url loading disposition.";
      break;
  }
}

void UrlLoadingService::LoadUrlInCurrentTab(const UrlLoadParams& params) {
  web::NavigationManager::WebLoadParams web_params = params.web_params;

  ios::ChromeBrowserState* browser_state = browser_->GetBrowserState();

  notifier_->TabWillLoadUrl(web_params.url, web_params.transition_type);

  // NOTE: This check for the Crash Host URL is here to avoid the URL from
  // ending up in the history causing the app to crash at every subsequent
  // restart.
  if (web_params.url.host() == kChromeUIBrowserCrashHost) {
    InduceBrowserCrash(web_params.url);
    // Under a debugger, the app can continue working even after the CHECK.
    // Adding a return avoids adding the crash url to history.
    notifier_->TabFailedToLoadUrl(web_params.url, web_params.transition_type);
    return;
  }

  // Ask the prerender service to load this URL if it can, and return if it does
  // so.
  PrerenderService* prerenderService =
      PrerenderServiceFactory::GetForBrowserState(browser_state);
  WebStateList* web_state_list = browser_->GetWebStateList();
  id<SessionWindowRestoring> restorer =
      (id<SessionWindowRestoring>)browser_->GetTabModel();
  if (prerenderService && prerenderService->MaybeLoadPrerenderedURL(
                              web_params.url, web_params.transition_type,
                              web_state_list, restorer)) {
    notifier_->TabDidPrerenderUrl(web_params.url, web_params.transition_type);
    return;
  }

  // Some URLs are not allowed while in incognito.  If we are in incognito and
  // load a disallowed URL, instead create a new tab not in the incognito state.
  if (browser_state->IsOffTheRecord() &&
      !IsURLAllowedInIncognito(web_params.url)) {
    notifier_->TabFailedToLoadUrl(web_params.url, web_params.transition_type);
    UrlLoadParams params =
        UrlLoadParams::InNewTab(web_params.url, web_params.virtual_url);
    params.web_params.referrer = web::Referrer();
    params.append_to = kCurrentTab;
    Load(params);
    return;
  }

  web::WebState* current_web_state = web_state_list->GetActiveWebState();
  DCHECK(current_web_state);

  BOOL typedOrGeneratedTransition =
      PageTransitionCoreTypeIs(web_params.transition_type,
                               ui::PAGE_TRANSITION_TYPED) ||
      PageTransitionCoreTypeIs(web_params.transition_type,
                               ui::PAGE_TRANSITION_GENERATED);
  if (typedOrGeneratedTransition) {
    LoadTimingTabHelper::FromWebState(current_web_state)->DidInitiatePageLoad();
  }

  // If this is a reload initiated from the omnibox.
  // TODO(crbug.com/730192): Add DCHECK to verify that whenever urlToLoad is the
  // same as the old url, the transition type is ui::PAGE_TRANSITION_RELOAD.
  if (PageTransitionCoreTypeIs(web_params.transition_type,
                               ui::PAGE_TRANSITION_RELOAD)) {
    current_web_state->GetNavigationManager()->Reload(
        web::ReloadType::NORMAL, true /* check_for_repost */);
    notifier_->TabDidReloadUrl(web_params.url, web_params.transition_type);
    return;
  }

  current_web_state->GetNavigationManager()->LoadURLWithParams(web_params);

  notifier_->TabDidLoadUrl(web_params.url, web_params.transition_type);
}

void UrlLoadingService::SwitchToTab(const UrlLoadParams& params) {
  DCHECK(app_service_);

  web::NavigationManager::WebLoadParams web_params = params.web_params;

  WebStateList* web_state_list = browser_->GetWebStateList();
  NSInteger new_web_state_index =
      web_state_list->GetIndexOfInactiveWebStateWithURL(web_params.url);
  bool old_tab_is_ntp_without_history =
      IsNTPWithoutHistory(web_state_list->GetActiveWebState());

  if (new_web_state_index == WebStateList::kInvalidIndex) {
    // If the tab containing the URL has been closed.
    if (old_tab_is_ntp_without_history) {
      // It is NTP, just load the URL.
      Load(UrlLoadParams::InCurrentTab(web_params));
    } else {
      // Load the URL in foreground.
      ios::ChromeBrowserState* browser_state = browser_->GetBrowserState();
      UrlLoadParams new_tab_params =
          UrlLoadParams::InNewTab(web_params.url, web_params.virtual_url);
      new_tab_params.web_params.referrer = web::Referrer();
      new_tab_params.in_incognito = browser_state->IsOffTheRecord();
      new_tab_params.append_to = kCurrentTab;
      app_service_->LoadUrlInNewTab(new_tab_params);
    }
    return;
  }

  notifier_->WillSwitchToTabWithUrl(web_params.url, new_web_state_index);

  NSInteger old_web_state_index = web_state_list->active_index();
  web_state_list->ActivateWebStateAt(new_web_state_index);

  // Close the tab if it is NTP with no back/forward history to avoid having
  // empty tabs.
  if (old_tab_is_ntp_without_history) {
    web_state_list->CloseWebStateAt(old_web_state_index,
                                    WebStateList::CLOSE_USER_ACTION);
  }

  notifier_->DidSwitchToTabWithUrl(web_params.url, new_web_state_index);
}

void UrlLoadingService::LoadUrlInNewTab(const UrlLoadParams& params) {
  DCHECK(app_service_);
  DCHECK(delegate_);
  DCHECK(browser_);
  ios::ChromeBrowserState* browser_state = browser_->GetBrowserState();

  if (params.in_incognito != browser_state->IsOffTheRecord()) {
    // When sending a load request that switches modes, ensure the tab
    // ends up appended to the end of the model, not just next to what is
    // currently selected in the other mode. This is done with the |append_to|
    // parameter.
    UrlLoadParams app_params = params;
    app_params.append_to = kLastTab;
    app_service_->LoadUrlInNewTab(app_params);
    return;
  }

  // Notify only after checking incognito match, otherwise the delegate will
  // take of changing the mode and try again. Notifying before the checks can
  // lead to be calling it twice, and calling 'did' below once.
  notifier_->NewTabWillLoadUrl(params.web_params.url, params.user_initiated);

  TabModel* tab_model = browser_->GetTabModel();

  Tab* adjacent_tab = nil;
  if (params.append_to == kCurrentTab)
    adjacent_tab = tab_model.currentTab;

  UrlLoadParams saved_params = params;
  auto openTab = ^{
    [tab_model
        insertTabWithLoadParams:saved_params.web_params
                         opener:adjacent_tab
                    openedByDOM:NO
                        atIndex:TabModelConstants::kTabPositionAutomatically
                   inBackground:saved_params.in_background()];

    notifier_->NewTabDidLoadUrl(saved_params.web_params.url,
                                saved_params.user_initiated);
  };

  if (!params.in_background()) {
    openTab();
  } else {
    [delegate_ animateOpenBackgroundTabFromParams:params completion:openTab];
  }
}

id<UrlLoader> UrlLoadingService::GetUrlLoader() {
  if (!url_loader_) {
    url_loader_ =
        [[UrlLoadingServiceUrlLoader alloc] initWithUrlLoadingService:this];
  }
  return url_loader_;
}
