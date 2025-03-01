// Copyright (c) 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sync/profile_sync_service_factory.h"

#include <stddef.h>

#include <vector>

#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/task/task_scheduler/task_scheduler.h"
#include "build/build_config.h"
#include "chrome/common/buildflags.h"
#include "chrome/test/base/testing_profile.h"
#include "components/browser_sync/browser_sync_switches.h"
#include "components/sync/base/model_type.h"
#include "components/sync/driver/data_type_controller.h"
#include "components/sync/driver/sync_driver_switches.h"
#include "components/sync/driver/sync_service.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "testing/gtest/include/gtest/gtest.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/arc/arc_util.h"
#endif

class ProfileSyncServiceFactoryTest : public testing::Test {
 public:
  void SetUp() override {
    // Some services will only be created if there is a WebDataService.
    profile_->CreateWebDataService();
  }

  void TearDown() override {
    base::TaskScheduler::GetInstance()->FlushForTesting();
  }

 protected:
  ProfileSyncServiceFactoryTest() : profile_(new TestingProfile()) {}

  // Returns the collection of default datatypes.
  std::vector<syncer::ModelType> DefaultDatatypes() {
    static_assert(44 == syncer::ModelType::NUM_ENTRIES,
                  "When adding a new type, you probably want to add it here as "
                  "well (assuming it is already enabled).");

    std::vector<syncer::ModelType> datatypes;

    // Desktop types.
#if !defined(OS_ANDROID)
    datatypes.push_back(syncer::APPS);
#if BUILDFLAG(ENABLE_APP_LIST)
    datatypes.push_back(syncer::APP_LIST);
#endif
    datatypes.push_back(syncer::APP_SETTINGS);
#if defined(OS_LINUX) || defined(OS_WIN) || defined(OS_CHROMEOS)
    datatypes.push_back(syncer::DICTIONARY);
#endif
    datatypes.push_back(syncer::EXTENSIONS);
    datatypes.push_back(syncer::EXTENSION_SETTINGS);
    datatypes.push_back(syncer::SEARCH_ENGINES);
    datatypes.push_back(syncer::THEMES);
#endif  // !OS_ANDROID

#if defined(OS_CHROMEOS)
    if (arc::IsArcAllowedForProfile(profile()))
      datatypes.push_back(syncer::ARC_PACKAGE);
    datatypes.push_back(syncer::PRINTERS);
#endif  // OS_CHROMEOS

    // Common types.
    datatypes.push_back(syncer::AUTOFILL);
    datatypes.push_back(syncer::AUTOFILL_PROFILE);
    datatypes.push_back(syncer::AUTOFILL_WALLET_DATA);
    datatypes.push_back(syncer::AUTOFILL_WALLET_METADATA);
    datatypes.push_back(syncer::BOOKMARKS);
    datatypes.push_back(syncer::DEVICE_INFO);
    datatypes.push_back(syncer::FAVICON_TRACKING);
    datatypes.push_back(syncer::FAVICON_IMAGES);
    datatypes.push_back(syncer::HISTORY_DELETE_DIRECTIVES);
    datatypes.push_back(syncer::PASSWORDS);
    datatypes.push_back(syncer::PREFERENCES);
    datatypes.push_back(syncer::PRIORITY_PREFERENCES);
    datatypes.push_back(syncer::SESSIONS);
    datatypes.push_back(syncer::PROXY_TABS);
    datatypes.push_back(syncer::SUPERVISED_USER_SETTINGS);
    datatypes.push_back(syncer::SUPERVISED_USER_WHITELISTS);
    datatypes.push_back(syncer::TYPED_URLS);
    datatypes.push_back(syncer::USER_EVENTS);
    datatypes.push_back(syncer::USER_CONSENTS);
    if (base::FeatureList::IsEnabled(switches::kSyncSendTabToSelf)) {
      datatypes.push_back(syncer::SEND_TAB_TO_SELF);
    }
    // TODO(markusheintz): Add security events once it is enabled.
    // datatypes.push_back(syncer::SECURITY_EVENTS);
    return datatypes;
  }

  // Returns the number of default datatypes.
  size_t DefaultDatatypesCount() { return DefaultDatatypes().size(); }

  // Asserts that all the default datatypes are in |types|, except
  // for |exception_types|, which are asserted to not be in |types|.
  void CheckDefaultDatatypesInSetExcept(syncer::ModelTypeSet types,
                                        syncer::ModelTypeSet exception_types) {
    std::vector<syncer::ModelType> defaults = DefaultDatatypes();
    std::vector<syncer::ModelType>::iterator iter;
    for (iter = defaults.begin(); iter != defaults.end(); ++iter) {
      if (exception_types.Has(*iter))
        EXPECT_FALSE(types.Has(*iter))
            << *iter << " found in dataypes map, shouldn't be there.";
      else
        EXPECT_TRUE(types.Has(*iter)) << *iter << " not found in datatypes map";
    }
  }

  void SetDisabledTypes(syncer::ModelTypeSet disabled_types) {
    base::CommandLine::ForCurrentProcess()->AppendSwitchASCII(
        switches::kDisableSyncTypes,
        syncer::ModelTypeSetToString(disabled_types));
  }

  Profile* profile() { return profile_.get(); }

 private:
  content::TestBrowserThreadBundle thread_bundle_;
  std::unique_ptr<TestingProfile> profile_;
};

// Verify that the disable sync flag disables creation of the sync service.
TEST_F(ProfileSyncServiceFactoryTest, DisableSyncFlag) {
  base::CommandLine::ForCurrentProcess()->AppendSwitch(switches::kDisableSync);
  EXPECT_EQ(nullptr, ProfileSyncServiceFactory::GetForProfile(profile()));
}

// Verify that a normal (no command line flags) PSS can be created and
// properly initialized.
TEST_F(ProfileSyncServiceFactoryTest, CreatePSSDefault) {
  syncer::SyncService* pss =
      ProfileSyncServiceFactory::GetForProfile(profile());
  syncer::ModelTypeSet types = pss->GetRegisteredDataTypes();
  EXPECT_EQ(DefaultDatatypesCount(), types.Size());
  CheckDefaultDatatypesInSetExcept(types, syncer::ModelTypeSet());
}

// Verify that a PSS with a disabled datatype can be created and properly
// initialized.
TEST_F(ProfileSyncServiceFactoryTest, CreatePSSDisableOne) {
  syncer::ModelTypeSet disabled_types(syncer::AUTOFILL);
  SetDisabledTypes(disabled_types);
  syncer::SyncService* pss =
      ProfileSyncServiceFactory::GetForProfile(profile());
  syncer::ModelTypeSet types = pss->GetRegisteredDataTypes();
  EXPECT_EQ(DefaultDatatypesCount() - disabled_types.Size(), types.Size());
  CheckDefaultDatatypesInSetExcept(types, disabled_types);
}

// Verify that a PSS with multiple disabled datatypes can be created and
// properly initialized.
TEST_F(ProfileSyncServiceFactoryTest, CreatePSSDisableMultiple) {
  syncer::ModelTypeSet disabled_types(syncer::AUTOFILL_PROFILE,
                                      syncer::BOOKMARKS);
  SetDisabledTypes(disabled_types);
  syncer::SyncService* pss =
      ProfileSyncServiceFactory::GetForProfile(profile());
  syncer::ModelTypeSet types = pss->GetRegisteredDataTypes();
  EXPECT_EQ(DefaultDatatypesCount() - disabled_types.Size(), types.Size());
  CheckDefaultDatatypesInSetExcept(types, disabled_types);
}
