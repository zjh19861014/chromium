// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/oobe_screen.h"

#include <vector>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/strings/string_split.h"
#include "chromeos/constants/chromeos_switches.h"

namespace chromeos {
namespace {

// These get mapped by the Screen enum ordinal values, so this has to be defined
// in the same order as the Screen enum.
const char* kScreenNames[] = {
    "hid-detection",                   // SCREEN_OOBE_HID_DETECTION
    "connect",                         // SCREEN_OOBE_WELCOME
    "network-selection",               // SCREEN_OOBE_NETWORK
    "eula",                            // SCREEN_OOBE_EULA
    "update",                          // SCREEN_OOBE_UPDATE
    "debugging",                       // SCREEN_OOBE_ENABLE_DEBUGGING
    "oauth-enrollment",                // SCREEN_OOBE_ENROLLMENT
    "reset",                           // SCREEN_OOBE_RESET
    "gaia-signin",                     // SCREEN_GAIA_SIGNIN
    "account-picker",                  // SCREEN_ACCOUNT_PICKER
    "autolaunch",                      // SCREEN_KIOSK_AUTOLAUNCH
    "kiosk-enable",                    // SCREEN_KIOSK_ENABLE
    "error-message",                   // SCREEN_ERROR_MESSAGE
    "tpm-error-message",               // SCREEN_TPM_ERROR
    "password-changed",                // SCREEN_PASSWORD_CHANGED
    "supervised-user-creation",        // SCREEN_CREATE_SUPERVISED_USER_FLOW
    "terms-of-service",                // SCREEN_TERMS_OF_SERVICE
    "arc-tos",                         // SCREEN_ARC_TERMS_OF_SERVICE
    "wrong-hwid",                      // SCREEN_WRONG_HWID
    "auto-enrollment-check",           // SCREEN_AUTO_ENROLLMENT_CHECK
    "app-launch-splash",               // SCREEN_APP_LAUNCH_SPLASH
    "arc-kiosk-splash",                // SCREEN_ARC_KIOSK_SPLASH
    "confirm-password",                // SCREEN_CONFIRM_PASSWORD
    "fatal-error",                     // SCREEN_FATAL_ERROR
    "device-disabled",                 // SCREEN_DEVICE_DISABLED
    "unrecoverable-cryptohome-error",  // SCREEN_UNRECOVERABLE_CRYPTOHOME_ERROR
    "userBoard",                       // SCREEN_USER_SELECTION
    "ad-password-change",            // SCREEN_ACTIVE_DIRECTORY_PASSWORD_CHANGE
    "encryption-migration",          // SCREEN_ENCRYPTION_MIGRATION
    "supervision-transition",        // SCREEN_SUPERVISION_TRANSITION
    "update-required",               // SCREEN_UPDATE_REQUIRED
    "assistant-optin-flow",          // SCREEN_ASSISTANT_OPTIN_FLOW
    "login",                         // SCREEN_SPECIAL_LOGIN
    "oobe",                          // SCREEN_SPECIAL_OOBE
    "test:nowindow",                 // SCREEN_TEST_NO_WINDOW
    "sync-consent",                  // SCREEN_SYNC_CONSENT
    "fingerprint-setup",             // SCREEN_FINGERPRINT_SETUP
    "demo-setup",                    // SCREEN_OOBE_DEMO_SETUP
    "demo-preferences",              // SCREEN_OOBE_DEMO_PREFERENCES
    "recommend-apps",                // SCREEN_RECOMMEND_APPS
    "app-downloading",               // SCREEN_APP_DOWNLOADING
    "discover",                      // SCREEN_DISCOVER
    "marketing-opt-in",              // SCREEN_MARKETING_OPT_IN
    "multidevice-setup",             // SCREEN_MULTIDEVICE_SETUP
    "unknown",                       // SCREEN_UNKNOWN
};

static_assert(static_cast<size_t>(OobeScreen::SCREEN_UNKNOWN) ==
                  base::size(kScreenNames) - 1,
              "Missing element in OobeScreen or kScreenNames");

}  // namespace

std::string GetOobeScreenName(OobeScreen screen) {
  DCHECK(screen <= OobeScreen::SCREEN_UNKNOWN);
  return kScreenNames[static_cast<size_t>(screen)];
}

OobeScreen GetOobeScreenFromName(const std::string& name) {
  for (size_t i = 0; i < base::size(kScreenNames); ++i) {
    if (name == kScreenNames[i])
      return static_cast<OobeScreen>(i);
  }

  return OobeScreen::SCREEN_UNKNOWN;
}

}  // namespace chromeos
