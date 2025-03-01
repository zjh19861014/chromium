// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WEBAUTHN_AUTHENTICATOR_REQUEST_DIALOG_MODEL_H_
#define CHROME_BROWSER_WEBAUTHN_AUTHENTICATOR_REQUEST_DIALOG_MODEL_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/optional.h"
#include "base/strings/string16.h"
#include "base/strings/string_piece.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/webauthn/authenticator_reference.h"
#include "chrome/browser/webauthn/authenticator_transport.h"
#include "chrome/browser/webauthn/observable_authenticator_list.h"
#include "device/fido/fido_request_handler_base.h"
#include "device/fido/fido_transport_protocol.h"

namespace device {
class AuthenticatorGetAssertionResponse;
}

// Encapsulates the model behind the Web Authentication request dialog's UX
// flow. This is essentially a state machine going through the states defined in
// the `Step` enumeration.
//
// Ultimately, this will become an observer of the AuthenticatorRequest, and
// contain the logic to figure out which steps the user needs to take, in which
// order, to complete the authentication flow.
class AuthenticatorRequestDialogModel {
 public:
  using RequestCallback = device::FidoRequestHandlerBase::RequestCallback;
  using BlePairingCallback = device::FidoRequestHandlerBase::BlePairingCallback;
  using BleDevicePairedCallback = base::RepeatingCallback<void(std::string)>;
  using TransportAvailabilityInfo =
      device::FidoRequestHandlerBase::TransportAvailabilityInfo;

  // Defines the potential steps of the Web Authentication API request UX flow.
  enum class Step {
    // The UX flow has not started yet, the dialog should still be hidden.
    kNotStarted,

    kWelcomeScreen,
    kTransportSelection,

    // The request errored out before completing. Error will only be sent
    // after user interaction.
    kErrorNoAvailableTransports,
    kErrorInternalUnrecognized,

    // The request is already complete, but the error dialog should wait
    // until user acknowledgement.
    kTimedOut,
    kKeyNotRegistered,
    kKeyAlreadyRegistered,
    kMissingResidentKeys,
    kMissingUserVerification,

    // The request is completed, and the dialog should be closed.
    kClosed,

    // Universal Serial Bus (USB).
    kUsbInsertAndActivate,

    // Bluetooth Low Energy (BLE).
    kBlePowerOnAutomatic,
    kBlePowerOnManual,

    kBlePairingBegin,
    kBleEnterPairingMode,
    kBleDeviceSelection,
    kBlePinEntry,

    kBleActivate,
    kBleVerifying,

    // Touch ID.
    kTouchIdIncognitoSpeedBump,

    // Phone as a security key.
    kCableActivate,

    // Authenticator Client PIN.
    kClientPinEntry,
    kClientPinSetup,
    kClientPinTapAgain,
    kClientPinErrorSoftBlock,
    kClientPinErrorHardBlock,
    kClientPinErrorAuthenticatorRemoved,

    // Account selection,
    kSelectAccount,

    // Attestation permission request.
    kAttestationPermissionRequest,
  };

  // Implemented by the dialog to observe this model and show the UI panels
  // appropriate for the current step.
  class Observer {
   public:
    // Called just before the model is destructed.
    virtual void OnModelDestroyed() = 0;

    // Called when the UX flow has navigated to a different step, so the UI
    // should update.
    virtual void OnStepTransition() {}

    // Called when the model corresponding to the current sheet of the UX flow
    // was updated, so UI should update.
    virtual void OnSheetModelChanged() {}

    // Called when the power state of the Bluetooth adapter has changed.
    virtual void OnBluetoothPoweredStateChanged() {}

    // Called when the user cancelled WebAuthN request by clicking the
    // "cancel" button or the back arrow in the UI dialog.
    virtual void OnCancelRequest() {}
  };

  AuthenticatorRequestDialogModel();
  ~AuthenticatorRequestDialogModel();

  void SetCurrentStep(Step step);
  Step current_step() const { return current_step_; }

