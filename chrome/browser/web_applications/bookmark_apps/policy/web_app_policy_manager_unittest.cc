// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/web_applications/components/policy/web_app_policy_manager.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/run_loop.h"
#include "base/values.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/browser/web_applications/bookmark_apps/test_web_app_provider.h"
#include "chrome/browser/web_applications/components/install_options.h"
#include "chrome/browser/web_applications/components/pending_app_manager.h"
#include "chrome/browser/web_applications/components/policy/web_app_policy_constants.h"
#include "chrome/browser/web_applications/components/test_pending_app_manager.h"
#include "chrome/browser/web_applications/components/web_app_constants.h"
#include "chrome/browser/web_applications/extensions/web_app_extension_ids_map.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "components/crx_file/id_util.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension_builder.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

using sync_preferences::TestingPrefServiceSyncable;

namespace web_app {

namespace {

const char kWindowedUrl[] = "https://windowed.example/";
const char kTabbedUrl[] = "https://tabbed.example/";
const char kNoContainerUrl[] = "https://no-container.example/";

base::Value GetWindowedItem() {
  base::Value item(base::Value::Type::DICTIONARY);
  item.SetKey(kUrlKey, base::Value(kWindowedUrl));
  item.SetKey(kDefaultLaunchContainerKey,
              base::Value(kDefaultLaunchContainerWindowValue));
  return item;
}

InstallOptions GetWindowedInstallOptions() {
  InstallOptions options(GURL(kWindowedUrl), LaunchContainer::kWindow,
                         InstallSource::kExternalPolicy);
  options.add_to_applications_menu = false;
  options.add_to_desktop = false;
  options.add_to_quick_launch_bar = false;
  options.install_placeholder = true;
  return options;
}

base::Value GetTabbedItem() {
  base::Value item(base::Value::Type::DICTIONARY);
  item.SetKey(kUrlKey, base::Value(kTabbedUrl));
  item.SetKey(kDefaultLaunchContainerKey,
              base::Value(kDefaultLaunchContainerTabValue));
  return item;
}

InstallOptions GetTabbedInstallOptions() {
  InstallOptions options(GURL(kTabbedUrl), LaunchContainer::kTab,
                         InstallSource::kExternalPolicy);
  options.add_to_applications_menu = false;
  options.add_to_desktop = false;
  options.add_to_quick_launch_bar = false;
  options.install_placeholder = true;
  return options;
}

base::Value GetNoContainerItem() {
  base::Value item(base::Value::Type::DICTIONARY);
  item.SetKey(kUrlKey, base::Value(kNoContainerUrl));
  return item;
}

InstallOptions GetNoContainerInstallOptions() {
  InstallOptions options(GURL(kNoContainerUrl), LaunchContainer::kTab,
                         InstallSource::kExternalPolicy);
  options.add_to_applications_menu = false;
  options.add_to_desktop = false;
  options.add_to_quick_launch_bar = false;
  options.install_placeholder = true;
  return options;
}

base::Value GetCreateDesktopShorcutDefaultItem() {
  base::Value item(base::Value::Type::DICTIONARY);
  item.SetKey(kUrlKey, base::Value(kNoContainerUrl));
  return item;
}

InstallOptions GetCreateDesktopShorcutDefaultInstallOptions() {
  InstallOptions options(GURL(kNoContainerUrl), LaunchContainer::kTab,
                         InstallSource::kExternalPolicy);
  options.add_to_applications_menu = false;
  options.add_to_desktop = false;
  options.add_to_quick_launch_bar = false;
  options.install_placeholder = true;
  return options;
}

base::Value GetCreateDesktopShorcutFalseItem() {
  base::Value item(base::Value::Type::DICTIONARY);
  item.SetKey(kUrlKey, base::Value(kNoContainerUrl));
  item.SetKey(kCreateDesktopShorcutKey, base::Value(false));
  return item;
}

InstallOptions GetCreateDesktopShorcutFalseInstallOptions() {
  InstallOptions options(GURL(kNoContainerUrl), LaunchContainer::kTab,
                         InstallSource::kExternalPolicy);
  options.add_to_applications_menu = false;
  options.add_to_desktop = false;
  options.add_to_quick_launch_bar = false;
  options.install_placeholder = true;
  return options;
}

base::Value GetCreateDesktopShorcutTrueItem() {
  base::Value item(base::Value::Type::DICTIONARY);
  item.SetKey(kUrlKey, base::Value(kNoContainerUrl));
  item.SetKey(kCreateDesktopShorcutKey, base::Value(true));
  return item;
}

InstallOptions GetCreateDesktopShorcutTrueInstallOptions() {
  InstallOptions options(GURL(kNoContainerUrl), LaunchContainer::kTab,
                         InstallSource::kExternalPolicy);
  options.add_to_applications_menu = true;
  options.add_to_desktop = true;
  options.add_to_quick_launch_bar = false;
  options.install_placeholder = true;
  return options;
}

}  // namespace

class WebAppPolicyManagerTest : public ChromeRenderViewHostTestHarness {
 public:
  WebAppPolicyManagerTest()
      : test_web_app_provider_creator_(
            base::BindOnce(&WebAppPolicyManagerTest::CreateWebAppProvider,
                           base::Unretained(this))) {}

