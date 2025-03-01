// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/sync/driver/sync_auth_manager.h"

#include "base/bind_helpers.h"
#include "base/run_loop.h"
#include "base/test/mock_callback.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_task_environment.h"
#include "build/build_config.h"
#include "components/sync/driver/sync_driver_switches.h"
#include "components/sync/engine/connection_status.h"
#include "components/sync/engine/sync_credentials.h"
#include "net/base/net_errors.h"
#include "services/identity/public/cpp/identity_test_environment.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace syncer {

namespace {

class SyncAuthManagerTest : public testing::Test {
 protected:
  using AccountStateChangedCallback =
      SyncAuthManager::AccountStateChangedCallback;
  using CredentialsChangedCallback =
      SyncAuthManager::CredentialsChangedCallback;

  SyncAuthManagerTest() : identity_env_(&test_url_loader_factory_) {}

  ~SyncAuthManagerTest() override {}

  std::unique_ptr<SyncAuthManager> CreateAuthManager() {
    return CreateAuthManager(base::DoNothing(), base::DoNothing());
  }

  std::unique_ptr<SyncAuthManager> CreateAuthManager(
      const AccountStateChangedCallback& account_state_changed,
      const CredentialsChangedCallback& credentials_changed) {
    return std::make_unique<SyncAuthManager>(identity_env_.identity_manager(),
                                             account_state_changed,
                                             credentials_changed);
  }

  std::unique_ptr<SyncAuthManager> CreateAuthManagerForLocalSync() {
    return std::make_unique<SyncAuthManager>(nullptr, base::DoNothing(),
                                             base::DoNothing());
  }

  identity::IdentityTestEnvironment* identity_env() { return &identity_env_; }

 private:
  base::test::ScopedTaskEnvironment task_environment_;
  network::TestURLLoaderFactory test_url_loader_factory_;
  identity::IdentityTestEnvironment identity_env_;
};

TEST_F(SyncAuthManagerTest, ProvidesNothingInLocalSyncMode) {
  auto auth_manager = CreateAuthManagerForLocalSync();
  EXPECT_TRUE(auth_manager->GetActiveAccountInfo().account_info.IsEmpty());
  syncer::SyncCredentials credentials = auth_manager->GetCredentials();
  EXPECT_TRUE(credentials.account_id.empty());
  EXPECT_TRUE(credentials.email.empty());
  EXPECT_TRUE(credentials.access_token.empty());
  EXPECT_TRUE(auth_manager->access_token().empty());
  // Note: Calling RegisterForAuthNotifications or any of the Connection*()
  // methods is illegal in local Sync mode, so we don't test that.
}

// ChromeOS doesn't support sign-in/sign-out.
#if !defined(OS_CHROMEOS)
TEST_F(SyncAuthManagerTest, IgnoresEventsIfNotRegistered) {
  base::MockCallback<AccountStateChangedCallback> account_state_changed;
  base::MockCallback<CredentialsChangedCallback> credentials_changed;
  EXPECT_CALL(account_state_changed, Run()).Times(0);
  EXPECT_CALL(credentials_changed, Run()).Times(0);
  auto auth_manager =
      CreateAuthManager(account_state_changed.Get(), credentials_changed.Get());

  // Fire some auth events. We haven't called RegisterForAuthNotifications, so
  // none of this should result in any callback calls.
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  // Without RegisterForAuthNotifications, the active account should always be
  // reported as empty.
  EXPECT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());
  identity_env()->SetRefreshTokenForPrimaryAccount();
  EXPECT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());
  identity_env()->ClearPrimaryAccount();
  EXPECT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());
}

TEST_F(SyncAuthManagerTest, ForwardsPrimaryAccountEvents) {
  // Start out already signed in before the SyncAuthManager is created.
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;

  base::MockCallback<AccountStateChangedCallback> account_state_changed;
  base::MockCallback<CredentialsChangedCallback> credentials_changed;
  EXPECT_CALL(account_state_changed, Run()).Times(0);
  EXPECT_CALL(credentials_changed, Run()).Times(0);
  auto auth_manager =
      CreateAuthManager(account_state_changed.Get(), credentials_changed.Get());

  auth_manager->RegisterForAuthNotifications();

  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  // Sign out of the account.
  EXPECT_CALL(account_state_changed, Run());
  // Note: The ordering of removing the refresh token and the actual sign-out is
  // undefined, see comment on IdentityManager::Observer. So we might or might
  // not get a |credentials_changed| call here.
  EXPECT_CALL(credentials_changed, Run()).Times(testing::AtMost(1));
  identity_env()->ClearPrimaryAccount();
  EXPECT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());

  // Sign in to a different account.
  EXPECT_CALL(account_state_changed, Run());
  std::string second_account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  EXPECT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            second_account_id);
}