  // Hides the dialog. A subsequent call to SetCurrentStep() will unhide it.
  void HideDialog();

  // Returns whether the UI is in a state at which the |request_| member of
  // AuthenticatorImpl has completed processing. Note that the request callback
  // is only resolved after the UI is dismissed.
  bool is_request_complete() const {
    return current_step() == Step::kTimedOut ||
           current_step() == Step::kKeyNotRegistered ||
           current_step() == Step::kKeyAlreadyRegistered ||
           current_step() == Step::kMissingResidentKeys ||
           current_step() == Step::kMissingUserVerification ||
           current_step() == Step::kClosed;
  }

  bool should_dialog_be_closed() const {
    return current_step() == Step::kClosed;
  }
  bool should_dialog_be_hidden() const {
    return current_step() == Step::kNotStarted;
  }

  const TransportAvailabilityInfo* transport_availability() const {
    return &transport_availability_;
  }

  bool ble_adapter_is_powered() const {
    return transport_availability()->is_ble_powered;
  }

  const base::Optional<std::string>& selected_authenticator_id() const {
    return selected_authenticator_id_;
  }

  // Starts the UX flow, by either showing the welcome screen, the transport
  // selection screen, or the guided flow for them most likely transport.
  //
  // Valid action when at step: kNotStarted.
  void StartFlow(
      TransportAvailabilityInfo transport_availability,
      base::Optional<device::FidoTransportProtocol> last_used_transport,
      const base::ListValue* previously_paired_bluetooth_device_list);

  // Starts the UX flow. Tries to figure out the most likely transport to be
  // used, and starts the guided flow for that transport; or shows the manual
  // transport selection screen if the transport could not be uniquely
  // identified.
  //
  // Valid action when at step: kNotStarted, kWelcomeScreen.
  void StartGuidedFlowForMostLikelyTransportOrShowTransportSelection();

  // Requests that the step-by-step wizard flow commence, guiding the user
  // through using the Secutity Key with the given |transport|.
  //
  // Valid action when at step: kNotStarted, kWelcomeScreen,
  // kTransportSelection, and steps where the other transports menu is shown,
  // namely, kUsbInsertAndActivate, kBleActivate, kCableActivate.
  void StartGuidedFlowForTransport(
      AuthenticatorTransport transport,
      bool pair_with_new_device_for_bluetooth_low_energy = false);

  // Hides the modal Chrome UI dialog and shows the native Windows WebAuthn
  // UI instead.
  void HideDialogAndDispatchToNativeWindowsApi();

  // Ensures that the Bluetooth adapter is powered before proceeding to |step|.
  //  -- If the adapter is powered, advanced directly to |step|.
  //  -- If the adapter is not powered, but Chrome can turn it automatically,
  //     then advanced to the flow to turn on Bluetooth automatically.
  //  -- Otherwise advanced to the manual Bluetooth power on flow.
  //
  // Valid action when at step: kNotStarted, kWelcomeScreen,
  // kTransportSelection, and steps where the other transports menu is shown,
  // namely, kUsbInsertAndActivate, kBleActivate, kCableActivate.
  void EnsureBleAdapterIsPoweredBeforeContinuingWithStep(Step step);

  // Continues with the BLE/caBLE flow now that the Bluetooth adapter is
  // powered.
  //
  // Valid action when at step: kBlePowerOnManual, kBlePowerOnAutomatic.
  void ContinueWithFlowAfterBleAdapterPowered();

  // Turns on the BLE adapter automatically.
  //
  // Valid action when at step: kBlePowerOnAutomatic.
  void PowerOnBleAdapter();

  // Lets the pairing procedure start after the user learned about the need.
  //
  // Valid action when at step: kBlePairingBegin.
  void StartBleDiscovery();

  // Initiates pairing of the device that the user has chosen.
  //
  // Valid action when at step: kBleDeviceSelection.
  void InitiatePairingDevice(base::StringPiece authenticator_id);