  ~WebAppPolicyManagerTest() override = default;

  std::unique_ptr<KeyedService> CreateWebAppProvider(Profile* profile) {
    auto provider = std::make_unique<TestWebAppProvider>(profile);

    auto test_pending_app_manager = std::make_unique<TestPendingAppManager>();
    test_pending_app_manager_ = test_pending_app_manager.get();
    provider->SetPendingAppManager(std::move(test_pending_app_manager));

    auto web_app_policy_manager = std::make_unique<WebAppPolicyManager>(
        profile, test_pending_app_manager_);
    web_app_policy_manager_ = web_app_policy_manager.get();
    provider->SetWebAppPolicyManager(std::move(web_app_policy_manager));

    return provider;
  }

  std::string GenerateFakeExtensionId(const GURL& url) {
    return crx_file::id_util::GenerateId("fake_app_id_for:" + url.spec());
  }

  void SimulatePreviouslyInstalledApp(
      GURL url,
      InstallSource install_source) {
    std::string id = GenerateFakeExtensionId(url);
    extensions::ExtensionRegistry::Get(profile())->AddEnabled(
        extensions::ExtensionBuilder("Dummy Name").SetID(id).Build());

    ExtensionIdsMap extension_ids_map(profile()->GetPrefs());
    extension_ids_map.Insert(url, id, install_source);

    pending_app_manager()->SimulatePreviouslyInstalledApp(url, install_source);
  }

 protected:
  TestPendingAppManager* pending_app_manager() {
    return test_pending_app_manager_;
  }

  WebAppPolicyManager* policy_manager() { return web_app_policy_manager_; }

 private:
  TestWebAppProviderCreator test_web_app_provider_creator_;
  TestPendingAppManager* test_pending_app_manager_ = nullptr;
  WebAppPolicyManager* web_app_policy_manager_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(WebAppPolicyManagerTest);
};

TEST_F(WebAppPolicyManagerTest, NoForceInstalledAppsPrefValue) {
  policy_manager()->Start();

  base::RunLoop().RunUntilIdle();

  const auto& install_requests = pending_app_manager()->install_requests();
  EXPECT_TRUE(install_requests.empty());
}

TEST_F(WebAppPolicyManagerTest, NoForceInstalledApps) {
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList,
                             base::Value(base::Value::Type::LIST));

  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  const auto& install_requests = pending_app_manager()->install_requests();
  EXPECT_TRUE(install_requests.empty());
}

