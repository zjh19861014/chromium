// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/fido/virtual_ctap2_device.h"

#include <array>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/cbor/reader.h"
#include "components/cbor/writer.h"
#include "crypto/ec_private_key.h"
#include "device/fido/authenticator_get_assertion_response.h"
#include "device/fido/authenticator_make_credential_response.h"
#include "device/fido/ctap_get_assertion_request.h"
#include "device/fido/ctap_make_credential_request.h"
#include "device/fido/ec_public_key.h"
#include "device/fido/fido_constants.h"
#include "device/fido/fido_parsing_utils.h"
#include "device/fido/opaque_attestation_statement.h"
#include "device/fido/pin.h"
#include "device/fido/pin_internal.h"
#include "device/fido/virtual_u2f_device.h"
#include "third_party/boringssl/src/include/openssl/aes.h"
#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/ec_key.h"
#include "third_party/boringssl/src/include/openssl/hmac.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/obj.h"
#include "third_party/boringssl/src/include/openssl/rand.h"
#include "third_party/boringssl/src/include/openssl/sha.h"

namespace device {

namespace {

constexpr std::array<uint8_t, kAaguidLength> kDeviceAaguid = {
    {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x02, 0x03, 0x04,
     0x05, 0x06, 0x07, 0x08}};

std::vector<uint8_t> ConstructResponse(CtapDeviceResponseCode response_code,
                                       base::span<const uint8_t> data) {
  std::vector<uint8_t> response{base::strict_cast<uint8_t>(response_code)};
  fido_parsing_utils::Append(&response, data);
  return response;
}

void ReturnCtap2Response(
    FidoDevice::DeviceCallback cb,
    CtapDeviceResponseCode response_code,
    base::Optional<base::span<const uint8_t>> data = base::nullopt) {
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(cb),
                     ConstructResponse(response_code,
                                       data.value_or(std::vector<uint8_t>{}))));
}

// CheckPINToken returns true iff |pin_auth| is a valid authentication of
// |client_data_hash| given that the PIN token in effect is |pin_token|.
bool CheckPINToken(base::span<const uint8_t> pin_token,
                   base::span<const uint8_t> pin_auth,
                   base::span<const uint8_t> client_data_hash) {
  uint8_t calculated_pin_auth[SHA256_DIGEST_LENGTH];
  unsigned hmac_bytes;
  CHECK(HMAC(EVP_sha256(), pin_token.data(), pin_token.size(),
             client_data_hash.data(), client_data_hash.size(),
             calculated_pin_auth, &hmac_bytes));
  DCHECK_EQ(sizeof(calculated_pin_auth), static_cast<size_t>(hmac_bytes));

  return pin_auth.size() == 16 &&
         CRYPTO_memcmp(pin_auth.data(), calculated_pin_auth, 16) == 0;
}

// CheckUserVerification implements the first, common steps of
// makeCredential and getAssertion from the CTAP2 spec.
CtapDeviceResponseCode CheckUserVerification(
    bool is_make_credential,
    const AuthenticatorSupportedOptions& options,
    const base::Optional<std::vector<uint8_t>>& pin_auth,
    const base::Optional<uint8_t>& pin_protocol,
    base::span<const uint8_t> pin_token,
    base::span<const uint8_t> client_data_hash,
    UserVerificationRequirement user_verification,
    base::RepeatingCallback<void(void)> simulate_press_callback,
    bool* out_user_verified) {
  // The following quotes are from the CTAP2 spec:

  // 1. "If authenticator supports clientPin and platform sends a zero length
  // pinAuth, wait for user touch and then return either CTAP2_ERR_PIN_NOT_SET
  // if pin is not set or CTAP2_ERR_PIN_INVALID if pin has been set."
  const bool supports_pin =
      options.client_pin_availability !=
      AuthenticatorSupportedOptions::ClientPinAvailability::kNotSupported;
  if (supports_pin && pin_auth && pin_auth->empty()) {
    if (simulate_press_callback) {
      simulate_press_callback.Run();
    }
    switch (options.client_pin_availability) {
      case AuthenticatorSupportedOptions::ClientPinAvailability::
          kSupportedAndPinSet:
        return CtapDeviceResponseCode::kCtap2ErrPinInvalid;
      case AuthenticatorSupportedOptions::ClientPinAvailability::
          kSupportedButPinNotSet:
        return CtapDeviceResponseCode::kCtap2ErrPinNotSet;
      case AuthenticatorSupportedOptions::ClientPinAvailability::kNotSupported:
        NOTREACHED();
    }
  }

  // 2. "If authenticator supports clientPin and pinAuth parameter is present
  // and the pinProtocol is not supported, return CTAP2_ERR_PIN_AUTH_INVALID
  // error."
  if (supports_pin && pin_auth && (!pin_protocol || *pin_protocol != 1)) {
    return CtapDeviceResponseCode::kCtap2ErrPinAuthInvalid;
  }

  // 3. "If authenticator is not protected by some form of user verification and
  // platform has set "uv" or pinAuth to get the user verification, return
  // CTAP2_ERR_INVALID_OPTION."
  const bool can_do_uv =
      options.user_verification_availability ==
          AuthenticatorSupportedOptions::UserVerificationAvailability::
              kSupportedAndConfigured ||
      options.client_pin_availability ==
          AuthenticatorSupportedOptions::ClientPinAvailability::
              kSupportedAndPinSet;
  if (!can_do_uv &&
      (user_verification == UserVerificationRequirement::kRequired ||
       pin_auth)) {
    return CtapDeviceResponseCode::kCtap2ErrInvalidOption;
  }

  // Step 4.
  bool uv = false;
  if (can_do_uv) {
    if (user_verification == UserVerificationRequirement::kRequired) {
      if (options.user_verification_availability ==
          AuthenticatorSupportedOptions::UserVerificationAvailability::
              kSupportedAndConfigured) {
        // Internal UV is assumed to always succeed.
        if (simulate_press_callback) {
          simulate_press_callback.Run();
        }
        uv = true;
      } else {
        // UV was requested, but either not supported or not configured.
        return CtapDeviceResponseCode::kCtap2ErrPinAuthInvalid;
      }
    }

    if (pin_auth && options.client_pin_availability ==
                        AuthenticatorSupportedOptions::ClientPinAvailability::
                            kSupportedAndPinSet) {
      DCHECK(pin_protocol && *pin_protocol == 1);
      if (CheckPINToken(pin_token, *pin_auth, client_data_hash)) {
        uv = true;
      } else {
        return CtapDeviceResponseCode::kCtap2ErrPinAuthInvalid;
      }
    }

    if (is_make_credential && !uv) {
      return CtapDeviceResponseCode::kCtap2ErrPinRequired;
    }
  }

  *out_user_verified = uv;
  return CtapDeviceResponseCode::kSuccess;
}

