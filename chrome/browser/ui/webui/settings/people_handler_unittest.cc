// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/settings/people_handler.h"

#include <memory>
#include <string>
#include <vector>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/json/json_writer.h"
#include "base/macros.h"
#include "base/stl_util.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/defaults.h"
#include "chrome/browser/signin/identity_test_environment_profile_adaptor.h"
#include "chrome/browser/signin/scoped_account_consistency.h"
#include "chrome/browser/signin/signin_error_controller_factory.h"
#include "chrome/browser/sync/profile_sync_service_factory.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/webui/signin/login_ui_service.h"
#include "chrome/browser/ui/webui/signin/login_ui_service_factory.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/url_constants.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/scoped_testing_local_state.h"
#include "chrome/test/base/test_chrome_web_ui_controller_factory.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_service.h"
#include "components/sync/base/passphrase_enums.h"
#include "components/sync/driver/mock_sync_service.h"
#include "components/sync/driver/sync_user_settings_impl.h"
#include "components/sync/driver/sync_user_settings_mock.h"
#include "components/unified_consent/scoped_unified_consent.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "content/public/test/test_web_ui.h"
#include "content/public/test/web_contents_tester.h"
#include "services/identity/public/cpp/accounts_mutator.h"
#include "services/identity/public/cpp/identity_manager.h"
#include "services/identity/public/cpp/identity_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::_;
using ::testing::ByMove;
using ::testing::Const;
using ::testing::Invoke;
using ::testing::Mock;
using ::testing::Return;
using ::testing::Values;

namespace {

MATCHER_P(ModelTypeSetMatches, value, "") {
  return arg == value;
}

const char kTestUser[] = "chrome.p13n.test@gmail.com";
const char kTestCallbackId[] = "test-callback-id";

// Returns a ModelTypeSet with all user selectable types set.
syncer::ModelTypeSet GetAllTypes() {
  return syncer::UserSelectableTypes();
}

enum SyncAllDataConfig {
  SYNC_ALL_DATA,
  CHOOSE_WHAT_TO_SYNC
};

enum EncryptAllConfig {
  ENCRYPT_ALL_DATA,
  ENCRYPT_PASSWORDS
};

// Create a json-format string with the key/value pairs appropriate for a call
// to HandleSetEncryption(). If |extra_values| is non-null, then the values from
// the passed dictionary are added to the json.
std::string GetConfiguration(const base::DictionaryValue* extra_values,
                             SyncAllDataConfig sync_all,
                             syncer::ModelTypeSet types,
                             const std::string& passphrase,
                             EncryptAllConfig encrypt_all) {
  base::DictionaryValue result;
  if (extra_values)
    result.MergeDictionary(extra_values);
  result.SetBoolean("syncAllDataTypes", sync_all == SYNC_ALL_DATA);
  result.SetBoolean("encryptAllData", encrypt_all == ENCRYPT_ALL_DATA);
  if (!passphrase.empty())
    result.SetString("passphrase", passphrase);
  // Add all of our data types.
  result.SetBoolean("appsSynced", types.Has(syncer::APPS));
  result.SetBoolean("autofillSynced", types.Has(syncer::AUTOFILL));
  result.SetBoolean("bookmarksSynced", types.Has(syncer::BOOKMARKS));
  result.SetBoolean("extensionsSynced", types.Has(syncer::EXTENSIONS));
  result.SetBoolean("passwordsSynced", types.Has(syncer::PASSWORDS));
  result.SetBoolean("preferencesSynced", types.Has(syncer::PREFERENCES));
  result.SetBoolean("tabsSynced", types.Has(syncer::PROXY_TABS));
  result.SetBoolean("themesSynced", types.Has(syncer::THEMES));
  result.SetBoolean("typedUrlsSynced", types.Has(syncer::TYPED_URLS));
  result.SetBoolean("paymentsIntegrationEnabled", false);
  std::string args;
  base::JSONWriter::Write(result, &args);
  return args;
}

// Checks whether the passed |dictionary| contains a |key| with the given
// |expected_value|. If |omit_if_false| is true, then the value should only
// be present if |expected_value| is true.
void CheckBool(const base::DictionaryValue* dictionary,
               const std::string& key,
               bool expected_value,
               bool omit_if_false) {
  if (omit_if_false && !expected_value) {
    EXPECT_FALSE(dictionary->HasKey(key)) <<
        "Did not expect to find value for " << key;
  } else {
    bool actual_value;
    EXPECT_TRUE(dictionary->GetBoolean(key, &actual_value)) <<
        "No value found for " << key;
    EXPECT_EQ(expected_value, actual_value) <<
        "Mismatch found for " << key;
  }
}

void CheckBool(const base::DictionaryValue* dictionary,
               const std::string& key,
               bool expected_value) {
  return CheckBool(dictionary, key, expected_value, false);
}

// Checks to make sure that the values stored in |dictionary| match the values
// expected by the showSyncSetupPage() JS function for a given set of data
// types.
void CheckConfigDataTypeArguments(const base::DictionaryValue* dictionary,
                                  SyncAllDataConfig config,
                                  syncer::ModelTypeSet types) {
  CheckBool(dictionary, "syncAllDataTypes", config == SYNC_ALL_DATA);
  CheckBool(dictionary, "appsSynced", types.Has(syncer::APPS));
  CheckBool(dictionary, "autofillSynced", types.Has(syncer::AUTOFILL));
  CheckBool(dictionary, "bookmarksSynced", types.Has(syncer::BOOKMARKS));
  CheckBool(dictionary, "extensionsSynced", types.Has(syncer::EXTENSIONS));
  CheckBool(dictionary, "passwordsSynced", types.Has(syncer::PASSWORDS));
  CheckBool(dictionary, "preferencesSynced", types.Has(syncer::PREFERENCES));
  CheckBool(dictionary, "tabsSynced", types.Has(syncer::PROXY_TABS));
  CheckBool(dictionary, "themesSynced", types.Has(syncer::THEMES));
  CheckBool(dictionary, "typedUrlsSynced", types.Has(syncer::TYPED_URLS));
}

std::unique_ptr<KeyedService> BuildMockSyncService(
    content::BrowserContext* context) {
  return std::make_unique<testing::NiceMock<syncer::MockSyncService>>();
}

}  // namespace