TEST_F(SyncAuthManagerTest, ClearsAuthErrorOnSignout) {
  // Start out already signed in before the SyncAuthManager is created.
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;

  auto auth_manager = CreateAuthManager();

  auth_manager->RegisterForAuthNotifications();

  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);
  ASSERT_EQ(auth_manager->GetLastAuthError().state(),
            GoogleServiceAuthError::NONE);

  // Sign out of the account.
  // The ordering of removing the refresh token and the actual sign-out is
  // undefined, see comment on IdentityManager::Observer. Here, explicitly
  // revoke the refresh token first to force an auth error.
  identity_env()->RemoveRefreshTokenForPrimaryAccount();

  ASSERT_NE(auth_manager->GetLastAuthError().state(),
            GoogleServiceAuthError::NONE);

  // Now actually sign out, i.e. remove the primary account. This should clear
  // the auth error, since it's now not meaningful anymore.
  identity_env()->ClearPrimaryAccount();
  EXPECT_EQ(auth_manager->GetLastAuthError().state(),
            GoogleServiceAuthError::NONE);
}

TEST_F(SyncAuthManagerTest, DoesNotClearAuthErrorOnSyncDisable) {
  // Start out already signed in before the SyncAuthManager is created.
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;

  auto auth_manager = CreateAuthManager();

  auth_manager->RegisterForAuthNotifications();

  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);
  ASSERT_EQ(auth_manager->GetLastAuthError().state(),
            GoogleServiceAuthError::NONE);

  // Force an auth error by revoking the refresh token.
  identity_env()->RemoveRefreshTokenForPrimaryAccount();
  ASSERT_NE(auth_manager->GetLastAuthError().state(),
            GoogleServiceAuthError::NONE);

  // Now Sync gets turned off, e.g. because the user disabled it.
  auth_manager->ConnectionClosed();

  // Since the user is still signed in, the auth error should have remained.
  EXPECT_NE(auth_manager->GetLastAuthError().state(),
            GoogleServiceAuthError::NONE);
}
#endif  // !OS_CHROMEOS

TEST_F(SyncAuthManagerTest, ForwardsCredentialsEvents) {
  // Start out already signed in before the SyncAuthManager is created.
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;

  base::MockCallback<AccountStateChangedCallback> account_state_changed;
  base::MockCallback<CredentialsChangedCallback> credentials_changed;
  EXPECT_CALL(account_state_changed, Run()).Times(0);
  EXPECT_CALL(credentials_changed, Run()).Times(0);
  auto auth_manager =
      CreateAuthManager(account_state_changed.Get(), credentials_changed.Get());

  auth_manager->RegisterForAuthNotifications();

  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();

  // Once an access token is available, the callback should get run.
  EXPECT_CALL(credentials_changed, Run());
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token", base::Time::Now() + base::TimeDelta::FromHours(1));
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");

  // Now the refresh token gets updated. The access token will get dropped, so
  // this should cause another notification.
  EXPECT_CALL(credentials_changed, Run());
  identity_env()->SetRefreshTokenForPrimaryAccount();
  ASSERT_TRUE(auth_manager->GetCredentials().access_token.empty());

  // Once a new token is available, there's another notification.
  EXPECT_CALL(credentials_changed, Run());
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token_2", base::Time::Now() + base::TimeDelta::FromHours(1));
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token_2");

  // Revoking the refresh token should also cause the access token to get
  // dropped.
  EXPECT_CALL(credentials_changed, Run());
  identity_env()->RemoveRefreshTokenForPrimaryAccount();
  EXPECT_TRUE(auth_manager->GetCredentials().access_token.empty());
}

TEST_F(SyncAuthManagerTest, RequestsAccessTokenOnSyncStartup) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();

  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token", base::Time::Now() + base::TimeDelta::FromHours(1));

  EXPECT_EQ(auth_manager->GetCredentials().access_token, "access_token");
}