// Checks that whether the received MakeCredential request includes EA256
// algorithm in publicKeyCredParam.
bool AreMakeCredentialParamsValid(const CtapMakeCredentialRequest& request) {
  const auto& params =
      request.public_key_credential_params().public_key_credential_params();
  return std::any_of(
      params.begin(), params.end(), [](const auto& credential_info) {
        return credential_info.algorithm ==
               base::strict_cast<int>(CoseAlgorithmIdentifier::kCoseEs256);
      });
}

std::unique_ptr<ECPublicKey> ConstructECPublicKey(
    std::string public_key_string) {
  DCHECK_EQ(64u, public_key_string.size());

  const auto public_key_x_coordinate =
      base::as_bytes(base::make_span(public_key_string)).first(32);
  const auto public_key_y_coordinate =
      base::as_bytes(base::make_span(public_key_string)).last(32);
  return std::make_unique<ECPublicKey>(
      fido_parsing_utils::kEs256,
      fido_parsing_utils::Materialize(public_key_x_coordinate),
      fido_parsing_utils::Materialize(public_key_y_coordinate));
}

std::vector<uint8_t> ConstructSignatureBuffer(
    const AuthenticatorData& authenticator_data,
    base::span<const uint8_t, kClientDataHashLength> client_data_hash) {
  std::vector<uint8_t> signature_buffer;
  fido_parsing_utils::Append(&signature_buffer,
                             authenticator_data.SerializeToByteArray());
  fido_parsing_utils::Append(&signature_buffer, client_data_hash);
  return signature_buffer;
}

std::vector<uint8_t> ConstructMakeCredentialResponse(
    const base::Optional<std::vector<uint8_t>> attestation_certificate,
    base::span<const uint8_t> signature,
    AuthenticatorData authenticator_data) {
  cbor::Value::MapValue attestation_map;
  attestation_map.emplace("alg", -7);
  attestation_map.emplace("sig", fido_parsing_utils::Materialize(signature));

  if (attestation_certificate) {
    cbor::Value::ArrayValue certificate_chain;
    certificate_chain.emplace_back(std::move(*attestation_certificate));
    attestation_map.emplace("x5c", std::move(certificate_chain));
  }

  AuthenticatorMakeCredentialResponse make_credential_response(
      FidoTransportProtocol::kUsbHumanInterfaceDevice,
      AttestationObject(
          std::move(authenticator_data),
          std::make_unique<OpaqueAttestationStatement>(
              "packed", cbor::Value(std::move(attestation_map)))));
  return GetSerializedCtapDeviceResponse(make_credential_response);
}

bool IsMakeCredentialOptionMapFormatCorrect(
    const cbor::Value::MapValue& option_map) {
  return std::all_of(
      option_map.begin(), option_map.end(), [](const auto& param) {
        if (!param.first.is_string())
          return false;

        const auto& key = param.first.GetString();
        return ((key == kResidentKeyMapKey || key == kUserVerificationMapKey) &&
                param.second.is_bool());
      });
}

bool AreMakeCredentialRequestMapKeysCorrect(
    const cbor::Value::MapValue& request_map) {
  return std::all_of(request_map.begin(), request_map.end(),
                     [](const auto& param) {
                       if (!param.first.is_integer())
                         return false;

                       const auto& key = param.first.GetInteger();
                       return (key <= 9u && key >= 1u);
                     });
}

bool IsGetAssertionOptionMapFormatCorrect(
    const cbor::Value::MapValue& option_map) {
  return std::all_of(
      option_map.begin(), option_map.end(), [](const auto& param) {
        if (!param.first.is_string())
          return false;

        const auto& key = param.first.GetString();
        return (key == kUserPresenceMapKey || key == kUserVerificationMapKey) &&
               param.second.is_bool();
      });
}

bool AreGetAssertionRequestMapKeysCorrect(
    const cbor::Value::MapValue& request_map) {
  return std::all_of(request_map.begin(), request_map.end(),
                     [](const auto& param) {
                       if (!param.first.is_integer())
                         return false;

                       const auto& key = param.first.GetInteger();
                       return (key <= 7u || key >= 1u);
                     });
}

base::Optional<std::vector<uint8_t>> GetPINBytestring(
    const cbor::Value::MapValue& request,
    pin::RequestKey key) {
  const auto it = request.find(cbor::Value(static_cast<int>(key)));
  if (it == request.end() || !it->second.is_bytestring()) {
    return base::nullopt;
  }
  return it->second.GetBytestring();
}

base::Optional<bssl::UniquePtr<EC_POINT>> GetPINKey(
    const cbor::Value::MapValue& request,
    pin::RequestKey map_key) {
  const auto it = request.find(cbor::Value(static_cast<int>(map_key)));
  if (it == request.end() || !it->second.is_map()) {
    return base::nullopt;
  }
  const auto& cose_key = it->second.GetMap();
  auto response = pin::KeyAgreementResponse::ParseFromCOSE(cose_key);
  if (!response) {
    return base::nullopt;
  }

  bssl::UniquePtr<EC_GROUP> group(
      EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
  return pin::PointFromKeyAgreementResponse(group.get(), *response).value();
}

// ConfirmPresentedPIN checks whether |encrypted_pin_hash| is a valid proof-of-
// possession of the PIN, given that |shared_key| is the result of the ECDH key
// agreement.
CtapDeviceResponseCode ConfirmPresentedPIN(
    VirtualCtap2Device::State* state,
    const uint8_t shared_key[SHA256_DIGEST_LENGTH],
    const std::vector<uint8_t>& encrypted_pin_hash) {
  if (state->retries == 0) {
    return CtapDeviceResponseCode::kCtap2ErrPinBlocked;
  }
  if (state->soft_locked) {
    return CtapDeviceResponseCode::kCtap2ErrPinAuthBlocked;
  }

  state->retries--;
  state->retries_since_insertion++;

  DCHECK((encrypted_pin_hash.size() % AES_BLOCK_SIZE) == 0);
  uint8_t pin_hash[AES_BLOCK_SIZE];
  pin::Decrypt(shared_key, encrypted_pin_hash, pin_hash);

  uint8_t calculated_pin_hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(state->pin.data()), state->pin.size(),
         calculated_pin_hash);

  if (state->pin.empty() ||
      CRYPTO_memcmp(pin_hash, calculated_pin_hash, sizeof(pin_hash)) != 0) {
    if (state->retries == 0) {
      return CtapDeviceResponseCode::kCtap2ErrPinBlocked;
    }
    if (state->retries_since_insertion == 3) {
      state->soft_locked = true;
      return CtapDeviceResponseCode::kCtap2ErrPinAuthBlocked;
    }
    return CtapDeviceResponseCode::kCtap2ErrPinInvalid;
  }

  state->retries = 8;
  state->retries_since_insertion = 0;

  return CtapDeviceResponseCode::kSuccess;
}