namespace settings {

class TestingPeopleHandler : public PeopleHandler {
 public:
  TestingPeopleHandler(content::WebUI* web_ui, Profile* profile)
      : PeopleHandler(profile) {
    set_web_ui(web_ui);
  }

  using PeopleHandler::is_configuring_sync;

 private:
#if !defined(OS_CHROMEOS)
  void DisplayGaiaLoginInNewTabOrWindow(
      signin_metrics::AccessPoint access_point) override {}
#endif

  DISALLOW_COPY_AND_ASSIGN(TestingPeopleHandler);
};

class TestWebUIProvider
    : public TestChromeWebUIControllerFactory::WebUIProvider {
 public:
  std::unique_ptr<content::WebUIController> NewWebUI(content::WebUI* web_ui,
                                                     const GURL& url) override {
    return std::make_unique<content::WebUIController>(web_ui);
  }
};

class PeopleHandlerTest : public ChromeRenderViewHostTestHarness {
 public:
  PeopleHandlerTest() = default;

  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();

    // Sign in the user.
    identity_test_env_adaptor_ =
        std::make_unique<IdentityTestEnvironmentProfileAdaptor>(profile());

    std::string username = GetTestUser();
    if (!username.empty())
      identity_test_env()->SetPrimaryAccount(username);

    mock_sync_service_ = static_cast<syncer::MockSyncService*>(
        ProfileSyncServiceFactory::GetInstance()->SetTestingFactoryAndUse(
            profile(), base::BindRepeating(&BuildMockSyncService)));

    ON_CALL(*mock_sync_service_, IsAuthenticatedAccountPrimary())
        .WillByDefault(Return(true));

    ON_CALL(*mock_sync_service_->GetMockUserSettings(), GetPassphraseType())
        .WillByDefault(Return(syncer::PassphraseType::IMPLICIT_PASSPHRASE));
    ON_CALL(*mock_sync_service_->GetMockUserSettings(),
            GetExplicitPassphraseTime())
        .WillByDefault(Return(base::Time()));
    ON_CALL(*mock_sync_service_, GetRegisteredDataTypes())
        .WillByDefault(Return(syncer::ModelTypeSet()));
    ON_CALL(*mock_sync_service_, GetSetupInProgressHandle())
        .WillByDefault(
            Return(ByMove(std::make_unique<syncer::SyncSetupInProgressHandle>(
                base::BindRepeating(
                    &PeopleHandlerTest::OnSetupInProgressHandleDestroyed,
                    base::Unretained(this))))));

    handler_.reset(new TestingPeopleHandler(&web_ui_, profile()));
    handler_->AllowJavascript();
  }

  void TearDown() override {
    handler_->set_web_ui(nullptr);
    handler_->DisallowJavascript();
    handler_->sync_startup_tracker_.reset();
    identity_test_env_adaptor_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
  }

  content::BrowserContext* CreateBrowserContext() override {
    // Setup the profile.
    std::unique_ptr<TestingProfile> profile =
        IdentityTestEnvironmentProfileAdaptor::
            CreateProfileForIdentityTestEnvironment();
    return profile.release();
  }

  // Setup the expectations for calls made when displaying the config page.
  void SetDefaultExpectationsForConfigPage() {
    ON_CALL(*mock_sync_service_, GetDisableReasons())
        .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
    ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
        .WillByDefault(Return(true));
    ON_CALL(*mock_sync_service_, GetRegisteredDataTypes())
        .WillByDefault(Return(GetAllTypes()));
    ON_CALL(*mock_sync_service_->GetMockUserSettings(),
            IsSyncEverythingEnabled())
        .WillByDefault(Return(true));
    ON_CALL(*mock_sync_service_->GetMockUserSettings(), GetChosenDataTypes())
        .WillByDefault(Return(GetAllTypes()));
    ON_CALL(*mock_sync_service_, GetPreferredDataTypes())
        .WillByDefault(
            Return(syncer::SyncUserSettingsImpl::ResolvePrefGroupsForTesting(
                GetAllTypes())));
    ON_CALL(*mock_sync_service_, GetActiveDataTypes())
        .WillByDefault(Return(GetAllTypes()));
    ON_CALL(*mock_sync_service_->GetMockUserSettings(),
            IsEncryptEverythingAllowed())
        .WillByDefault(Return(true));
    ON_CALL(*mock_sync_service_->GetMockUserSettings(),
            IsEncryptEverythingEnabled())
        .WillByDefault(Return(false));
  }

  void SetupInitializedSyncService() {
    // An initialized SyncService will have already completed sync setup and
    // will have an initialized sync engine.
    ON_CALL(*mock_sync_service_, GetTransportState())
        .WillByDefault(Return(syncer::SyncService::TransportState::ACTIVE));
  }

