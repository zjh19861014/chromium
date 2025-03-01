// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SYNC_DRIVER_PROFILE_SYNC_SERVICE_BUNDLE_H_
#define COMPONENTS_SYNC_DRIVER_PROFILE_SYNC_SERVICE_BUNDLE_H_

#include <memory>

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "components/invalidation/impl/fake_invalidation_service.h"
#include "components/invalidation/impl/profile_identity_provider.h"
#include "components/sync/device_info/local_device_info_provider_impl.h"
#include "components/sync/driver/profile_sync_service.h"
#include "components/sync/driver/sync_api_component_factory_mock.h"
#include "components/sync/driver/sync_client_mock.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "services/identity/public/cpp/identity_test_environment.h"
#include "services/network/test/test_url_loader_factory.h"

namespace syncer {

// Aggregate this class to get all necessary support for creating a
// ProfileSyncService in tests. The test still needs to have its own
// MessageLoop, though.
class ProfileSyncServiceBundle {
 public:
  ProfileSyncServiceBundle();

  ~ProfileSyncServiceBundle();

  // Creates a mock sync client that leverages the dependencies in this bundle.
  std::unique_ptr<SyncClientMock> CreateSyncClientMock();

  // Creates an InitParams instance with the specified |start_behavior| and
  // |sync_client|, and fills the rest with dummy values and objects owned by
  // the bundle.
  ProfileSyncService::InitParams CreateBasicInitParams(
      ProfileSyncService::StartBehavior start_behavior,
      std::unique_ptr<SyncClient> sync_client);

  // Accessors

  network::TestURLLoaderFactory* url_loader_factory() {
    return &test_url_loader_factory_;
  }

  sync_preferences::TestingPrefServiceSyncable* pref_service() {
    return &pref_service_;
  }

  identity::IdentityTestEnvironment* identity_test_env() {
    return &identity_test_env_;
  }

  identity::IdentityManager* identity_manager() {
    return identity_test_env_.identity_manager();
  }

  SyncApiComponentFactoryMock* component_factory() {
    return &component_factory_;
  }

  invalidation::ProfileIdentityProvider* identity_provider() {
    return identity_provider_.get();
  }

  invalidation::FakeInvalidationService* fake_invalidation_service() {
    return &fake_invalidation_service_;
  }

  LocalDeviceInfoProvider* local_device_info_provider() {
    return &local_device_info_provider_;
  }

 private:
  sync_preferences::TestingPrefServiceSyncable pref_service_;
  LocalDeviceInfoProviderImpl local_device_info_provider_;
  identity::IdentityTestEnvironment identity_test_env_;
  testing::NiceMock<SyncApiComponentFactoryMock> component_factory_;
  std::unique_ptr<invalidation::ProfileIdentityProvider> identity_provider_;
  invalidation::FakeInvalidationService fake_invalidation_service_;
  network::TestURLLoaderFactory test_url_loader_factory_;

  DISALLOW_COPY_AND_ASSIGN(ProfileSyncServiceBundle);
};

}  // namespace syncer

#endif  // COMPONENTS_SYNC_DRIVER_PROFILE_SYNC_SERVICE_BUNDLE_H_