TEST_F(SyncAuthManagerTest,
       RetriesAccessTokenFetchWithBackoffOnTransientFailure) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();

  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithError(
      GoogleServiceAuthError::FromConnectionError(net::ERR_TIMED_OUT));

  // The access token fetch should get retried (with backoff, hence no actual
  // request yet), without exposing an auth error.
  EXPECT_TRUE(auth_manager->IsRetryingAccessTokenFetchForTest());
  EXPECT_EQ(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());
}

TEST_F(SyncAuthManagerTest, AbortsAccessTokenFetchOnPersistentFailure) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();

  GoogleServiceAuthError auth_error =
      GoogleServiceAuthError::FromInvalidGaiaCredentialsReason(
          GoogleServiceAuthError::InvalidGaiaCredentialsReason::
              CREDENTIALS_REJECTED_BY_SERVER);
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithError(
      auth_error);

  // Auth error should get exposed; no retry.
  EXPECT_FALSE(auth_manager->IsRetryingAccessTokenFetchForTest());
  EXPECT_EQ(auth_manager->GetLastAuthError(), auth_error);
}

TEST_F(SyncAuthManagerTest, FetchesNewAccessTokenWithBackoffOnServerError) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token", base::Time::Now() + base::TimeDelta::FromHours(1));
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");

  // The server is returning AUTH_ERROR - maybe something's wrong with the
  // token we got.
  auth_manager->ConnectionStatusChanged(syncer::CONNECTION_AUTH_ERROR);

  // The access token fetch should get retried (with backoff, hence no actual
  // request yet), without exposing an auth error.
  EXPECT_TRUE(auth_manager->IsRetryingAccessTokenFetchForTest());
  EXPECT_EQ(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());
}

TEST_F(SyncAuthManagerTest, ExposesServerError) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token", base::Time::Now() + base::TimeDelta::FromHours(1));
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");

  // Now a server error happens.
  auth_manager->ConnectionStatusChanged(syncer::CONNECTION_SERVER_ERROR);

  // The error should be reported.
  EXPECT_NE(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());
  // But the access token should still be there - this might just be some
  // non-auth-related problem with the server.
  EXPECT_EQ(auth_manager->GetCredentials().access_token, "access_token");
}

TEST_F(SyncAuthManagerTest, ClearsServerErrorOnSyncDisable) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token", base::Time::Now() + base::TimeDelta::FromHours(1));
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");

  // A server error happens.
  auth_manager->ConnectionStatusChanged(syncer::CONNECTION_SERVER_ERROR);
  ASSERT_NE(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());

  // Now Sync gets turned off, e.g. because the user disabled it.
  auth_manager->ConnectionClosed();

  // This should have cleared the auth error, because it was due to a server
  // error which is now not meaningful anymore.
  EXPECT_EQ(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());
}

TEST_F(SyncAuthManagerTest, RequestsNewAccessTokenOnExpiry) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token", base::Time::Now() + base::TimeDelta::FromHours(1));
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");

  // Now everything is okay for a while.
  auth_manager->ConnectionStatusChanged(syncer::CONNECTION_OK);
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");
  ASSERT_EQ(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());

  // But then the token expires, resulting in an auth error from the server.
  auth_manager->ConnectionStatusChanged(syncer::CONNECTION_AUTH_ERROR);

  // Should immediately drop the access token and fetch a new one (no backoff).
  EXPECT_TRUE(auth_manager->GetCredentials().access_token.empty());

  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token_2", base::Time::Now() + base::TimeDelta::FromHours(1));
  EXPECT_EQ(auth_manager->GetCredentials().access_token, "access_token_2");
}

TEST_F(SyncAuthManagerTest, RequestsNewAccessTokenOnRefreshTokenUpdate) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token", base::Time::Now() + base::TimeDelta::FromHours(1));
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");

  // Now everything is okay for a while.
  auth_manager->ConnectionStatusChanged(syncer::CONNECTION_OK);
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");
  ASSERT_EQ(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());

  // But then the refresh token changes.
  identity_env()->SetRefreshTokenForPrimaryAccount();

  // Should immediately drop the access token and fetch a new one (no backoff).
  EXPECT_TRUE(auth_manager->GetCredentials().access_token.empty());

  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token_2", base::Time::Now() + base::TimeDelta::FromHours(1));
  EXPECT_EQ(auth_manager->GetCredentials().access_token, "access_token_2");
}