  // Finishes pairing of the previously chosen device with the |pin| code
  // entered.
  //
  // Valid action when at step: kBlePinEntry.
  void FinishPairingWithPin(const base::string16& pin);

  // Dispatches WebAuthN request to successfully paired Bluetooth authenticator.
  //
  // Valid action when at step: kBleVerifying.
  void OnPairingSuccess();

  // Returns to Bluetooth device selection modal.
  //
  // Valid action when at step: kBleVerifying.
  void OnPairingFailure();

  // Tries if a USB device is present -- the user claims they plugged it in.
  //
  // Valid action when at step: kUsbInsert.
  void TryUsbDevice();

  // Tries to use Touch ID -- either because the request requires it or because
  // the user told us to. May show an error for unrecognized credential, or an
  // Incognito mode interstitial, or proceed straight to the Touch ID prompt.
  //
  // Valid action when at all steps.
  void StartTouchIdFlow();

  // Proceeds straight to the Touch ID prompt.
  //
  // Valid action when at all steps.
  void HideDialogAndTryTouchId();

  // Cancels the flow as a result of the user clicking `Cancel` on the UI.
  //
  // Valid action at all steps.
  void Cancel();

  // Backtracks in the flow as a result of the user clicking `Back` on the UI.
  //
  // Valid action at all steps.
  void Back();

  // Called by the AuthenticatorRequestSheetModel subclasses when their state
  // changes, which will trigger notifying observers of OnSheetModelChanged.
  void OnSheetModelDidChange();

  // The |observer| must either outlive the object, or unregister itself on its
  // destruction.
  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // To be called when the Web Authentication request is complete.
  void OnRequestComplete();

  // To be called when Web Authentication request times-out.
  void OnRequestTimeout();

  // To be called when the user activates a security key that does not recognize
  // any of the allowed credentials (during a GetAssertion request).
  void OnActivatedKeyNotRegistered();

  // To be called when the user activates a security key that does recognize
  // one of excluded credentials (during a MakeCredential request).
  void OnActivatedKeyAlreadyRegistered();

  // To be called when the selected authenticator cannot currently handle PIN
  // requests because it needs a power-cycle due to too many failures.
  void OnSoftPINBlock();

  // To be called when the selected authenticator must be reset before
  // performing any PIN operations because of too many failures.
  void OnHardPINBlock();

  // To be called when the selected authenticator was removed while
  // waiting for a PIN to be entered.
  void OnAuthenticatorRemovedDuringPINEntry();

  // To be called when the selected authenticator doesn't have the requested
  // resident key capability.
  void OnAuthenticatorMissingResidentKeys();

  // To be called when the selected authenticator doesn't have the requested
  // user verification capability.
  void OnAuthenticatorMissingUserVerification();

  // To be called when the Bluetooth adapter powered state changes.
  void OnBluetoothPoweredStateChanged(bool powered);

  void SetRequestCallback(RequestCallback request_callback);

  void SetBlePairingCallback(BlePairingCallback ble_pairing_callback);

  void SetBluetoothAdapterPowerOnCallback(
      base::RepeatingClosure bluetooth_adapter_power_on_callback);

  void SetBleDevicePairedCallback(
      BleDevicePairedCallback ble_device_paired_callback);

  void SetPINCallback(base::OnceCallback<void(std::string)> pin_callback);

  // OnHavePIN is called when the user enters a PIN in the UI.
  void OnHavePIN(const std::string& pin);

  // OnAttestationPermissionResponse is called when the user either allows or
  // disallows an attestation permission request.
  void OnAttestationPermissionResponse(bool attestation_permission_granted);

  void UpdateAuthenticatorReferenceId(base::StringPiece old_authenticator_id,
                                      std::string new_authenticator_id);
  void AddAuthenticator(const device::FidoAuthenticator& authenticator);
  void RemoveAuthenticator(base::StringPiece authenticator_id);

  void UpdateAuthenticatorReferencePairingMode(
      base::StringPiece authenticator_id,
      bool is_in_pairing_mode);