TEST_F(WebAppPolicyManagerTest, TwoForceInstalledApps) {
  // Add two sites, one that opens in a window and one that opens in a tab.
  base::Value list(base::Value::Type::LIST);
  list.GetList().push_back(GetWindowedItem());
  list.GetList().push_back(GetTabbedItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList, std::move(list));

  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  const auto& install_requests = pending_app_manager()->install_requests();

  std::vector<InstallOptions> expected_install_options_list;
  expected_install_options_list.push_back(GetWindowedInstallOptions());
  expected_install_options_list.push_back(GetTabbedInstallOptions());

  EXPECT_EQ(install_requests, expected_install_options_list);
}

TEST_F(WebAppPolicyManagerTest, ForceInstallAppWithNoDefaultLaunchContainer) {
  base::Value list(base::Value::Type::LIST);
  list.GetList().push_back(GetNoContainerItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList, std::move(list));

  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  const auto& install_requests = pending_app_manager()->install_requests();

  std::vector<InstallOptions> expected_install_options_list;
  expected_install_options_list.push_back(GetNoContainerInstallOptions());

  EXPECT_EQ(install_requests, expected_install_options_list);
}

TEST_F(WebAppPolicyManagerTest,
       ForceInstallAppWithDefaultCreateDesktopShorcut) {
  base::Value list(base::Value::Type::LIST);
  list.GetList().push_back(GetCreateDesktopShorcutDefaultItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList, std::move(list));

  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  const auto& install_requests = pending_app_manager()->install_requests();

  std::vector<InstallOptions> expected_install_options_list;
  expected_install_options_list.push_back(
      GetCreateDesktopShorcutDefaultInstallOptions());

  EXPECT_EQ(install_requests, expected_install_options_list);
}

TEST_F(WebAppPolicyManagerTest, ForceInstallAppWithCreateDesktopShortcut) {
  base::Value list(base::Value::Type::LIST);
  list.GetList().push_back(GetCreateDesktopShorcutFalseItem());
  list.GetList().push_back(GetCreateDesktopShorcutTrueItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList, std::move(list));

  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  const auto& install_requests = pending_app_manager()->install_requests();

  std::vector<InstallOptions> expected_install_options_list;
  expected_install_options_list.push_back(
      GetCreateDesktopShorcutFalseInstallOptions());
  expected_install_options_list.push_back(
      GetCreateDesktopShorcutTrueInstallOptions());

  EXPECT_EQ(install_requests, expected_install_options_list);
}

TEST_F(WebAppPolicyManagerTest, DynamicRefresh) {
  base::Value first_list(base::Value::Type::LIST);
  first_list.GetList().push_back(GetWindowedItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList,
                             std::move(first_list));

  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  const auto& install_requests = pending_app_manager()->install_requests();

  std::vector<InstallOptions> expected_install_options_list;
  expected_install_options_list.push_back(GetWindowedInstallOptions());

  EXPECT_EQ(install_requests, expected_install_options_list);

  base::Value second_list(base::Value::Type::LIST);
  second_list.GetList().push_back(GetTabbedItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList,
                             std::move(second_list));

  base::RunLoop().RunUntilIdle();

  expected_install_options_list.push_back(GetTabbedInstallOptions());

  EXPECT_EQ(install_requests, expected_install_options_list);
}

TEST_F(WebAppPolicyManagerTest, UninstallAppInstalledInPreviousSession) {
  // Simulate two policy apps and a regular app that were installed in the
  // previous session.
  SimulatePreviouslyInstalledApp(GURL(kWindowedUrl),
                                 InstallSource::kExternalPolicy);
  SimulatePreviouslyInstalledApp(GURL(kTabbedUrl),
                                 InstallSource::kExternalPolicy);
  SimulatePreviouslyInstalledApp(GURL(kNoContainerUrl),
                                 InstallSource::kInternal);

  // Push a policy with only one of the apps.
  base::Value first_list(base::Value::Type::LIST);
  first_list.GetList().push_back(GetWindowedItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList,
                             std::move(first_list));

  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  // We should only try to install the app in the policy.
  std::vector<InstallOptions> expected_install_options_list;
  expected_install_options_list.push_back(GetWindowedInstallOptions());
  EXPECT_EQ(pending_app_manager()->install_requests(),
            expected_install_options_list);

  // We should try to uninstall the app that is no longer in the policy.
  EXPECT_EQ(std::vector<GURL>({GURL(kTabbedUrl)}),
            pending_app_manager()->uninstall_requests());
}