  void ExpectPageStatusResponse(const std::string& expected_status) {
    auto& data = *web_ui_.call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());
    std::string callback_id;
    ASSERT_TRUE(data.arg1()->GetAsString(&callback_id));
    EXPECT_EQ(kTestCallbackId, callback_id);
    bool success = false;
    ASSERT_TRUE(data.arg2()->GetAsBoolean(&success));
    EXPECT_TRUE(success);
    std::string status;
    ASSERT_TRUE(data.arg3()->GetAsString(&status));
    EXPECT_EQ(expected_status, status);
  }

  void ExpectPageStatusChanged(const std::string& expected_status) {
    auto& data = *web_ui_.call_data().back();
    EXPECT_EQ("cr.webUIListenerCallback", data.function_name());
    std::string event;
    ASSERT_TRUE(data.arg1()->GetAsString(&event));
    EXPECT_EQ("page-status-changed", event);
    std::string status;
    ASSERT_TRUE(data.arg2()->GetAsString(&status));
    EXPECT_EQ(expected_status, status);
  }

  void ExpectSpinnerAndClose() {
    ExpectPageStatusChanged(PeopleHandler::kSpinnerPageStatus);

    // Cancelling the spinner dialog will cause CloseSyncSetup().
    handler_->CloseSyncSetup();
    EXPECT_EQ(
        NULL,
        LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());
  }

  const base::DictionaryValue* ExpectSyncPrefsChanged() {
    const content::TestWebUI::CallData& data1 = *web_ui_.call_data().back();
    EXPECT_EQ("cr.webUIListenerCallback", data1.function_name());

    std::string event;
    EXPECT_TRUE(data1.arg1()->GetAsString(&event));
    EXPECT_EQ(event, "sync-prefs-changed");

    const base::DictionaryValue* dictionary = nullptr;
    EXPECT_TRUE(data1.arg2()->GetAsDictionary(&dictionary));
    return dictionary;
  }

  // It's difficult to notify sync listeners when using a MockSyncService
  // so this helper routine dispatches an OnStateChanged() notification to the
  // SyncStartupTracker.
  void NotifySyncListeners() {
    if (handler_->sync_startup_tracker_)
      handler_->sync_startup_tracker_->OnStateChanged(mock_sync_service_);
  }

  void NotifySyncStateChanged() {
    handler_->OnStateChanged(mock_sync_service_);
  }

  virtual std::string GetTestUser() {
    return std::string(kTestUser);
  }

  identity::IdentityTestEnvironment* identity_test_env() {
    return identity_test_env_adaptor_->identity_test_env();
  }

  MOCK_METHOD0(OnSetupInProgressHandleDestroyed, void());

  syncer::MockSyncService* mock_sync_service_;
  std::unique_ptr<IdentityTestEnvironmentProfileAdaptor>
      identity_test_env_adaptor_;
  content::TestWebUI web_ui_;
  TestWebUIProvider test_provider_;
  std::unique_ptr<TestChromeWebUIControllerFactory> test_factory_;
  std::unique_ptr<TestingPeopleHandler> handler_;
};

class PeopleHandlerFirstSigninTest : public PeopleHandlerTest {
  std::string GetTestUser() override { return std::string(); }
};

#if !defined(OS_CHROMEOS)
TEST_F(PeopleHandlerFirstSigninTest, DisplayBasicLogin) {
  // Test that the HandleStartSignin call enables JavaScript.
  handler_->DisallowJavascript();

  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NOT_SIGNED_IN));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  // Ensure that the user is not signed in before calling |HandleStartSignin()|.
  identity_test_env()->ClearPrimaryAccount();
  base::ListValue list_args;
  handler_->HandleStartSignin(&list_args);

  // Sync setup hands off control to the gaia login tab.
  EXPECT_EQ(
      NULL,
      LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());

  ASSERT_FALSE(handler_->is_configuring_sync());

  handler_->CloseSyncSetup();
  EXPECT_EQ(
      NULL,
      LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());
}

TEST_F(PeopleHandlerTest, ShowSyncSetupWhenNotSignedIn) {
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NOT_SIGNED_IN));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  handler_->HandleShowSetupUI(nullptr);

  ExpectPageStatusChanged(PeopleHandler::kDonePageStatus);

  ASSERT_FALSE(handler_->is_configuring_sync());
  EXPECT_EQ(
      NULL,
      LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());
}
#endif  // !defined(OS_CHROMEOS)

// Verifies that the sync setup is terminated correctly when the
// sync is disabled.
TEST_F(PeopleHandlerTest, HandleSetupUIWhenSyncDisabled) {
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(
          Return(syncer::SyncService::DISABLE_REASON_ENTERPRISE_POLICY));
  handler_->HandleShowSetupUI(nullptr);

  // Sync setup is closed when sync is disabled.
  EXPECT_EQ(
      NULL,
      LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());
  ASSERT_FALSE(handler_->is_configuring_sync());
}

// Verifies that the handler correctly handles a cancellation when
// it is displaying the spinner to the user.
TEST_F(PeopleHandlerTest, DisplayConfigureWithEngineDisabledAndCancel) {
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(Return(syncer::SyncService::TransportState::INITIALIZING));
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetSyncRequested(true));

  // We're simulating a user setting up sync, which would cause the engine to
  // kick off initialization, but not download user data types. The sync
  // engine will try to download control data types (e.g encryption info), but
  // that won't finish for this test as we're simulating cancelling while the
  // spinner is showing.
  handler_->HandleShowSetupUI(nullptr);

  EXPECT_EQ(
      handler_.get(),
      LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());

  ExpectSpinnerAndClose();
}

// Verifies that the handler correctly transitions from showing the spinner
// to showing a configuration page when sync setup completes successfully.
TEST_F(PeopleHandlerTest,
       DisplayConfigureWithEngineDisabledAndSyncStartupCompleted) {
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
      .WillByDefault(Return(true));
  // Sync engine is stopped initially, and will start up.
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(
          Return(syncer::SyncService::TransportState::START_DEFERRED));
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetSyncRequested(true));
  SetDefaultExpectationsForConfigPage();

  handler_->HandleShowSetupUI(nullptr);

  EXPECT_EQ(1U, web_ui_.call_data().size());
  ExpectPageStatusChanged(PeopleHandler::kSpinnerPageStatus);

  Mock::VerifyAndClearExpectations(mock_sync_service_);
  // Now, act as if the SyncService has started up.
  SetDefaultExpectationsForConfigPage();
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(Return(syncer::SyncService::TransportState::ACTIVE));
  handler_->SyncStartupCompleted();

  EXPECT_EQ(2U, web_ui_.call_data().size());

  const base::DictionaryValue* dictionary = ExpectSyncPrefsChanged();
  CheckBool(dictionary, "syncAllDataTypes", true);
  CheckBool(dictionary, "encryptAllDataAllowed", true);
  CheckBool(dictionary, "encryptAllData", false);
  CheckBool(dictionary, "passphraseRequired", false);
}