  // SelectAccount is called to trigger an account selection dialog.
  void SelectAccount(
      std::vector<device::AuthenticatorGetAssertionResponse> responses,
      base::OnceCallback<void(device::AuthenticatorGetAssertionResponse)>
          callback);

  // OnAccountSelected is called when one of the accounts from |SelectAccount|
  // has been picked. |index| is the index of the selected account in
  // |responses()|.
  void OnAccountSelected(size_t index);

  void SetSelectedAuthenticatorForTesting(AuthenticatorReference authenticator);

  ObservableAuthenticatorList& saved_authenticators() {
    return saved_authenticators_;
  }

  const std::vector<AuthenticatorTransport>& available_transports() {
    return available_transports_;
  }

  void CollectPIN(base::Optional<int> attempts,
                  base::OnceCallback<void(std::string)> provide_pin_cb);
  bool has_attempted_pin_entry() const { return has_attempted_pin_entry_; }
  base::Optional<int> pin_attempts() const { return pin_attempts_; }

  void RequestAttestationPermission(base::OnceCallback<void(bool)> callback);

  const std::vector<device::AuthenticatorGetAssertionResponse>& responses() {
    return responses_;
  }

  void set_has_attempted_pin_entry_for_testing() {
    has_attempted_pin_entry_ = true;
  }

  void set_incognito_mode(bool incognito_mode) {
    incognito_mode_ = incognito_mode;
  }

 private:
  void DispatchRequestAsync(AuthenticatorReference* authenticator);
  void DispatchRequestAsyncInternal(const std::string& authenticator_id);

  // The current step of the request UX flow that is currently shown.
  Step current_step_ = Step::kNotStarted;

  // Determines which step to continue with once the Blueooth adapter is
  // powered. Only set while the |current_step_| is either kBlePowerOnManual,
  // kBlePowerOnAutomatic.
  base::Optional<Step> next_step_once_ble_powered_;

  // Determines whether Bluetooth device selection UI and pin pairing UI should
  // be shown. We proceed directly to Step::kBleVerifying if the user has paired
  // with a bluetooth authenticator previously.
  bool previously_paired_with_bluetooth_authenticator_ = false;

  base::ObserverList<Observer>::Unchecked observers_;

  // These fields are only filled out when the UX flow is started.
  TransportAvailabilityInfo transport_availability_;
  std::vector<AuthenticatorTransport> available_transports_;
  base::Optional<device::FidoTransportProtocol> last_used_transport_;

  // Transport type and id of Mac TouchId and BLE authenticators are cached so
  // that the WebAuthN request for the corresponding authenticators can be
  // dispatched lazily after the user interacts with the UI element.
  ObservableAuthenticatorList saved_authenticators_;

  // Represents the id of the Bluetooth authenticator that the user is trying to
  // connect to or conduct WebAuthN request to via the WebAuthN UI.
  base::Optional<std::string> selected_authenticator_id_;

  RequestCallback request_callback_;
  BlePairingCallback ble_pairing_callback_;
  base::RepeatingClosure bluetooth_adapter_power_on_callback_;
  BleDevicePairedCallback ble_device_paired_callback_;

  base::OnceCallback<void(std::string)> pin_callback_;
  bool has_attempted_pin_entry_ = false;
  base::Optional<int> pin_attempts_;

  base::OnceCallback<void(bool)> attestation_callback_;

  // responses_ contains possible accounts to select between.
  std::vector<device::AuthenticatorGetAssertionResponse> responses_;
  base::OnceCallback<void(device::AuthenticatorGetAssertionResponse)>
      selection_callback_;

  bool incognito_mode_ = false;

  base::WeakPtrFactory<AuthenticatorRequestDialogModel> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(AuthenticatorRequestDialogModel);
};

#endif  // CHROME_BROWSER_WEBAUTHN_AUTHENTICATOR_REQUEST_DIALOG_MODEL_H_