// SetPIN sets the current PIN based on the ciphertext in |encrypted_pin|, given
// that |shared_key| is the result of the ECDH key agreement.
CtapDeviceResponseCode SetPIN(VirtualCtap2Device::State* state,
                              const uint8_t shared_key[SHA256_DIGEST_LENGTH],
                              const std::vector<uint8_t>& encrypted_pin,
                              const std::vector<uint8_t>& pin_auth) {
  // See
  // https://fidoalliance.org/specs/fido-v2.0-rd-20180702/fido-client-to-authenticator-protocol-v2.0-rd-20180702.html#settingNewPin
  uint8_t calculated_pin_auth[SHA256_DIGEST_LENGTH];
  unsigned hmac_bytes;
  CHECK(HMAC(EVP_sha256(), shared_key, SHA256_DIGEST_LENGTH,
             encrypted_pin.data(), encrypted_pin.size(), calculated_pin_auth,
             &hmac_bytes));
  DCHECK_EQ(sizeof(calculated_pin_auth), static_cast<size_t>(hmac_bytes));

  if (pin_auth.size() != sizeof(calculated_pin_auth) ||
      CRYPTO_memcmp(calculated_pin_auth, pin_auth.data(),
                    sizeof(calculated_pin_auth)) != 0) {
    return CtapDeviceResponseCode::kCtap2ErrPinAuthInvalid;
  }

  if (encrypted_pin.size() < 64) {
    return CtapDeviceResponseCode::kCtap2ErrPinPolicyViolation;
  }

  std::vector<uint8_t> plaintext_pin(encrypted_pin.size());
  pin::Decrypt(shared_key, encrypted_pin, plaintext_pin.data());

  size_t padding_len = 0;
  while (padding_len < plaintext_pin.size() &&
         plaintext_pin[plaintext_pin.size() - padding_len - 1] == 0) {
    padding_len++;
  }

  plaintext_pin.resize(plaintext_pin.size() - padding_len);
  if (padding_len == 0 || plaintext_pin.size() < 4 ||
      plaintext_pin.size() > 63) {
    return CtapDeviceResponseCode::kCtap2ErrPinPolicyViolation;
  }

  state->pin = std::string(reinterpret_cast<const char*>(plaintext_pin.data()),
                           plaintext_pin.size());
  state->retries = 8;

  return CtapDeviceResponseCode::kSuccess;
}

}  // namespace

VirtualCtap2Device::Config::Config() = default;

VirtualCtap2Device::VirtualCtap2Device()
    : VirtualFidoDevice(), weak_factory_(this) {
  device_info_ =
      AuthenticatorGetInfoResponse({ProtocolVersion::kCtap}, kDeviceAaguid);
}

VirtualCtap2Device::VirtualCtap2Device(scoped_refptr<State> state,
                                       const Config& config)
    : VirtualFidoDevice(std::move(state)),
      config_(config),
      weak_factory_(this) {
  std::vector<ProtocolVersion> versions = {ProtocolVersion::kCtap};
  if (config.u2f_support) {
    versions.emplace_back(ProtocolVersion::kU2f);
    u2f_device_.reset(new VirtualU2fDevice(NewReferenceToState()));
  }
  device_info_ =
      AuthenticatorGetInfoResponse(std::move(versions), kDeviceAaguid);

  AuthenticatorSupportedOptions options;
  bool options_updated = false;
  if (config.pin_support) {
    options_updated = true;

    if (mutable_state()->pin.empty()) {
      options.client_pin_availability = AuthenticatorSupportedOptions::
          ClientPinAvailability::kSupportedButPinNotSet;
    } else {
      options.client_pin_availability = AuthenticatorSupportedOptions::
          ClientPinAvailability::kSupportedAndPinSet;
    }
  }

  if (config.internal_uv_support) {
    options_updated = true;
    if (mutable_state()->fingerprints_enrolled) {
      options.user_verification_availability = AuthenticatorSupportedOptions::
          UserVerificationAvailability::kSupportedAndConfigured;
    } else {
      options.user_verification_availability = AuthenticatorSupportedOptions::
          UserVerificationAvailability::kSupportedButNotConfigured;
    }
  }

  if (config.resident_key_support) {
    options_updated = true;
    options.supports_resident_key = true;
  }

  if (options_updated) {
    device_info_->SetOptions(options);
  }
}

VirtualCtap2Device::~VirtualCtap2Device() = default;

// As all operations for VirtualCtap2Device are synchronous and we do not wait
// for user touch, Cancel command is no-op.
void VirtualCtap2Device::Cancel(CancelToken) {}