// Verifies the case where the user cancels after the sync engine has
// initialized (meaning it already transitioned from the spinner to a proper
// configuration page, tested by
// DisplayConfigureWithEngineDisabledAndSyncStartupCompleted), but before the
// user has continued on.
TEST_F(PeopleHandlerTest,
       DisplayConfigureWithEngineDisabledAndCancelAfterSigninSuccess) {
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  EXPECT_CALL(*mock_sync_service_, GetTransportState())
      .WillOnce(Return(syncer::SyncService::TransportState::INITIALIZING))
      .WillRepeatedly(Return(syncer::SyncService::TransportState::ACTIVE));
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetSyncRequested(true));
  SetDefaultExpectationsForConfigPage();
  handler_->HandleShowSetupUI(nullptr);

  // It's important to tell sync the user cancelled the setup flow before we
  // tell it we're through with the setup progress.
  testing::InSequence seq;
  EXPECT_CALL(*mock_sync_service_, StopAndClear());
  EXPECT_CALL(*this, OnSetupInProgressHandleDestroyed());

  handler_->CloseSyncSetup();
  EXPECT_EQ(
      NULL,
      LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());
}

TEST_F(PeopleHandlerTest, DisplayConfigureWithEngineDisabledAndSigninFailed) {
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(Return(syncer::SyncService::TransportState::INITIALIZING));
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetSyncRequested(true));

  handler_->HandleShowSetupUI(nullptr);
  ExpectPageStatusChanged(PeopleHandler::kSpinnerPageStatus);
  Mock::VerifyAndClearExpectations(mock_sync_service_);
  ON_CALL(*mock_sync_service_, GetAuthError())
      .WillByDefault(Return(GoogleServiceAuthError(
          GoogleServiceAuthError::INVALID_GAIA_CREDENTIALS)));
  NotifySyncListeners();

  // On failure, the dialog will be closed.
  EXPECT_EQ(
      NULL,
      LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());
}

TEST_F(PeopleHandlerTest, RestartSyncAfterDashboardClear) {
  // Clearing sync from the dashboard results in DISABLE_REASON_USER_CHOICE
  // being set.
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_USER_CHOICE));
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(Return(syncer::SyncService::TransportState::DISABLED));

  // Attempting to open the setup UI should restart sync.
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetSyncRequested(true))
      .WillOnce([&](bool) {
        // SetSyncRequested(true) clears DISABLE_REASON_USER_CHOICE, and
        // immediately starts initializing the engine.
        ON_CALL(*mock_sync_service_, GetDisableReasons())
            .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
        ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
            .WillByDefault(Return(true));
        ON_CALL(*mock_sync_service_, GetTransportState())
            .WillByDefault(
                Return(syncer::SyncService::TransportState::INITIALIZING));
      });

  handler_->HandleShowSetupUI(nullptr);
  // Since the engine is not initialized yet, we should get a spinner.
  ExpectPageStatusChanged(PeopleHandler::kSpinnerPageStatus);
}

TEST_F(PeopleHandlerTest,
       RestartSyncAfterDashboardClearWithStandaloneTransport) {
  // Clearing sync from the dashboard results in DISABLE_REASON_USER_CHOICE
  // being set. However, the sync engine has restarted in standalone transport
  // mode.
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_USER_CHOICE));
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(Return(syncer::SyncService::TransportState::ACTIVE));

  // Attempting to open the setup UI should re-enable sync-the-feature.
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetSyncRequested(true))
      .WillOnce([&](bool) {
        // SetSyncRequested(true) clears DISABLE_REASON_USER_CHOICE. Since the
        // engine is already running, it just gets reconfigured.
        ON_CALL(*mock_sync_service_, GetDisableReasons())
            .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
        ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
            .WillByDefault(Return(true));
        ON_CALL(*mock_sync_service_, GetTransportState())
            .WillByDefault(
                Return(syncer::SyncService::TransportState::CONFIGURING));
      });

  handler_->HandleShowSetupUI(nullptr);
  // Since the engine was already running, we should *not* get a spinner - all
  // the necessary values are already available.
  ExpectSyncPrefsChanged();
}

// Tests that signals not related to user intention to configure sync don't
// trigger sync engine start.
TEST_F(PeopleHandlerTest, OnlyStartEngineWhenConfiguringSync) {
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(Return(syncer::SyncService::TransportState::INITIALIZING));
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetSyncRequested(true))
      .Times(0);
  NotifySyncStateChanged();
}

TEST_F(PeopleHandlerTest, AcquireSyncBlockerWhenLoadingSyncSettingsSubpage) {
  // We set up a factory override here to prevent a new web ui from being
  // created when we navigate to a page that would normally create one.
  web_ui_.set_web_contents(web_contents());
  test_factory_ = std::make_unique<TestChromeWebUIControllerFactory>();
  test_factory_->AddFactoryOverride(
      chrome::GetSettingsUrl(chrome::kSyncSetupSubPage).host(),
      &test_provider_);
  content::WebUIControllerFactory::RegisterFactory(test_factory_.get());
  content::WebUIControllerFactory::UnregisterFactoryForTesting(
      ChromeWebUIControllerFactory::GetInstance());

  EXPECT_FALSE(handler_->sync_blocker_);

  auto navigation = content::NavigationSimulator::CreateBrowserInitiated(
      chrome::GetSettingsUrl(chrome::kSyncSetupSubPage), web_contents());
  navigation->Start();
  handler_->InitializeSyncBlocker();

  EXPECT_TRUE(handler_->sync_blocker_);
}

#if !defined(OS_CHROMEOS)

class PeopleHandlerNonCrosTest : public PeopleHandlerTest {
 public:
  PeopleHandlerNonCrosTest() {}
};

