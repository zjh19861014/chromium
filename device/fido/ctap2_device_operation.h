// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_CTAP2_DEVICE_OPERATION_H_
#define DEVICE_FIDO_CTAP2_DEVICE_OPERATION_H_

#include <stdint.h>

#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/callback.h"
#include "base/containers/span.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/strings/string_number_conversions.h"
#include "components/cbor/diagnostic_writer.h"
#include "components/cbor/reader.h"
#include "components/cbor/values.h"
#include "components/cbor/writer.h"
#include "components/device_event_log/device_event_log.h"
#include "device/fido/device_operation.h"
#include "device/fido/device_response_converter.h"
#include "device/fido/fido_constants.h"
#include "device/fido/fido_device.h"

namespace device {

// Ctap2DeviceOperation performs a single request--response operation on a CTAP2
// device. The |Request| class must implement an |EncodeToCBOR| method that
// returns a pair of |CtapRequestCommand| and an optional CBOR |Value|.
// The response will be parsed to CBOR and then further parsed into a |Response|
// using a provided callback.
template <class Request, class Response>
class Ctap2DeviceOperation : public DeviceOperation<Request, Response> {
 public:
  // DeviceResponseCallback is either called with a |kSuccess| and a |Response|
  // object, or else is called with a value other than |kSuccess| and
  // |nullopt|.
  using DeviceResponseCallback =
      base::OnceCallback<void(CtapDeviceResponseCode,
                              base::Optional<Response>)>;
  // DeviceResponseParser converts a generic CBOR structure into an
  // operation-specific response. If the response didn't have a payload then the
  // argument will be |nullopt|. The parser should return |nullopt| on error.
  using DeviceResponseParser = base::OnceCallback<base::Optional<Response>(
      const base::Optional<cbor::Value>&)>;

  Ctap2DeviceOperation(FidoDevice* device,
                       Request request,
                       DeviceResponseCallback callback,
                       DeviceResponseParser device_response_parser)
      : DeviceOperation<Request, Response>(device,
                                           std::move(request),
                                           std::move(callback)),
        device_response_parser_(std::move(device_response_parser)),
        weak_factory_(this) {}

  ~Ctap2DeviceOperation() override = default;

  void Start() override {
    std::pair<CtapRequestCommand, base::Optional<cbor::Value>> request(
        this->request().EncodeAsCBOR());
    std::vector<uint8_t> request_bytes;

    // TODO: it would be nice to see which device each request is going to, but
    // that breaks every mock test because they aren't expecting a call to
    // GetId().
    if (request.second) {
      FIDO_LOG(DEBUG) << "<- " << static_cast<int>(request.first) << " "
                      << cbor::DiagnosticWriter::Write(*request.second);
      base::Optional<std::vector<uint8_t>> cbor_bytes =
          cbor::Writer::Write(*request.second);
      DCHECK(cbor_bytes);
      request_bytes = std::move(*cbor_bytes);
    } else {
      FIDO_LOG(DEBUG) << "<- " << static_cast<int>(request.first)
                      << " (no payload)";
    }

    request_bytes.insert(request_bytes.begin(),
                         static_cast<uint8_t>(request.first));

    this->token_ = this->device()->DeviceTransact(
        std::move(request_bytes),
        base::BindOnce(&Ctap2DeviceOperation::OnResponseReceived,
                       weak_factory_.GetWeakPtr()));
  }

  void OnResponseReceived(
      base::Optional<std::vector<uint8_t>> device_response) {
    this->token_.reset();

    // TODO: it would be nice to see which device each response is coming from,
    // but that breaks every mock test because they aren't expecting a call to
    // GetId().
    if (!device_response) {
      FIDO_LOG(ERROR) << "-> (error reading)";
      std::move(this->callback())
          .Run(CtapDeviceResponseCode::kCtap2ErrOther, base::nullopt);
      return;
    }

    auto response_code = GetResponseCode(*device_response);
    if (response_code != CtapDeviceResponseCode::kSuccess) {
      FIDO_LOG(DEBUG) << "-> (CTAP2 error code "
                      << static_cast<int>(response_code) << ")";
      std::move(this->callback()).Run(response_code, base::nullopt);
      return;
    }
    DCHECK(!device_response->empty());

    base::Optional<cbor::Value> cbor;
    base::Optional<Response> response;
    base::span<const uint8_t> cbor_bytes(*device_response);
    cbor_bytes = cbor_bytes.subspan(1);

    if (!cbor_bytes.empty()) {
      cbor::Reader::DecoderError error;
      cbor = cbor::Reader::Read(cbor_bytes, &error);
      if (!cbor) {
        FIDO_LOG(ERROR) << "-> (CBOR parse error " << static_cast<int>(error)
                        << " from "
                        << base::HexEncode(device_response->data(),
                                           device_response->size())
                        << ")";
        std::move(this->callback())
            .Run(CtapDeviceResponseCode::kCtap2ErrInvalidCBOR, base::nullopt);
        return;
      }

      response = std::move(std::move(device_response_parser_).Run(cbor));
      if (response) {
        FIDO_LOG(DEBUG) << "-> " << cbor::DiagnosticWriter::Write(*cbor);
      } else {
        FIDO_LOG(ERROR) << "-> (rejected CBOR structure) "
                        << cbor::DiagnosticWriter::Write(*cbor);
      }
    } else {
      response =
          std::move(std::move(device_response_parser_).Run(base::nullopt));
      if (response) {
        FIDO_LOG(DEBUG) << "-> (empty payload)";
      } else {
        FIDO_LOG(ERROR) << "-> (rejected empty payload)";
      }
    }

    if (!response) {
      response_code = CtapDeviceResponseCode::kCtap2ErrInvalidCBOR;
    }
    std::move(this->callback()).Run(response_code, std::move(response));
  }

 private:
  DeviceResponseParser device_response_parser_;
  base::WeakPtrFactory<Ctap2DeviceOperation> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(Ctap2DeviceOperation);
};

}  // namespace device

#endif  // DEVICE_FIDO_CTAP2_DEVICE_OPERATION_H_