FidoDevice::CancelToken VirtualCtap2Device::DeviceTransact(
    std::vector<uint8_t> command,
    DeviceCallback cb) {
  if (command.empty()) {
    ReturnCtap2Response(std::move(cb), CtapDeviceResponseCode::kCtap2ErrOther);
    return 0;
  }

  auto cmd_type = command[0];
  // The CTAP2 commands start at one, so a "command" of zero indicates that this
  // is a U2F message.
  if (cmd_type == 0 && config_.u2f_support) {
    u2f_device_->DeviceTransact(std::move(command), std::move(cb));
    return 0;
  }

  const auto request_bytes = base::make_span(command).subspan(1);
  CtapDeviceResponseCode response_code = CtapDeviceResponseCode::kCtap2ErrOther;
  std::vector<uint8_t> response_data;

  switch (static_cast<CtapRequestCommand>(cmd_type)) {
    case CtapRequestCommand::kAuthenticatorGetInfo:
      if (!request_bytes.empty()) {
        ReturnCtap2Response(std::move(cb),
                            CtapDeviceResponseCode::kCtap2ErrOther);
        return 0;
      }

      response_code = OnAuthenticatorGetInfo(&response_data);
      break;
    case CtapRequestCommand::kAuthenticatorMakeCredential:
      response_code = OnMakeCredential(request_bytes, &response_data);
      break;
    case CtapRequestCommand::kAuthenticatorGetAssertion:
      response_code = OnGetAssertion(request_bytes, &response_data);
      break;
    case CtapRequestCommand::kAuthenticatorGetNextAssertion:
      response_code = OnGetNextAssertion(request_bytes, &response_data);
      break;
    case CtapRequestCommand::kAuthenticatorClientPin:
      response_code = OnPINCommand(request_bytes, &response_data);
      break;
    default:
      break;
  }

  // Call |callback| via the |MessageLoop| because |AuthenticatorImpl| doesn't
  // support callback hairpinning.
  ReturnCtap2Response(std::move(cb), response_code, std::move(response_data));
  return 0;
}

base::WeakPtr<FidoDevice> VirtualCtap2Device::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void VirtualCtap2Device::SetAuthenticatorSupportedOptions(
    const AuthenticatorSupportedOptions& options) {
  device_info_->SetOptions(options);
}

CtapDeviceResponseCode VirtualCtap2Device::OnMakeCredential(
    base::span<const uint8_t> request_bytes,
    std::vector<uint8_t>* response) {
  auto request_and_hash = ParseCtapMakeCredentialRequest(request_bytes);
  if (!request_and_hash) {
    DLOG(ERROR) << "Incorrectly formatted MakeCredential request.";
    return CtapDeviceResponseCode::kCtap2ErrOther;
  }
  CtapMakeCredentialRequest request = std::get<0>(*request_and_hash);
  CtapMakeCredentialRequest::ClientDataHash client_data_hash =
      std::get<1>(*request_and_hash);
  const AuthenticatorSupportedOptions& options = device_info_->options();

  bool user_verified;
  const CtapDeviceResponseCode uv_error = CheckUserVerification(
      true /* is makeCredential */, options, request.pin_auth(),
      request.pin_protocol(), mutable_state()->pin_token, client_data_hash,
      request.user_verification(), mutable_state()->simulate_press_callback,
      &user_verified);
  if (uv_error != CtapDeviceResponseCode::kSuccess) {
    return uv_error;
  }

  // 6. Check for already registered credentials.
  const auto rp_id_hash =
      fido_parsing_utils::CreateSHA256Hash(request.rp().rp_id());
  if (request.exclude_list()) {
    if (config_.reject_large_allow_and_exclude_lists &&
        request.exclude_list()->size() > 1) {
      return CtapDeviceResponseCode::kCtap2ErrLimitExceeded;
    }

    for (const auto& excluded_credential : *request.exclude_list()) {
      if (FindRegistrationData(excluded_credential.id(), rp_id_hash)) {
        if (mutable_state()->simulate_press_callback) {
          mutable_state()->simulate_press_callback.Run();
        }
        return CtapDeviceResponseCode::kCtap2ErrCredentialExcluded;
      }
    }
  }

  // Step 7.
  if (!AreMakeCredentialParamsValid(request)) {
    DLOG(ERROR) << "Virtual CTAP2 device does not support options required by "
                   "the request.";
    return CtapDeviceResponseCode::kCtap2ErrUnsupportedAlgorithm;
  }

  // Step 8.
  if ((request.resident_key_required() && !options.supports_resident_key) ||
      !options.supports_user_presence) {
    return CtapDeviceResponseCode::kCtap2ErrUnsupportedOption;
  }

  // Step 10.
  if (!user_verified && mutable_state()->simulate_press_callback) {
    mutable_state()->simulate_press_callback.Run();
  }

  // Create key to register.
  // Note: Non-deterministic, you need to mock this out if you rely on
  // deterministic behavior.
  auto private_key = crypto::ECPrivateKey::Create();
  std::string public_key;
  bool status = private_key->ExportRawPublicKey(&public_key);
  DCHECK(status);

  // Our key handles are simple hashes of the public key.
  auto hash = fido_parsing_utils::CreateSHA256Hash(public_key);
  std::vector<uint8_t> key_handle(hash.begin(), hash.end());

  base::Optional<cbor::Value> extensions;
  if (request.hmac_secret()) {
    cbor::Value::MapValue extensions_map;
    extensions_map.emplace(cbor::Value(kExtensionHmacSecret),
                           cbor::Value(true));
    extensions = cbor::Value(std::move(extensions_map));
  }

  auto authenticator_data = ConstructAuthenticatorData(
      rp_id_hash, user_verified, 01ul,
      ConstructAttestedCredentialData(key_handle,
                                      ConstructECPublicKey(public_key)),
      std::move(extensions));
  auto sign_buffer =
      ConstructSignatureBuffer(authenticator_data, client_data_hash);

  // Sign with attestation key.
  // Note: Non-deterministic, you need to mock this out if you rely on
  // deterministic behavior.
  std::vector<uint8_t> sig;
  std::unique_ptr<crypto::ECPrivateKey> attestation_private_key =
      crypto::ECPrivateKey::CreateFromPrivateKeyInfo(GetAttestationKey());
  status = Sign(attestation_private_key.get(), std::move(sign_buffer), &sig);
  DCHECK(status);

  base::Optional<std::vector<uint8_t>> attestation_cert;
  if (!mutable_state()->self_attestation) {
    attestation_cert = GenerateAttestationCertificate(
        false /* individual_attestation_requested */);
    if (!attestation_cert) {
      DLOG(ERROR) << "Failed to generate attestation certificate.";
      return CtapDeviceResponseCode::kCtap2ErrOther;
    }
  }

  *response = ConstructMakeCredentialResponse(std::move(attestation_cert), sig,
                                              std::move(authenticator_data));
  RegistrationData registration(std::move(private_key), rp_id_hash,
                                1 /* signature counter */);

  if (request.resident_key_required()) {
    // If there's already a registration for this RP and user ID, delete it.
    for (const auto& registration : mutable_state()->registrations) {
      if (registration.second.is_resident &&
          rp_id_hash == registration.second.application_parameter &&
          registration.second.user->user_id() == request.user().user_id()) {
        mutable_state()->registrations.erase(registration.first);
        break;
      }
    }

    size_t num_resident_keys = 0;
    for (const auto& registration : mutable_state()->registrations) {
      if (registration.second.is_resident) {
        num_resident_keys++;
      }
    }

    if (num_resident_keys >= config_.resident_credential_storage) {
      return CtapDeviceResponseCode::kCtap2ErrKeyStoreFull;
    }

    registration.is_resident = true;
    registration.user = request.user();
  }

  StoreNewKey(key_handle, std::move(registration));
  return CtapDeviceResponseCode::kSuccess;
}