// TODO(kochi): We need equivalent tests for ChromeOS.
TEST_F(PeopleHandlerNonCrosTest, UnrecoverableErrorInitializingSync) {
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(
          Return(syncer::SyncService::DISABLE_REASON_UNRECOVERABLE_ERROR));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  // Open the web UI.
  handler_->HandleShowSetupUI(nullptr);

  ASSERT_FALSE(handler_->is_configuring_sync());
}

TEST_F(PeopleHandlerNonCrosTest, GaiaErrorInitializingSync) {
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NOT_SIGNED_IN));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  // Open the web UI.
  handler_->HandleShowSetupUI(nullptr);

  ASSERT_FALSE(handler_->is_configuring_sync());
}

#endif  // #if !defined(OS_CHROMEOS)

TEST_F(PeopleHandlerTest, TestSyncEverything) {
  std::string args = GetConfiguration(
      NULL, SYNC_ALL_DATA, GetAllTypes(), std::string(), ENCRYPT_PASSWORDS);
  base::ListValue list_args;
  list_args.AppendString(kTestCallbackId);
  list_args.AppendString(args);
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsPassphraseRequiredForDecryption())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetChosenDataTypes(true, _));
  handler_->HandleSetDatatypes(&list_args);

  ExpectPageStatusResponse(PeopleHandler::kConfigurePageStatus);
}

TEST_F(PeopleHandlerTest, TestPassphraseStillRequired) {
  std::string args = GetConfiguration(
      NULL, SYNC_ALL_DATA, GetAllTypes(), std::string(), ENCRYPT_PASSWORDS);
  base::ListValue list_args;
  list_args.AppendString(kTestCallbackId);
  list_args.AppendString(args);
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsPassphraseRequiredForDecryption())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  SetDefaultExpectationsForConfigPage();

  handler_->HandleSetEncryption(&list_args);
  // We should navigate back to the configure page since we need a passphrase.
  ExpectPageStatusResponse(PeopleHandler::kPassphraseFailedPageStatus);
}

TEST_F(PeopleHandlerTest, EnterExistingFrozenImplicitPassword) {
  base::DictionaryValue dict;
  dict.SetBoolean("setNewPassphrase", false);
  std::string args = GetConfiguration(&dict, SYNC_ALL_DATA, GetAllTypes(),
                                      "oldGaiaPassphrase", ENCRYPT_PASSWORDS);
  base::ListValue list_args;
  list_args.AppendString(kTestCallbackId);
  list_args.AppendString(args);
  // Act as if an encryption passphrase is required the first time, then never
  // again after that.
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              IsPassphraseRequired())
      .WillOnce(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsPassphraseRequiredForDecryption())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetDecryptionPassphrase("oldGaiaPassphrase"))
      .WillOnce(Return(true));

  handler_->HandleSetEncryption(&list_args);
  ExpectPageStatusResponse(PeopleHandler::kConfigurePageStatus);
}

TEST_F(PeopleHandlerTest, SetNewCustomPassphrase) {
  base::DictionaryValue dict;
  dict.SetBoolean("setNewPassphrase", true);
  std::string args = GetConfiguration(&dict, SYNC_ALL_DATA, GetAllTypes(),
                                      "custom_passphrase", ENCRYPT_ALL_DATA);
  base::ListValue list_args;
  list_args.AppendString(kTestCallbackId);
  list_args.AppendString(args);
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsEncryptEverythingAllowed())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsPassphraseRequiredForDecryption())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetEncryptionPassphrase("custom_passphrase"));

  handler_->HandleSetEncryption(&list_args);
  ExpectPageStatusResponse(PeopleHandler::kConfigurePageStatus);
}

TEST_F(PeopleHandlerTest, EnterWrongExistingPassphrase) {
  base::DictionaryValue dict;
  dict.SetBoolean("setNewPassphrase", false);
  std::string args = GetConfiguration(&dict, SYNC_ALL_DATA, GetAllTypes(),
                                      "invalid_passphrase", ENCRYPT_ALL_DATA);
  base::ListValue list_args;
  list_args.AppendString(kTestCallbackId);
  list_args.AppendString(args);
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsPassphraseRequiredForDecryption())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetDecryptionPassphrase("invalid_passphrase"))
      .WillOnce(Return(false));

  SetDefaultExpectationsForConfigPage();

  handler_->HandleSetEncryption(&list_args);
  // We should navigate back to the configure page since we need a passphrase.
  ExpectPageStatusResponse(PeopleHandler::kPassphraseFailedPageStatus);
}

TEST_F(PeopleHandlerTest, EnterBlankExistingPassphrase) {
  base::DictionaryValue dict;
  dict.SetBoolean("setNewPassphrase", false);
  std::string args = GetConfiguration(&dict,
                                      SYNC_ALL_DATA,
                                      GetAllTypes(),
                                      "",
                                      ENCRYPT_PASSWORDS);
  base::ListValue list_args;
  list_args.AppendString(kTestCallbackId);
  list_args.AppendString(args);
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsPassphraseRequiredForDecryption())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();

  SetDefaultExpectationsForConfigPage();

  handler_->HandleSetEncryption(&list_args);
  // We should navigate back to the configure page since we need a passphrase.
  ExpectPageStatusResponse(PeopleHandler::kPassphraseFailedPageStatus);
}