TEST_F(SyncAuthManagerTest, DoesNotRequestAccessTokenAutonomously) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  // Do *not* call ConnectionStatusChanged here (which is what usually kicks off
  // the token fetch).

  // Now the refresh token gets updated. If we already had an access token
  // before, then this should trigger a new fetch. But since that initial fetch
  // never happened (e.g. because Sync is turned off), this should do nothing.
  base::MockCallback<base::OnceClosure> access_token_requested;
  EXPECT_CALL(access_token_requested, Run()).Times(0);
  identity_env()->SetCallbackForNextAccessTokenRequest(
      access_token_requested.Get());
  identity_env()->SetRefreshTokenForPrimaryAccount();

  // Make sure no access token request was sent. Since the request goes through
  // posted tasks, we have to spin the message loop.
  base::RunLoop().RunUntilIdle();

  EXPECT_TRUE(auth_manager->GetCredentials().access_token.empty());
}

TEST_F(SyncAuthManagerTest, ClearsCredentialsOnRefreshTokenRemoval) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token", base::Time::Now() + base::TimeDelta::FromHours(1));
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");

  // Now everything is okay for a while.
  auth_manager->ConnectionStatusChanged(syncer::CONNECTION_OK);
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");
  ASSERT_EQ(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());

  // But then the refresh token gets revoked. No new access token should get
  // requested due to this.
  base::MockCallback<base::OnceClosure> access_token_requested;
  EXPECT_CALL(access_token_requested, Run()).Times(0);
  identity_env()->SetCallbackForNextAccessTokenRequest(
      access_token_requested.Get());
  identity_env()->RemoveRefreshTokenForPrimaryAccount();

  // Should immediately drop the access token and expose an auth error.
  EXPECT_TRUE(auth_manager->GetCredentials().access_token.empty());
  EXPECT_NE(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());

  // No new access token should have been requested. Since the request goes
  // through posted tasks, we have to spin the message loop.
  base::RunLoop().RunUntilIdle();
}

TEST_F(SyncAuthManagerTest, ClearsCredentialsOnInvalidRefreshToken) {
  std::string account_id =
      identity_env()->MakePrimaryAccountAvailable("test@email.com").account_id;
  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_id);

  auth_manager->ConnectionOpened();
  identity_env()->WaitForAccessTokenRequestIfNecessaryAndRespondWithToken(
      "access_token", base::Time::Now() + base::TimeDelta::FromHours(1));
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");

  // Now everything is okay for a while.
  auth_manager->ConnectionStatusChanged(syncer::CONNECTION_OK);
  ASSERT_EQ(auth_manager->GetCredentials().access_token, "access_token");
  ASSERT_EQ(auth_manager->GetLastAuthError(),
            GoogleServiceAuthError::AuthErrorNone());

  // But now an invalid refresh token gets set. No new access token should get
  // requested due to this.
  base::MockCallback<base::OnceClosure> access_token_requested;
  EXPECT_CALL(access_token_requested, Run()).Times(0);
  identity_env()->SetCallbackForNextAccessTokenRequest(
      access_token_requested.Get());
  identity_env()->SetInvalidRefreshTokenForPrimaryAccount();

  // Should immediately drop the access token and expose a special auth error.
  EXPECT_TRUE(auth_manager->GetCredentials().access_token.empty());
  GoogleServiceAuthError invalid_token_error =
      GoogleServiceAuthError::FromInvalidGaiaCredentialsReason(
          GoogleServiceAuthError::InvalidGaiaCredentialsReason::
              CREDENTIALS_REJECTED_BY_CLIENT);
  EXPECT_EQ(auth_manager->GetLastAuthError(), invalid_token_error);

  // No new access token should have been requested. Since the request goes
  // through posted tasks, we have to spin the message loop.
  base::RunLoop().RunUntilIdle();
}

TEST_F(SyncAuthManagerTest, IgnoresCookieJarIfFeatureDisabled) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(switches::kSyncSupportSecondaryAccount);

  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();

  ASSERT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());

  // Make a non-primary account available with both a refresh token and cookie.
  AccountInfo account_info =
      identity_env()->MakeAccountAvailable("test@email.com");
  identity_env()->SetCookieAccounts({{account_info.email, account_info.gaia}});

  // Since secondary account support is disabled, this should have no effect.
  EXPECT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());
}

