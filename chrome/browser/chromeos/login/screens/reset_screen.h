// Copyright (c) 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_LOGIN_SCREENS_RESET_SCREEN_H_
#define CHROME_BROWSER_CHROMEOS_LOGIN_SCREENS_RESET_SCREEN_H_

#include <set>
#include <string>

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/chromeos/login/help_app_launcher.h"
#include "chrome/browser/chromeos/login/screens/base_screen.h"
#include "chrome/browser/chromeos/tpm_firmware_update.h"
#include "chromeos/dbus/update_engine_client.h"

class PrefRegistrySimple;

namespace chromeos {

class BaseScreenDelegate;
class ResetView;

// Representation independent class that controls screen showing reset to users.
// It run exit callback only if the user cancels the reset. Other user actions
// will end up in the device restart.
class ResetScreen : public BaseScreen, public UpdateEngineClient::Observer {
 public:
  ResetScreen(BaseScreenDelegate* base_screen_delegate,
              ResetView* view,
              const base::RepeatingClosure& exit_callback);
  ~ResetScreen() override;

  // Called when view is destroyed so there's no dead reference to it.
  void OnViewDestroyed(ResetView* view);

  // Registers Local State preferences.
  static void RegisterPrefs(PrefRegistrySimple* registry);

  using TpmFirmwareUpdateAvailabilityCallback = base::OnceCallback<void(
      const std::set<tpm_firmware_update::Mode>& modes)>;
  using TpmFirmwareUpdateAvailabilityChecker = base::RepeatingCallback<void(
      TpmFirmwareUpdateAvailabilityCallback callback,
      base::TimeDelta delay)>;
  // Overrides the method used to determine TPM firmware update availability.
  // It should be called before the ResetScreen is created, otherwise it will
  // have no effect.
  static void SetTpmFirmwareUpdateCheckerForTesting(
      TpmFirmwareUpdateAvailabilityChecker* checker);

 private:
  // BaseScreen implementation:
  void Show() override;
  void Hide() override;
  void OnUserAction(const std::string& action_id) override;

  // UpdateEngineClient::Observer implementation:
  void UpdateStatusChanged(const UpdateEngineClient::Status& status) override;

  void OnRollbackCheck(bool can_rollback);
  void OnTPMFirmwareUpdateAvailableCheck(
      const std::set<tpm_firmware_update::Mode>& modes);

  void OnCancel();
  void OnPowerwash();
  void OnRestart();
  void OnToggleRollback();
  void OnShowConfirm();
  void OnConfirmationDismissed();

  void ShowHelpArticle(HelpAppLauncher::HelpTopic topic);

  BaseScreenDelegate* const base_screen_delegate_;
  ResetView* view_;
  base::RepeatingClosure exit_callback_;

  // Help application used for help dialogs.
  scoped_refptr<HelpAppLauncher> help_app_;

  // Callback used to check whether a TPM firnware update is available.
  TpmFirmwareUpdateAvailabilityChecker tpm_firmware_update_checker_;

  base::WeakPtrFactory<ResetScreen> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(ResetScreen);
};

}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_LOGIN_SCREENS_RESET_SCREEN_H_
