// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/previews/previews_lite_page_decider.h"

#include <memory>

#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/scoped_task_environment.h"
#include "base/test/simple_test_tick_clock.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_compression_stats.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_service.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_settings.h"
#include "components/data_reduction_proxy/core/browser/data_reduction_proxy_test_utils.h"
#include "components/data_reduction_proxy/core/common/data_reduction_proxy_headers.h"
#include "components/data_reduction_proxy/core/common/data_reduction_proxy_pref_names.h"
#include "components/previews/core/previews_switches.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/web_contents_tester.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {
const char kTestUrl[] = "http://www.test.com/";
}

class PreviewsLitePageDeciderTest : public testing::Test {
 protected:
  PreviewsLitePageDeciderTest()
      : scoped_task_environment_(
            base::test::ScopedTaskEnvironment::MainThreadType::UI) {}

 private:
  base::test::ScopedTaskEnvironment scoped_task_environment_;
};

TEST_F(PreviewsLitePageDeciderTest, TestHostBypassBlacklist) {
  const int kBlacklistDurationDays = 30;
  const std::string kHost = "google.com";
  const std::string kOtherHost = "chromium.org";
  const base::TimeDelta kYesterday = base::TimeDelta::FromDays(-1);
  const base::TimeDelta kOneDay = base::TimeDelta::FromDays(1);

  std::unique_ptr<PreviewsLitePageDecider> decider =
      std::make_unique<PreviewsLitePageDecider>(nullptr);

  // Simple happy case.
  decider->BlacklistBypassedHost(kHost, kOneDay);
  EXPECT_TRUE(decider->HostBlacklistedFromBypass(kHost));
  decider->ClearStateForTesting();

  // Old entries are deleted.
  decider->BlacklistBypassedHost(kHost, kYesterday);
  EXPECT_FALSE(decider->HostBlacklistedFromBypass(kHost));
  decider->ClearStateForTesting();

  // Oldest entry is thrown out.
  decider->BlacklistBypassedHost(kHost, kOneDay);
  EXPECT_TRUE(decider->HostBlacklistedFromBypass(kHost));
  for (int i = 1; i <= kBlacklistDurationDays; i++) {
    decider->BlacklistBypassedHost(kHost + base::NumberToString(i),
                                   kOneDay + base::TimeDelta::FromSeconds(i));
  }
  EXPECT_FALSE(decider->HostBlacklistedFromBypass(kHost));
  decider->ClearStateForTesting();

  // Oldest entry is not thrown out if there was a stale entry to remove.
  decider->BlacklistBypassedHost(kHost, kOneDay);
  EXPECT_TRUE(decider->HostBlacklistedFromBypass(kHost));
  for (int i = 1; i <= kBlacklistDurationDays - 1; i++) {
    decider->BlacklistBypassedHost(kHost + base::NumberToString(i),
                                   kOneDay + base::TimeDelta::FromSeconds(i));
  }
  decider->BlacklistBypassedHost(kOtherHost, kYesterday);
  EXPECT_TRUE(decider->HostBlacklistedFromBypass(kHost));
  decider->ClearStateForTesting();
}

TEST_F(PreviewsLitePageDeciderTest, TestClearHostBypassBlacklist) {
  const std::string kHost = "1.chromium.org";

  std::unique_ptr<PreviewsLitePageDecider> decider =
      std::make_unique<PreviewsLitePageDecider>(nullptr);

  decider->BlacklistBypassedHost(kHost, base::TimeDelta::FromMinutes(1));
  EXPECT_TRUE(decider->HostBlacklistedFromBypass(kHost));

  decider->ClearBlacklist();
  EXPECT_FALSE(decider->HostBlacklistedFromBypass(kHost));
}

TEST_F(PreviewsLitePageDeciderTest, TestServerUnavailable) {
  struct TestCase {
    base::TimeDelta set_available_after;
    base::TimeDelta check_available_after;
    bool want_is_unavailable;
  };
  const TestCase kTestCases[]{
      {
          base::TimeDelta::FromMinutes(1), base::TimeDelta::FromMinutes(2),
          false,
      },
      {
          base::TimeDelta::FromMinutes(2), base::TimeDelta::FromMinutes(1),
          true,
      },
  };

  for (const TestCase& test_case : kTestCases) {
    std::unique_ptr<PreviewsLitePageDecider> decider =
        std::make_unique<PreviewsLitePageDecider>(nullptr);
    std::unique_ptr<base::SimpleTestTickClock> clock =
        std::make_unique<base::SimpleTestTickClock>();
    decider->SetClockForTesting(clock.get());

    decider->SetServerUnavailableFor(test_case.set_available_after);
    EXPECT_TRUE(decider->IsServerUnavailable());

    clock->Advance(test_case.check_available_after);
    EXPECT_EQ(decider->IsServerUnavailable(), test_case.want_is_unavailable);
  }
}