TEST_F(SyncAuthManagerTest, UsesCookieJarIfFeatureEnabled) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(switches::kSyncSupportSecondaryAccount);

  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();

  ASSERT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());

  // Make a non-primary account available with both a refresh token and cookie.
  AccountInfo account_info =
      identity_env()->MakeAccountAvailable("test@email.com");
  identity_env()->SetCookieAccounts({{account_info.email, account_info.gaia}});

  // Since secondary account support is enabled, SyncAuthManager should have
  // picked up this account
  EXPECT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_info.account_id);
}

TEST_F(SyncAuthManagerTest, DropsAccountWhenCookieGoesAway) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(switches::kSyncSupportSecondaryAccount);

  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();

  // Make a non-primary account available with both a refresh token and cookie.
  AccountInfo account_info =
      identity_env()->MakeAccountAvailable("test@email.com");
  identity_env()->SetCookieAccounts({{account_info.email, account_info.gaia}});
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_info.account_id);

  // If the cookie goes away, we're not using the account anymore, even though
  // we still have a refresh token.
  identity_env()->SetCookieAccounts({});
  EXPECT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());

  // Once the cookie comes back, we can use the account again.
  identity_env()->SetCookieAccounts({{account_info.email, account_info.gaia}});
  EXPECT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_info.account_id);
}

TEST_F(SyncAuthManagerTest, DropsAccountWhenRefreshTokenGoesAway) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(switches::kSyncSupportSecondaryAccount);

  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();

  // Make a non-primary account available with both a refresh token and cookie.
  AccountInfo account_info =
      identity_env()->MakeAccountAvailable("test@email.com");
  identity_env()->SetCookieAccounts({{account_info.email, account_info.gaia}});
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_info.account_id);

  // If the refresh token goes away, we're not using the account anymore, even
  // though the cookie is still there.
  identity_env()->RemoveRefreshTokenForAccount(account_info.account_id);
  EXPECT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());

  // Once the refresh token comes back, we can use the account again.
  identity_env()->SetRefreshTokenForAccount(account_info.account_id);
  EXPECT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_info.account_id);
}

TEST_F(SyncAuthManagerTest, PrefersPrimaryAccountOverCookie) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(switches::kSyncSupportSecondaryAccount);

  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();

  ASSERT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());

  // Make a non-primary account available with both a refresh token and cookie.
  AccountInfo secondary_account_info =
      identity_env()->MakeAccountAvailable("test@email.com");
  identity_env()->SetCookieAccounts(
      {{secondary_account_info.email, secondary_account_info.gaia}});
  ASSERT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            secondary_account_info.account_id);

  // Once a primary account becomes available, that one is preferred over the
  // one from the cookie.
  AccountInfo primary_account_info =
      identity_env()->MakePrimaryAccountAvailable("primary@email.com");
  EXPECT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            primary_account_info.account_id);
}

TEST_F(SyncAuthManagerTest, OnlyUsesFirstCookieAccount) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(switches::kSyncSupportSecondaryAccount);

  auto auth_manager = CreateAuthManager();
  auth_manager->RegisterForAuthNotifications();

  ASSERT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());

  // Make two non-primary accounts available with both refresh token and cookie.
  AccountInfo account_info1 =
      identity_env()->MakeAccountAvailable("test1@email.com");
  AccountInfo account_info2 =
      identity_env()->MakeAccountAvailable("test2@email.com");
  identity_env()->SetCookieAccounts(
      {{account_info1.email, account_info1.gaia},
       {account_info2.email, account_info2.gaia}});

  // SyncAuthManager should have picked up the first account.
  EXPECT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_info1.account_id);

  // If the order of the accounts in the cookie changes, then SyncAuthManager
  // should move to the other (now-first) account.
  identity_env()->SetCookieAccounts(
      {{account_info2.email, account_info2.gaia},
       {account_info1.email, account_info1.gaia}});
  EXPECT_EQ(auth_manager->GetActiveAccountInfo().account_info.account_id,
            account_info2.account_id);

  // If the refresh token for this account goes away, there should be no active
  // account anymore - we should *not* fall back to the second cookie account.
  identity_env()->RemoveRefreshTokenForAccount(account_info2.account_id);
  EXPECT_TRUE(
      auth_manager->GetActiveAccountInfo().account_info.account_id.empty());
}

}  // namespace

}  // namespace syncer