// Walks through each user selectable type, and tries to sync just that single
// data type.
TEST_F(PeopleHandlerTest, TestSyncIndividualTypes) {
  syncer::ModelTypeSet user_selectable_types = GetAllTypes();
  for (syncer::ModelType type : user_selectable_types) {
    syncer::ModelTypeSet type_to_set;
    type_to_set.Put(type);
    std::string args = GetConfiguration(NULL,
                                        CHOOSE_WHAT_TO_SYNC,
                                        type_to_set,
                                        std::string(),
                                        ENCRYPT_PASSWORDS);
    base::ListValue list_args;
    list_args.AppendString(kTestCallbackId);
    list_args.AppendString(args);
    ON_CALL(*mock_sync_service_->GetMockUserSettings(),
            IsPassphraseRequiredForDecryption())
        .WillByDefault(Return(false));
    ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
        .WillByDefault(Return(false));
    SetupInitializedSyncService();
    EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
                SetChosenDataTypes(false, ModelTypeSetMatches(type_to_set)));

    handler_->HandleSetDatatypes(&list_args);
    ExpectPageStatusResponse(PeopleHandler::kConfigurePageStatus);
    Mock::VerifyAndClearExpectations(mock_sync_service_);
  }
}

TEST_F(PeopleHandlerTest, TestSyncAllManually) {
  std::string args = GetConfiguration(NULL,
                                      CHOOSE_WHAT_TO_SYNC,
                                      GetAllTypes(),
                                      std::string(),
                                      ENCRYPT_PASSWORDS);
  base::ListValue list_args;
  list_args.AppendString(kTestCallbackId);
  list_args.AppendString(args);
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsPassphraseRequiredForDecryption())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetChosenDataTypes(false, ModelTypeSetMatches(GetAllTypes())));
  handler_->HandleSetDatatypes(&list_args);

  ExpectPageStatusResponse(PeopleHandler::kConfigurePageStatus);
}

TEST_F(PeopleHandlerTest, ShowSyncSetup) {
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  // This should display the sync setup dialog (not login).
  SetDefaultExpectationsForConfigPage();
  handler_->HandleShowSetupUI(nullptr);

  ExpectSyncPrefsChanged();
}

// We do not display signin on chromeos in the case of auth error.
TEST_F(PeopleHandlerTest, ShowSigninOnAuthError) {
  // Initialize the system to a signed in state, but with an auth error.
  ON_CALL(*mock_sync_service_, GetAuthError())
      .WillByDefault(Return(GoogleServiceAuthError(
          GoogleServiceAuthError::INVALID_GAIA_CREDENTIALS)));

  SetupInitializedSyncService();

  auto* identity_manager = identity_test_env()->identity_manager();
  CoreAccountInfo primary_account_info =
      identity_manager->GetPrimaryAccountInfo();
  DCHECK_EQ(primary_account_info.email, kTestUser);

  auto* accounts_mutator = identity_manager->GetAccountsMutator();
  DCHECK(accounts_mutator);

  accounts_mutator->AddOrUpdateAccount(
      primary_account_info.gaia, primary_account_info.email, "refresh_token",
      primary_account_info.is_under_advanced_protection,
      signin_metrics::SourceForRefreshTokenOperation::kUnknown);

  identity::UpdatePersistentErrorOfRefreshTokenForAccount(
      identity_manager, primary_account_info.account_id,
      GoogleServiceAuthError(GoogleServiceAuthError::INVALID_GAIA_CREDENTIALS));

  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(Return(syncer::SyncService::TransportState::INITIALIZING));

#if defined(OS_CHROMEOS)
  // On ChromeOS, auth errors are ignored - instead we just try to start the
  // sync engine (which will fail due to the auth error). This should only
  // happen if the user manually navigates to chrome://settings/syncSetup -
  // clicking on the button in the UI will sign the user out rather than
  // displaying a spinner. Should be no visible UI on ChromeOS in this case.
  EXPECT_EQ(
      NULL,
      LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());
#else

  // On ChromeOS, this should display the spinner while we try to startup the
  // sync engine, and on desktop this displays the login dialog.
  handler_->HandleShowSetupUI(nullptr);

  // Sync setup is closed when re-auth is in progress.
  EXPECT_EQ(
      NULL,
      LoginUIServiceFactory::GetForProfile(profile())->current_login_ui());

  ASSERT_FALSE(handler_->is_configuring_sync());
#endif
}

TEST_F(PeopleHandlerTest, ShowSetupSyncEverything) {
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  SetDefaultExpectationsForConfigPage();
  // This should display the sync setup dialog (not login).
  handler_->HandleShowSetupUI(nullptr);

  const base::DictionaryValue* dictionary = ExpectSyncPrefsChanged();
  CheckBool(dictionary, "syncAllDataTypes", true);
  CheckBool(dictionary, "appsRegistered", true);
  CheckBool(dictionary, "autofillRegistered", true);
  CheckBool(dictionary, "bookmarksRegistered", true);
  CheckBool(dictionary, "extensionsRegistered", true);
  CheckBool(dictionary, "passwordsRegistered", true);
  CheckBool(dictionary, "preferencesRegistered", true);
  CheckBool(dictionary, "tabsRegistered", true);
  CheckBool(dictionary, "themesRegistered", true);
  CheckBool(dictionary, "typedUrlsRegistered", true);
  CheckBool(dictionary, "paymentsIntegrationEnabled", true);
  CheckBool(dictionary, "passphraseRequired", false);
  CheckBool(dictionary, "encryptAllData", false);
  CheckConfigDataTypeArguments(dictionary, SYNC_ALL_DATA, GetAllTypes());
}

TEST_F(PeopleHandlerTest, ShowSetupManuallySyncAll) {
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  SetDefaultExpectationsForConfigPage();
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncEverythingEnabled())
      .WillByDefault(Return(false));
  // This should display the sync setup dialog (not login).
  handler_->HandleShowSetupUI(nullptr);

  const base::DictionaryValue* dictionary = ExpectSyncPrefsChanged();
  CheckConfigDataTypeArguments(dictionary, CHOOSE_WHAT_TO_SYNC, GetAllTypes());
}