TEST_F(PreviewsLitePageDeciderTest, TestSingleBypass) {
  const std::string kUrl = "http://test.com";
  struct TestCase {
    std::string add_url;
    base::TimeDelta clock_advance;
    std::string check_url;
    bool want_check;
  };
  const TestCase kTestCases[]{
      {
          kUrl, base::TimeDelta::FromMinutes(1), kUrl, true,
      },
      {
          kUrl, base::TimeDelta::FromMinutes(6), kUrl, false,
      },
      {
          "bad", base::TimeDelta::FromMinutes(1), kUrl, false,
      },
      {
          "bad", base::TimeDelta::FromMinutes(6), kUrl, false,
      },
      {
          kUrl, base::TimeDelta::FromMinutes(1), "bad", false,
      },
      {
          kUrl, base::TimeDelta::FromMinutes(6), "bad", false,
      },
  };

  for (const TestCase& test_case : kTestCases) {
    std::unique_ptr<PreviewsLitePageDecider> decider =
        std::make_unique<PreviewsLitePageDecider>(nullptr);
    std::unique_ptr<base::SimpleTestTickClock> clock =
        std::make_unique<base::SimpleTestTickClock>();
    decider->SetClockForTesting(clock.get());

    decider->AddSingleBypass(test_case.add_url);
    clock->Advance(test_case.clock_advance);
    EXPECT_EQ(decider->CheckSingleBypass(test_case.check_url),
              test_case.want_check);
  }
}

class PreviewsLitePageDeciderPrefTest : public ChromeRenderViewHostTestHarness {
 protected:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();

    drp_test_context_ =
        data_reduction_proxy::DataReductionProxyTestContext::Builder()
            .WithMockConfig()
            .SkipSettingsInitialization()
            .Build();
  }

  void TearDown() override {
    drp_test_context_->DestroySettings();
    ChromeRenderViewHostTestHarness::TearDown();
  }

  PreviewsLitePageDecider* GetDeciderWithDRPEnabled(bool enabled) {
    drp_test_context_->SetDataReductionProxyEnabled(enabled);

    decider_ = std::make_unique<PreviewsLitePageDecider>(
        web_contents()->GetBrowserContext());
    decider_->SetDRPSettingsForTesting(drp_test_context_->settings());

    drp_test_context_->InitSettings();

    return decider_.get();
  }

 private:
  std::unique_ptr<data_reduction_proxy::DataReductionProxyTestContext>
      drp_test_context_;
  std::unique_ptr<PreviewsLitePageDecider> decider_;
};

TEST_F(PreviewsLitePageDeciderPrefTest, TestDRPDisabled) {
  PreviewsLitePageDecider* decider = GetDeciderWithDRPEnabled(false);
  EXPECT_FALSE(decider->NeedsToNotifyUser());

  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL(kTestUrl));

  // Should still be false after a navigation
  EXPECT_FALSE(decider->NeedsToNotifyUser());
}

TEST_F(PreviewsLitePageDeciderPrefTest, TestDRPEnabled) {
  PreviewsLitePageDecider* decider = GetDeciderWithDRPEnabled(true);
  EXPECT_TRUE(decider->NeedsToNotifyUser());

  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL(kTestUrl));

  // Should still be true after a navigation
  EXPECT_TRUE(decider->NeedsToNotifyUser());
}

TEST_F(PreviewsLitePageDeciderPrefTest, TestDRPEnabledCmdLineIgnored) {
  PreviewsLitePageDecider* decider = GetDeciderWithDRPEnabled(true);
  base::CommandLine::ForCurrentProcess()->AppendSwitch(
      previews::switches::kDoNotRequireLitePageRedirectInfoBar);
  EXPECT_FALSE(decider->NeedsToNotifyUser());

  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL(kTestUrl));

  // Should still be false after a navigation.
  EXPECT_FALSE(decider->NeedsToNotifyUser());
}

TEST_F(PreviewsLitePageDeciderPrefTest, TestDRPEnabledThenNotify) {
  PreviewsLitePageDecider* decider = GetDeciderWithDRPEnabled(true);
  EXPECT_TRUE(decider->NeedsToNotifyUser());

  // Simulate the callback being run.
  decider->SetUserHasSeenUINotification();

  content::WebContentsTester::For(web_contents())
      ->NavigateAndCommit(GURL(kTestUrl));

  EXPECT_FALSE(decider->NeedsToNotifyUser());
}