CtapDeviceResponseCode VirtualCtap2Device::OnGetAssertion(
    base::span<const uint8_t> request_bytes,
    std::vector<uint8_t>* response) {
  // Step numbers in this function refer to
  // https://fidoalliance.org/specs/fido-v2.0-ps-20190130/fido-client-to-authenticator-protocol-v2.0-ps-20190130.html#authenticatorGetAssertion
  auto request_and_hash = ParseCtapGetAssertionRequest(request_bytes);
  if (!request_and_hash) {
    DLOG(ERROR) << "Incorrectly formatted GetAssertion request.";
    return CtapDeviceResponseCode::kCtap2ErrOther;
  }
  CtapGetAssertionRequest request = std::get<0>(*request_and_hash);
  CtapGetAssertionRequest::ClientDataHash client_data_hash =
      std::get<1>(*request_and_hash);
  const AuthenticatorSupportedOptions& options = device_info_->options();

  bool user_verified;
  const CtapDeviceResponseCode uv_error = CheckUserVerification(
      false /* not makeCredential */, options, request.pin_auth(),
      request.pin_protocol(), mutable_state()->pin_token, client_data_hash,
      request.user_verification(), mutable_state()->simulate_press_callback,
      &user_verified);
  if (uv_error != CtapDeviceResponseCode::kSuccess) {
    return uv_error;
  }

  // Resident keys are not supported.
  if (!config_.resident_key_support &&
      (!request.allow_list() || request.allow_list()->empty())) {
    return CtapDeviceResponseCode::kCtap2ErrNoCredentials;
  }

  const auto rp_id_hash = fido_parsing_utils::CreateSHA256Hash(request.rp_id());

  std::vector<std::pair<base::span<const uint8_t>, RegistrationData*>>
      found_registrations;

  if (request.allow_list()) {
    if (config_.reject_large_allow_and_exclude_lists &&
        request.allow_list()->size() > 1) {
      return CtapDeviceResponseCode::kCtap2ErrLimitExceeded;
    }

    // An empty allow_list could be considered to be a resident-key request, but
    // some authenticators in practice don't take it that way. Thus this code
    // mirrors that to better reflect reality. CTAP 2.0 leaves it as undefined
    // behaviour.
    for (const auto& allowed_credential : *request.allow_list()) {
      RegistrationData* found =
          FindRegistrationData(allowed_credential.id(), rp_id_hash);
      if (found) {
        found_registrations.emplace_back(allowed_credential.id(), found);
        break;
      }
    }
  } else {
    DCHECK(config_.resident_key_support);
    for (auto& registration : mutable_state()->registrations) {
      if (registration.second.is_resident &&
          registration.second.application_parameter == rp_id_hash) {
        found_registrations.emplace_back(registration.first,
                                         &registration.second);
      }
    }
  }

  if (config_.return_immediate_invalid_credential_error &&
      found_registrations.empty()) {
    return CtapDeviceResponseCode::kCtap2ErrInvalidCredential;
  }

  // Step 5.
  if (!options.supports_user_presence && request.user_presence_required()) {
    return CtapDeviceResponseCode::kCtap2ErrUnsupportedOption;
  }

  // Step 7.
  if (request.user_presence_required() && !user_verified &&
      mutable_state()->simulate_press_callback) {
    mutable_state()->simulate_press_callback.Run();
  }

  // Step 8.
  if (found_registrations.empty()) {
    return CtapDeviceResponseCode::kCtap2ErrNoCredentials;
  }

  // This implementation does not sort credentials by creation time as the spec
  // requires.

  mutable_state()->pending_assertions.clear();
  bool done_first = false;
  for (const auto& registration : found_registrations) {
    registration.second->counter++;

    auto* private_key = registration.second->private_key.get();
    std::string public_key;
    bool status = private_key->ExportRawPublicKey(&public_key);
    DCHECK(status);

    base::Optional<AttestedCredentialData> opt_attested_cred_data =
        config_.return_attested_cred_data_in_get_assertion_response
            ? base::make_optional(ConstructAttestedCredentialData(
                  fido_parsing_utils::Materialize(registration.first),
                  ConstructECPublicKey(public_key)))
            : base::nullopt;

    auto authenticator_data = ConstructAuthenticatorData(
        rp_id_hash, user_verified, registration.second->counter,
        std::move(opt_attested_cred_data), base::nullopt);
    auto signature_buffer =
        ConstructSignatureBuffer(authenticator_data, client_data_hash);

    std::vector<uint8_t> signature;
    status = Sign(private_key, std::move(signature_buffer), &signature);
    DCHECK(status);

    AuthenticatorGetAssertionResponse assertion(
        std::move(authenticator_data),
        fido_parsing_utils::Materialize(signature));

    assertion.SetCredential(
        {CredentialType::kPublicKey,
         fido_parsing_utils::Materialize(registration.first)});
    if (registration.second->is_resident) {
      assertion.SetUserEntity(registration.second->user.value());
    }

    if (!done_first) {
      if (found_registrations.size() > 1) {
        DCHECK_LT(found_registrations.size(), 256u);
        assertion.SetNumCredentials(found_registrations.size());
      }
      *response = GetSerializedCtapDeviceResponse(assertion);
      done_first = true;
    } else {
      // These replies will be returned in response to a GetNextAssertion
      // request.
      mutable_state()->pending_assertions.emplace_back(
          GetSerializedCtapDeviceResponse(assertion));
    }
  }

  return CtapDeviceResponseCode::kSuccess;
}