TEST_F(PeopleHandlerTest, ShowSetupSyncForAllTypesIndividually) {
  syncer::ModelTypeSet user_selectable_types = GetAllTypes();
  for (syncer::ModelType type : user_selectable_types) {
    ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
        .WillByDefault(Return(false));
    ON_CALL(*mock_sync_service_->GetMockUserSettings(),
            IsUsingSecondaryPassphrase())
        .WillByDefault(Return(false));
    SetupInitializedSyncService();
    SetDefaultExpectationsForConfigPage();
    ON_CALL(*mock_sync_service_->GetMockUserSettings(),
            IsSyncEverythingEnabled())
        .WillByDefault(Return(false));
    syncer::ModelTypeSet types(type);
    ON_CALL(*mock_sync_service_->GetMockUserSettings(), GetChosenDataTypes())
        .WillByDefault(Return(types));
    ON_CALL(*mock_sync_service_, GetPreferredDataTypes())
        .WillByDefault(Return(
            syncer::SyncUserSettingsImpl::ResolvePrefGroupsForTesting(types)));

    // This should display the sync setup dialog (not login).
    handler_->HandleShowSetupUI(nullptr);

    // Close the config overlay.
    LoginUIServiceFactory::GetForProfile(profile())->LoginUIClosed(
        handler_.get());

    const base::DictionaryValue* dictionary = ExpectSyncPrefsChanged();
    CheckConfigDataTypeArguments(dictionary, CHOOSE_WHAT_TO_SYNC, types);
    Mock::VerifyAndClearExpectations(mock_sync_service_);
    // Clean up so we can loop back to display the dialog again.
    web_ui_.ClearTrackedCalls();
  }
}

TEST_F(PeopleHandlerTest, ShowSetupOldGaiaPassphraseRequired) {
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), GetPassphraseType())
      .WillByDefault(
          Return(syncer::PassphraseType::FROZEN_IMPLICIT_PASSPHRASE));
  SetupInitializedSyncService();
  SetDefaultExpectationsForConfigPage();

  // This should display the sync setup dialog (not login).
  handler_->HandleShowSetupUI(nullptr);

  const base::DictionaryValue* dictionary = ExpectSyncPrefsChanged();
  CheckBool(dictionary, "passphraseRequired", true);
  EXPECT_TRUE(dictionary->FindKey("enterPassphraseBody"));
}

TEST_F(PeopleHandlerTest, ShowSetupCustomPassphraseRequired) {
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(true));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), GetPassphraseType())
      .WillByDefault(Return(syncer::PassphraseType::CUSTOM_PASSPHRASE));
  SetupInitializedSyncService();
  SetDefaultExpectationsForConfigPage();

  // This should display the sync setup dialog (not login).
  handler_->HandleShowSetupUI(nullptr);

  const base::DictionaryValue* dictionary = ExpectSyncPrefsChanged();
  CheckBool(dictionary, "passphraseRequired", true);
  EXPECT_TRUE(dictionary->FindKey("enterPassphraseBody"));
}

TEST_F(PeopleHandlerTest, ShowSetupEncryptAll) {
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  SetDefaultExpectationsForConfigPage();
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsEncryptEverythingEnabled())
      .WillByDefault(Return(true));

  // This should display the sync setup dialog (not login).
  handler_->HandleShowSetupUI(nullptr);

  const base::DictionaryValue* dictionary = ExpectSyncPrefsChanged();
  CheckBool(dictionary, "encryptAllData", true);
}

TEST_F(PeopleHandlerTest, ShowSetupEncryptAllDisallowed) {
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsUsingSecondaryPassphrase())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  SetDefaultExpectationsForConfigPage();
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsEncryptEverythingAllowed())
      .WillByDefault(Return(false));

  // This should display the sync setup dialog (not login).
  handler_->HandleShowSetupUI(nullptr);

  const base::DictionaryValue* dictionary = ExpectSyncPrefsChanged();
  CheckBool(dictionary, "encryptAllData", false);
  CheckBool(dictionary, "encryptAllDataAllowed", false);
}

TEST_F(PeopleHandlerTest, TurnOnEncryptAllDisallowed) {
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsPassphraseRequiredForDecryption())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsPassphraseRequired())
      .WillByDefault(Return(false));
  SetupInitializedSyncService();
  ON_CALL(*mock_sync_service_->GetMockUserSettings(),
          IsEncryptEverythingAllowed())
      .WillByDefault(Return(false));

  base::DictionaryValue dict;
  dict.SetBoolean("setNewPassphrase", true);
  std::string args = GetConfiguration(&dict, SYNC_ALL_DATA, GetAllTypes(),
                                      "password", ENCRYPT_ALL_DATA);
  base::ListValue list_args;
  list_args.AppendString(kTestCallbackId);
  list_args.AppendString(args);

  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              EnableEncryptEverything())
      .Times(0);
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetEncryptionPassphrase(_))
      .Times(0);

  handler_->HandleSetEncryption(&list_args);

  ExpectPageStatusResponse(PeopleHandler::kConfigurePageStatus);
}

TEST_F(PeopleHandlerTest, DashboardClearWhileSettingsOpen_ConfirmSoon) {
  // Sync starts out fully enabled.
  SetDefaultExpectationsForConfigPage();

  handler_->HandleShowSetupUI(nullptr);

  // Now sync gets reset from the dashboard (the user clicked the "Manage synced
  // data" link), which results in the sync-requested and first-setup-complete
  // bits being cleared.
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_USER_CHOICE));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  // Sync will eventually start again in transport mode.
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(
          Return(syncer::SyncService::TransportState::START_DEFERRED));

  NotifySyncStateChanged();

  // Now the user confirms sync again. This should set both the sync-requested
  // and the first-setup-complete bits.
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetSyncRequested(true))
      .WillOnce([&](bool) {
        // SetSyncRequested(true) clears DISABLE_REASON_USER_CHOICE, and
        // immediately starts initializing the engine.
        ON_CALL(*mock_sync_service_, GetDisableReasons())
            .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
        ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
            .WillByDefault(Return(true));
        ON_CALL(*mock_sync_service_, GetTransportState())
            .WillByDefault(
                Return(syncer::SyncService::TransportState::INITIALIZING));
        NotifySyncStateChanged();
      });
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetFirstSetupComplete())
      .WillOnce([&]() {
        ON_CALL(*mock_sync_service_->GetMockUserSettings(),
                IsFirstSetupComplete())
            .WillByDefault(Return(true));
        NotifySyncStateChanged();
      });

  base::ListValue did_abort;
  did_abort.GetList().push_back(base::Value(false));
  handler_->OnDidClosePage(&did_abort);
}