// Tests that we correctly uninstall an app that we installed in the same
// session.
TEST_F(WebAppPolicyManagerTest, UninstallAppInstalledInCurrentSession) {
  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  // Add two sites, one that opens in a window and one that opens in a tab.
  base::Value first_list(base::Value::Type::LIST);
  first_list.GetList().push_back(GetWindowedItem());
  first_list.GetList().push_back(GetTabbedItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList,
                             std::move(first_list));
  base::RunLoop().RunUntilIdle();

  const auto& install_requests = pending_app_manager()->install_requests();

  std::vector<InstallOptions> expected_install_options_list;
  expected_install_options_list.push_back(GetWindowedInstallOptions());
  expected_install_options_list.push_back(GetTabbedInstallOptions());

  EXPECT_EQ(install_requests, expected_install_options_list);

  // Push a new policy without the tabbed site.
  base::Value second_list(base::Value::Type::LIST);
  second_list.GetList().push_back(GetWindowedItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList,
                             std::move(second_list));
  base::RunLoop().RunUntilIdle();

  // We'll try to install the app again but PendingAppManager will handle
  // not re-installing the app.
  expected_install_options_list.push_back(GetWindowedInstallOptions());

  EXPECT_EQ(install_requests, expected_install_options_list);

  EXPECT_EQ(std::vector<GURL>({GURL(kTabbedUrl)}),
            pending_app_manager()->uninstall_requests());
}

// Tests that we correctly reinstall a placeholder app.
TEST_F(WebAppPolicyManagerTest, ReinstallPlaceholderApp) {
  base::Value list(base::Value::Type::LIST);
  list.GetList().push_back(GetWindowedItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList, std::move(list));

  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  std::vector<InstallOptions> expected_options_list;
  expected_options_list.push_back(GetWindowedInstallOptions());

  const auto& install_options_list = pending_app_manager()->install_requests();
  EXPECT_EQ(expected_options_list, install_options_list);

  policy_manager()->ReinstallPlaceholderAppIfNecessary(GURL(kWindowedUrl));
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(expected_options_list, install_options_list);

  const auto& reinstall_options_list =
      pending_app_manager()->reinstall_requests();
  EXPECT_EQ(expected_options_list, reinstall_options_list);
}

TEST_F(WebAppPolicyManagerTest, TryToInexistentPlaceholderApp) {
  base::Value list(base::Value::Type::LIST);
  list.GetList().push_back(GetWindowedItem());
  profile()->GetPrefs()->Set(prefs::kWebAppInstallForceList, std::move(list));

  policy_manager()->Start();
  base::RunLoop().RunUntilIdle();

  std::vector<InstallOptions> expected_options_list;
  expected_options_list.push_back(GetWindowedInstallOptions());

  const auto& install_options_list = pending_app_manager()->install_requests();
  EXPECT_EQ(expected_options_list, install_options_list);
  EXPECT_TRUE(pending_app_manager()->reinstall_requests().empty());

  // Try to reinstall for app not installed by policy.
  policy_manager()->ReinstallPlaceholderAppIfNecessary(GURL(kTabbedUrl));
  base::RunLoop().RunUntilIdle();

  EXPECT_EQ(expected_options_list, install_options_list);
  EXPECT_TRUE(pending_app_manager()->reinstall_requests().empty());
}

}  // namespace web_app