CtapDeviceResponseCode VirtualCtap2Device::OnGetNextAssertion(
    base::span<const uint8_t> request_bytes,
    std::vector<uint8_t>* response) {
  if (!request_bytes.empty() && !cbor::Reader::Read(request_bytes)) {
    return CtapDeviceResponseCode::kCtap2ErrCBORUnexpectedType;
  }

  auto& pending_assertions = mutable_state()->pending_assertions;
  if (pending_assertions.empty()) {
    return CtapDeviceResponseCode::kCtap2ErrNotAllowed;
  }

  *response = std::move(pending_assertions.back());
  pending_assertions.pop_back();

  return CtapDeviceResponseCode::kSuccess;
}

CtapDeviceResponseCode VirtualCtap2Device::OnPINCommand(
    base::span<const uint8_t> request_bytes,
    std::vector<uint8_t>* response) {
  if (device_info_->options().client_pin_availability ==
      AuthenticatorSupportedOptions::ClientPinAvailability::kNotSupported) {
    return CtapDeviceResponseCode::kCtap1ErrInvalidCommand;
  }

  const auto& cbor_request = cbor::Reader::Read(request_bytes);
  if (!cbor_request || !cbor_request->is_map()) {
    return CtapDeviceResponseCode::kCtap2ErrCBORUnexpectedType;
  }
  const auto& request_map = cbor_request->GetMap();

  const auto protocol_it = request_map.find(
      cbor::Value(static_cast<int>(pin::RequestKey::kProtocol)));
  if (protocol_it == request_map.end() || !protocol_it->second.is_unsigned()) {
    return CtapDeviceResponseCode::kCtap2ErrCBORUnexpectedType;
  }
  if (protocol_it->second.GetUnsigned() != pin::kProtocolVersion) {
    return CtapDeviceResponseCode::kCtap1ErrInvalidCommand;
  }

  const auto subcommand_it = request_map.find(
      cbor::Value(static_cast<int>(pin::RequestKey::kSubcommand)));
  if (subcommand_it == request_map.end() ||
      !subcommand_it->second.is_unsigned()) {
    return CtapDeviceResponseCode::kCtap2ErrCBORUnexpectedType;
  }
  const int64_t subcommand = subcommand_it->second.GetUnsigned();

  cbor::Value::MapValue response_map;
  switch (subcommand) {
    case static_cast<int>(device::pin::Subcommand::kGetRetries):
      response_map.emplace(static_cast<int>(pin::ResponseKey::kRetries),
                           mutable_state()->retries);
      break;

    case static_cast<int>(device::pin::Subcommand::kGetKeyAgreement): {
      bssl::UniquePtr<EC_KEY> key(
          EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
      CHECK(EC_KEY_generate_key(key.get()));
      response_map.emplace(static_cast<int>(pin::ResponseKey::kKeyAgreement),
                           pin::EncodeCOSEPublicKey(key.get()));
      mutable_state()->ecdh_key = std::move(key);
      break;
    }

    case static_cast<int>(device::pin::Subcommand::kSetPIN): {
      const auto encrypted_pin =
          GetPINBytestring(request_map, pin::RequestKey::kNewPINEnc);
      const auto pin_auth =
          GetPINBytestring(request_map, pin::RequestKey::kPINAuth);
      const auto peer_key =
          GetPINKey(request_map, pin::RequestKey::kKeyAgreement);

      if (!encrypted_pin || (encrypted_pin->size() % AES_BLOCK_SIZE) != 0 ||
          !pin_auth || !peer_key) {
        return CtapDeviceResponseCode::kCtap2ErrMissingParameter;
      }

      if (!mutable_state()->pin.empty()) {
        return CtapDeviceResponseCode::kCtap2ErrPinAuthInvalid;
      }

      uint8_t shared_key[SHA256_DIGEST_LENGTH];
      if (!mutable_state()->ecdh_key) {
        // kGetKeyAgreement should have been called first.
        NOTREACHED();
        return CtapDeviceResponseCode::kCtap2ErrPinTokenExpired;
      }
      pin::CalculateSharedKey(mutable_state()->ecdh_key.get(), peer_key->get(),
                              shared_key);

      CtapDeviceResponseCode err =
          SetPIN(mutable_state(), shared_key, *encrypted_pin, *pin_auth);
      if (err != CtapDeviceResponseCode::kSuccess) {
        return err;
      };

      AuthenticatorSupportedOptions options = device_info_->options();
      options.client_pin_availability = AuthenticatorSupportedOptions::
          ClientPinAvailability::kSupportedAndPinSet;
      device_info_->SetOptions(options);

      break;
    }

    case static_cast<int>(device::pin::Subcommand::kChangePIN): {
      const auto encrypted_new_pin =
          GetPINBytestring(request_map, pin::RequestKey::kNewPINEnc);
      const auto encrypted_pin_hash =
          GetPINBytestring(request_map, pin::RequestKey::kPINHashEnc);
      const auto pin_auth =
          GetPINBytestring(request_map, pin::RequestKey::kPINAuth);
      const auto peer_key =
          GetPINKey(request_map, pin::RequestKey::kKeyAgreement);

      if (!encrypted_pin_hash || encrypted_pin_hash->size() != AES_BLOCK_SIZE ||
          !encrypted_new_pin ||
          (encrypted_new_pin->size() % AES_BLOCK_SIZE) != 0 || !pin_auth ||
          !peer_key) {
        return CtapDeviceResponseCode::kCtap2ErrMissingParameter;
      }

      uint8_t shared_key[SHA256_DIGEST_LENGTH];
      if (!mutable_state()->ecdh_key) {
        // kGetKeyAgreement should have been called first.
        NOTREACHED();
        return CtapDeviceResponseCode::kCtap2ErrPinTokenExpired;
      }
      pin::CalculateSharedKey(mutable_state()->ecdh_key.get(), peer_key->get(),
                              shared_key);

      CtapDeviceResponseCode err =
          ConfirmPresentedPIN(mutable_state(), shared_key, *encrypted_pin_hash);
      if (err != CtapDeviceResponseCode::kSuccess) {
        return err;
      };

      err = SetPIN(mutable_state(), shared_key, *encrypted_new_pin, *pin_auth);
      if (err != CtapDeviceResponseCode::kSuccess) {
        return err;
      };

      break;
    }

    case static_cast<int>(device::pin::Subcommand::kGetPINToken): {
      const auto encrypted_pin_hash =
          GetPINBytestring(request_map, pin::RequestKey::kPINHashEnc);
      const auto peer_key =
          GetPINKey(request_map, pin::RequestKey::kKeyAgreement);

      if (!encrypted_pin_hash || encrypted_pin_hash->size() != AES_BLOCK_SIZE ||
          !peer_key) {
        return CtapDeviceResponseCode::kCtap2ErrMissingParameter;
      }

      uint8_t shared_key[SHA256_DIGEST_LENGTH];
      if (!mutable_state()->ecdh_key) {
        // kGetKeyAgreement should have been called first.
        NOTREACHED();
        return CtapDeviceResponseCode::kCtap2ErrPinTokenExpired;
      }
      pin::CalculateSharedKey(mutable_state()->ecdh_key.get(), peer_key->get(),
                              shared_key);

      CtapDeviceResponseCode err =
          ConfirmPresentedPIN(mutable_state(), shared_key, *encrypted_pin_hash);
      if (err != CtapDeviceResponseCode::kSuccess) {
        return err;
      };

      RAND_bytes(mutable_state()->pin_token,
                 sizeof(mutable_state()->pin_token));
      uint8_t encrypted_pin_token[sizeof(mutable_state()->pin_token)];
      pin::Encrypt(shared_key, mutable_state()->pin_token, encrypted_pin_token);
      response_map.emplace(static_cast<int>(pin::ResponseKey::kPINToken),
                           base::span<const uint8_t>(encrypted_pin_token));
      break;
    }

    default:
      return CtapDeviceResponseCode::kCtap1ErrInvalidCommand;
  }

  *response = cbor::Writer::Write(cbor::Value(std::move(response_map))).value();
  return CtapDeviceResponseCode::kSuccess;
}

CtapDeviceResponseCode VirtualCtap2Device::OnAuthenticatorGetInfo(
    std::vector<uint8_t>* response) const {
  *response = EncodeToCBOR(*device_info_);
  return CtapDeviceResponseCode::kSuccess;
}

AttestedCredentialData VirtualCtap2Device::ConstructAttestedCredentialData(
    std::vector<uint8_t> key_handle,
    std::unique_ptr<PublicKey> public_key) {
  constexpr std::array<uint8_t, 2> sha256_length = {0, crypto::kSHA256Length};
  constexpr std::array<uint8_t, 16> kZeroAaguid = {0, 0, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 0};
  base::span<const uint8_t, 16> aaguid(kDeviceAaguid);
  if (mutable_state()->self_attestation &&
      !mutable_state()->non_zero_aaguid_with_self_attestation) {
    aaguid = kZeroAaguid;
  }
  return AttestedCredentialData(aaguid, sha256_length, std::move(key_handle),
                                std::move(public_key));
}

AuthenticatorData VirtualCtap2Device::ConstructAuthenticatorData(
    base::span<const uint8_t, kRpIdHashLength> rp_id_hash,
    bool user_verified,
    uint32_t current_signature_count,
    base::Optional<AttestedCredentialData> attested_credential_data,
    base::Optional<cbor::Value> extensions) {
  uint8_t flag =
      base::strict_cast<uint8_t>(AuthenticatorData::Flag::kTestOfUserPresence);
  if (user_verified) {
    flag |= base::strict_cast<uint8_t>(
        AuthenticatorData::Flag::kTestOfUserVerification);
  }
  if (attested_credential_data)
    flag |= base::strict_cast<uint8_t>(AuthenticatorData::Flag::kAttestation);
  if (extensions) {
    flag |= base::strict_cast<uint8_t>(
        AuthenticatorData::Flag::kExtensionDataIncluded);
  }

  std::array<uint8_t, kSignCounterLength> signature_counter;
  signature_counter[0] = (current_signature_count >> 24) & 0xff;
  signature_counter[1] = (current_signature_count >> 16) & 0xff;
  signature_counter[2] = (current_signature_count >> 8) & 0xff;
  signature_counter[3] = (current_signature_count)&0xff;

  return AuthenticatorData(rp_id_hash, flag, signature_counter,
                           std::move(attested_credential_data),
                           std::move(extensions));
}

base::Optional<std::pair<CtapMakeCredentialRequest,
                         CtapMakeCredentialRequest::ClientDataHash>>
ParseCtapMakeCredentialRequest(base::span<const uint8_t> request_bytes) {
  const auto& cbor_request = cbor::Reader::Read(request_bytes);
  if (!cbor_request || !cbor_request->is_map())
    return base::nullopt;

  const auto& request_map = cbor_request->GetMap();
  if (!AreMakeCredentialRequestMapKeysCorrect(request_map))
    return base::nullopt;

  const auto client_data_hash_it = request_map.find(cbor::Value(1));
  if (client_data_hash_it == request_map.end() ||
      !client_data_hash_it->second.is_bytestring())
    return base::nullopt;

  const auto client_data_hash =
      base::make_span(client_data_hash_it->second.GetBytestring())
          .subspan<0, kClientDataHashLength>();

  const auto rp_entity_it = request_map.find(cbor::Value(2));
  if (rp_entity_it == request_map.end() || !rp_entity_it->second.is_map())
    return base::nullopt;

  auto rp_entity =
      PublicKeyCredentialRpEntity::CreateFromCBORValue(rp_entity_it->second);
  if (!rp_entity)
    return base::nullopt;

  const auto user_entity_it = request_map.find(cbor::Value(3));
  if (user_entity_it == request_map.end() || !user_entity_it->second.is_map())
    return base::nullopt;

  auto user_entity = PublicKeyCredentialUserEntity::CreateFromCBORValue(
      user_entity_it->second);
  if (!user_entity)
    return base::nullopt;

  const auto credential_params_it = request_map.find(cbor::Value(4));
  if (credential_params_it == request_map.end())
    return base::nullopt;

  auto credential_params = PublicKeyCredentialParams::CreateFromCBORValue(
      credential_params_it->second);
  if (!credential_params)
    return base::nullopt;

  CtapMakeCredentialRequest request(
      std::string() /* client_data_json */, std::move(*rp_entity),
      std::move(*user_entity), std::move(*credential_params));

  const auto exclude_list_it = request_map.find(cbor::Value(5));
  if (exclude_list_it != request_map.end()) {
    if (!exclude_list_it->second.is_array())
      return base::nullopt;

    const auto& credential_descriptors = exclude_list_it->second.GetArray();
    std::vector<PublicKeyCredentialDescriptor> exclude_list;
    for (const auto& credential_descriptor : credential_descriptors) {
      auto excluded_credential =
          PublicKeyCredentialDescriptor::CreateFromCBORValue(
              credential_descriptor);
      if (!excluded_credential)
        return base::nullopt;

      exclude_list.push_back(std::move(*excluded_credential));
    }
    request.SetExcludeList(std::move(exclude_list));
  }

  const auto extensions_it = request_map.find(cbor::Value(6));
  if (extensions_it != request_map.end()) {
    if (!extensions_it->second.is_map()) {
      return base::nullopt;
    }

    const auto& extensions = extensions_it->second.GetMap();
    const auto hmac_secret_it =
        extensions.find(cbor::Value(kExtensionHmacSecret));
    if (hmac_secret_it != extensions.end()) {
      if (!hmac_secret_it->second.is_bool()) {
        return base::nullopt;
      }
      request.SetHmacSecret(hmac_secret_it->second.GetBool());
    }
  }

  const auto option_it = request_map.find(cbor::Value(7));
  if (option_it != request_map.end()) {
    if (!option_it->second.is_map())
      return base::nullopt;

    const auto& option_map = option_it->second.GetMap();
    if (!IsMakeCredentialOptionMapFormatCorrect(option_map))
      return base::nullopt;

    const auto resident_key_option =
        option_map.find(cbor::Value(kResidentKeyMapKey));
    if (resident_key_option != option_map.end())
      request.SetResidentKeyRequired(resident_key_option->second.GetBool());

    const auto uv_option =
        option_map.find(cbor::Value(kUserVerificationMapKey));
    if (uv_option != option_map.end())
      request.SetUserVerification(
          uv_option->second.GetBool()
              ? UserVerificationRequirement::kRequired
              : UserVerificationRequirement::kDiscouraged);
  }

  const auto pin_auth_it = request_map.find(cbor::Value(8));
  if (pin_auth_it != request_map.end()) {
    if (!pin_auth_it->second.is_bytestring())
      return base::nullopt;
    request.SetPinAuth(pin_auth_it->second.GetBytestring());
  }

  const auto pin_protocol_it = request_map.find(cbor::Value(9));
  if (pin_protocol_it != request_map.end()) {
    if (!pin_protocol_it->second.is_unsigned() ||
        pin_protocol_it->second.GetUnsigned() >
            std::numeric_limits<uint8_t>::max())
      return base::nullopt;
    request.SetPinProtocol(pin_protocol_it->second.GetUnsigned());
  }

  return std::make_pair(std::move(request),
                        fido_parsing_utils::Materialize(client_data_hash));
}

base::Optional<
    std::pair<CtapGetAssertionRequest, CtapGetAssertionRequest::ClientDataHash>>
ParseCtapGetAssertionRequest(base::span<const uint8_t> request_bytes) {
  const auto& cbor_request = cbor::Reader::Read(request_bytes);
  if (!cbor_request || !cbor_request->is_map())
    return base::nullopt;

  const auto& request_map = cbor_request->GetMap();
  if (!AreGetAssertionRequestMapKeysCorrect(request_map))
    return base::nullopt;

  const auto rp_id_it = request_map.find(cbor::Value(1));
  if (rp_id_it == request_map.end() || !rp_id_it->second.is_string())
    return base::nullopt;

  const auto client_data_hash_it = request_map.find(cbor::Value(2));
  if (client_data_hash_it == request_map.end() ||
      !client_data_hash_it->second.is_bytestring())
    return base::nullopt;

  const auto client_data_hash =
      base::make_span(client_data_hash_it->second.GetBytestring())
          .subspan<0, kClientDataHashLength>();

  CtapGetAssertionRequest request(rp_id_it->second.GetString(),
                                  std::string() /* client_data_json */);

  const auto allow_list_it = request_map.find(cbor::Value(3));
  if (allow_list_it != request_map.end()) {
    if (!allow_list_it->second.is_array())
      return base::nullopt;

    const auto& credential_descriptors = allow_list_it->second.GetArray();
    std::vector<PublicKeyCredentialDescriptor> allow_list;
    for (const auto& credential_descriptor : credential_descriptors) {
      auto allowed_credential =
          PublicKeyCredentialDescriptor::CreateFromCBORValue(
              credential_descriptor);
      if (!allowed_credential)
        return base::nullopt;

      allow_list.push_back(std::move(*allowed_credential));
    }
    request.SetAllowList(std::move(allow_list));
  }

  const auto option_it = request_map.find(cbor::Value(5));
  if (option_it != request_map.end()) {
    if (!option_it->second.is_map())
      return base::nullopt;

    const auto& option_map = option_it->second.GetMap();
    if (!IsGetAssertionOptionMapFormatCorrect(option_map))
      return base::nullopt;

    const auto user_presence_option =
        option_map.find(cbor::Value(kUserPresenceMapKey));
    if (user_presence_option != option_map.end())
      request.SetUserPresenceRequired(user_presence_option->second.GetBool());

    const auto uv_option =
        option_map.find(cbor::Value(kUserVerificationMapKey));
    if (uv_option != option_map.end())
      request.SetUserVerification(
          uv_option->second.GetBool()
              ? UserVerificationRequirement::kRequired
              : UserVerificationRequirement::kPreferred);
  }

  const auto pin_auth_it = request_map.find(cbor::Value(6));
  if (pin_auth_it != request_map.end()) {
    if (!pin_auth_it->second.is_bytestring())
      return base::nullopt;
    request.SetPinAuth(pin_auth_it->second.GetBytestring());
  }

  const auto pin_protocol_it = request_map.find(cbor::Value(7));
  if (pin_protocol_it != request_map.end()) {
    if (!pin_protocol_it->second.is_unsigned() ||
        pin_protocol_it->second.GetUnsigned() >
            std::numeric_limits<uint8_t>::max())
      return base::nullopt;
    request.SetPinProtocol(pin_protocol_it->second.GetUnsigned());
  }

  return std::make_pair(std::move(request),
                        fido_parsing_utils::Materialize(client_data_hash));
}

}  // namespace device