TEST_F(PeopleHandlerTest, DashboardClearWhileSettingsOpen_ConfirmLater) {
  // Sync starts out fully enabled.
  SetDefaultExpectationsForConfigPage();

  handler_->HandleShowSetupUI(nullptr);

  // Now sync gets reset from the dashboard (the user clicked the "Manage synced
  // data" link), which results in the sync-requested and first-setup-complete
  // bits being cleared.
  ON_CALL(*mock_sync_service_, GetDisableReasons())
      .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_USER_CHOICE));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
      .WillByDefault(Return(false));
  ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
      .WillByDefault(Return(false));
  // Sync will eventually start again in transport mode.
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(
          Return(syncer::SyncService::TransportState::START_DEFERRED));

  NotifySyncStateChanged();

  // The user waits a while before doing anything, so sync starts up in
  // transport mode.
  ON_CALL(*mock_sync_service_, GetTransportState())
      .WillByDefault(Return(syncer::SyncService::TransportState::ACTIVE));
  // On some platforms (e.g. ChromeOS), the first-setup-complete bit gets set
  // automatically during engine startup.
  if (browser_defaults::kSyncAutoStarts) {
    ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsFirstSetupComplete())
        .WillByDefault(Return(true));
  }
  NotifySyncStateChanged();

  // Now the user confirms sync again. This should set the sync-requested bit
  // and (if it wasn't automatically set above already) also the
  // first-setup-complete bit.
  EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
              SetSyncRequested(true))
      .WillOnce([&](bool) {
        // SetSyncRequested(true) clears DISABLE_REASON_USER_CHOICE, and
        // immediately starts initializing the engine.
        ON_CALL(*mock_sync_service_, GetDisableReasons())
            .WillByDefault(Return(syncer::SyncService::DISABLE_REASON_NONE));
        ON_CALL(*mock_sync_service_->GetMockUserSettings(), IsSyncRequested())
            .WillByDefault(Return(true));
        ON_CALL(*mock_sync_service_, GetTransportState())
            .WillByDefault(
                Return(syncer::SyncService::TransportState::INITIALIZING));
        NotifySyncStateChanged();
      });
  if (!browser_defaults::kSyncAutoStarts) {
    EXPECT_CALL(*mock_sync_service_->GetMockUserSettings(),
                SetFirstSetupComplete())
        .WillOnce([&]() {
          ON_CALL(*mock_sync_service_->GetMockUserSettings(),
                  IsFirstSetupComplete())
              .WillByDefault(Return(true));
          NotifySyncStateChanged();
        });
  }

  base::ListValue did_abort;
  did_abort.GetList().push_back(base::Value(false));
  handler_->OnDidClosePage(&did_abort);
}

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
class PeopleHandlerDiceUnifiedConsentTest
    : public ::testing::TestWithParam<std::tuple<bool, bool>> {};

TEST_P(PeopleHandlerDiceUnifiedConsentTest, StoredAccountsList) {
  content::TestBrowserThreadBundle test_browser_thread_bundle;

  // Decode test parameters.
  bool dice_enabled;
  bool unified_consent_enabled;
  std::tie(dice_enabled, unified_consent_enabled) = GetParam();
  unified_consent::ScopedUnifiedConsent unified_consent(
      unified_consent_enabled
          ? unified_consent::UnifiedConsentFeatureState::kEnabled
          : unified_consent::UnifiedConsentFeatureState::kDisabled);
  ScopedAccountConsistency dice(
      dice_enabled ? signin::AccountConsistencyMethod::kDice
                   : signin::AccountConsistencyMethod::kDiceMigration);

  // Setup the profile.
  std::unique_ptr<TestingProfile> profile =
      IdentityTestEnvironmentProfileAdaptor::
          CreateProfileForIdentityTestEnvironment();

  auto identity_test_env_adaptor =
      std::make_unique<IdentityTestEnvironmentProfileAdaptor>(profile.get());
  auto* identity_test_env = identity_test_env_adaptor->identity_test_env();

  auto account_1 = identity_test_env->MakeAccountAvailable("a@gmail.com");
  auto account_2 = identity_test_env->MakeAccountAvailable("b@gmail.com");
  identity_test_env->SetPrimaryAccount(account_1.email);

  PeopleHandler handler(profile.get());
  base::Value accounts = handler.GetStoredAccountsList();

  ASSERT_TRUE(accounts.is_list());
  const base::Value::ListStorage& accounts_list = accounts.GetList();

  if (dice_enabled) {
    ASSERT_EQ(2u, accounts_list.size());
    ASSERT_TRUE(accounts_list[0].FindKey("email"));
    ASSERT_TRUE(accounts_list[1].FindKey("email"));
    EXPECT_EQ("a@gmail.com", accounts_list[0].FindKey("email")->GetString());
    EXPECT_EQ("b@gmail.com", accounts_list[1].FindKey("email")->GetString());
  } else if (unified_consent_enabled) {
    ASSERT_EQ(1u, accounts_list.size());
    ASSERT_TRUE(accounts_list[0].FindKey("email"));
    EXPECT_EQ("a@gmail.com", accounts_list[0].FindKey("email")->GetString());
  } else {
    EXPECT_EQ(0u, accounts_list.size());
  }
}

INSTANTIATE_TEST_SUITE_P(Test,
                         PeopleHandlerDiceUnifiedConsentTest,
                         ::testing::Combine(::testing::Bool(),
                                            ::testing::Bool()));
#endif  // BUILDFLAG(ENABLE_DICE_SUPPORT)

}  // namespace settings
