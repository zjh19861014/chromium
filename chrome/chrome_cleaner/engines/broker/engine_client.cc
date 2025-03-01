// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/chrome_cleaner/engines/broker/engine_client.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>
#include <memory>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string16.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "chrome/chrome_cleaner/constants/quarantine_constants.h"
#include "chrome/chrome_cleaner/interfaces/engine_file_requests.mojom.h"
#include "chrome/chrome_cleaner/interfaces/engine_requests.mojom.h"
#include "chrome/chrome_cleaner/interfaces/engine_sandbox.mojom.h"
#include "chrome/chrome_cleaner/ipc/mojo_task_runner.h"
#include "chrome/chrome_cleaner/logging/logging_service_api.h"
#include "chrome/chrome_cleaner/os/layered_service_provider_wrapper.h"
#include "chrome/chrome_cleaner/os/system_util_cleaner.h"
#include "chrome/chrome_cleaner/pup_data/pup_data.h"
#include "chrome/chrome_cleaner/settings/settings.h"
#include "chrome/chrome_cleaner/zip_archiver/sandboxed_zip_archiver.h"
#include "components/chrome_cleaner/public/constants/constants.h"
#include "components/chrome_cleaner/public/constants/result_codes.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "sandbox/win/src/sandbox_factory.h"

using base::WaitableEvent;

namespace chrome_cleaner {

namespace {

// The maximal allowed time to run the scanner (15 minutes).
const uint32_t kIncreasedWatchdogTimeoutInSeconds = 15 * 60;

using ResultCallback = base::OnceCallback<void(uint32_t)>;

// Wraps a CallbackWithDeleteHelper around |callback| which is to be passed to
// |engine_commands_ptr_|. If the connection dies before |callback| is invoked,
// Mojo will delete it without running it. In that case call it with default
// arguments to ensure that side effects (such as unblocking a WaitableEvent)
// still happen.
//
// If |callback| must be called on a particular sequence, then the
// CallbackWithDeleteHelper which is returned from this function must be
// destroyed on that sequence, since the destructor can invoke |callback|. For
// example when this is used to wrap SaveResultCallback it must be destroyed on
// the Mojo thread. The easiest way to ensure this is to call
// CallbackWithErrorHandling from the Mojo thread and never use base::Passed on
// the resulting ScopedCallbackRunner to pass it another sequence.
ResultCallback CallbackWithErrorHandling(ResultCallback callback) {
  return mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      std::move(callback), EngineResultCode::kSandboxUnavailable);
}

// Writes |result| into |result_holder| and then signals |event| to let the
// main thread know that the result is ready to consume.
//
// This callback must be called on the Mojo thread to avoid deadlocks as the
// main thread will be blocked waiting for |event| to be signalled.
void SaveResultCallback(uint32_t* result_holder,
                        WaitableEvent* event,
                        uint32_t result) {
  *result_holder = result;
  event->Signal();
}

#if DCHECK_IS_ON()
bool g_has_created_instance = false;
#endif

}  // namespace

void EngineClient::ResetCreatedInstanceCheckForTesting() {
#if DCHECK_IS_ON()
  g_has_created_instance = false;
#endif
}

scoped_refptr<EngineClient> EngineClient::CreateEngineClient(
    const Engine::Name engine,
    const ResultCodeLoggingCallback& logging_callback,
    const SandboxConnectionErrorCallback& connection_error_callback,
    scoped_refptr<MojoTaskRunner> mojo_task_runner,
    std::unique_ptr<InterfaceMetadataObserver> metadata_observer) {
  // WrapRefCounted is needed instead of MakeRefCounted to access the protected
  // constructor.
  auto engine_client = base::WrapRefCounted(
      new EngineClient(engine, logging_callback, connection_error_callback,
                       mojo_task_runner, std::move(metadata_observer)));

#if DCHECK_IS_ON()
  DCHECK(!g_has_created_instance);
  g_has_created_instance = true;
#endif

  return engine_client;
}

EngineClient::EngineClient(
    const Engine::Name engine,
    const ResultCodeLoggingCallback& logging_callback,
    const SandboxConnectionErrorCallback& connection_error_callback,
    scoped_refptr<MojoTaskRunner> mojo_task_runner,
    std::unique_ptr<InterfaceMetadataObserver> metadata_observer)
    : engine_(engine),
      registry_logging_callback_(logging_callback),
      connection_error_callback_(connection_error_callback),
      mojo_task_runner_(mojo_task_runner),
      engine_commands_ptr_(std::make_unique<mojom::EngineCommandsPtr>()),
      interface_metadata_observer_(std::move(metadata_observer)) {
  DCHECK(mojo_task_runner_);
  InitializeReadOnlyCallbacks();
}

void EngineClient::InitializeReadOnlyCallbacks() {
  sandbox_file_requests_ = std::make_unique<EngineFileRequestsImpl>(
      mojo_task_runner_, interface_metadata_observer_.get());
  sandbox_requests_ = std::make_unique<EngineRequestsImpl>(
      mojo_task_runner_, interface_metadata_observer_.get());
  scan_results_impl_ = std::make_unique<EngineScanResultsImpl>(
      interface_metadata_observer_.get());
}

bool EngineClient::InitializeCleaningCallbacks(
    const std::vector<UwSId>& enabled_uws) {
  // |archive| = nullptr means the quarantine feature is disabled.
  std::unique_ptr<SandboxedZipArchiver> archiver = nullptr;
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(kQuarantineSwitch)) {
    if (!InitializeQuarantine(&archiver))
      return false;
  }

  std::unique_ptr<chrome_cleaner::FileRemoverAPI> file_remover =
      CreateFileRemoverWithDigestVerifier(
          enabled_uws, std::move(archiver),
          base::BindRepeating(&EngineClient::SetRebootRequired,
                              base::Unretained(this)));
  sandbox_cleaner_requests_ = std::make_unique<CleanerEngineRequestsImpl>(
      mojo_task_runner_, interface_metadata_observer_.get(),
      std::move(file_remover));
  cleanup_results_impl_ = std::make_unique<EngineCleanupResultsImpl>(
      interface_metadata_observer_.get());

  return true;
}

bool EngineClient::InitializeQuarantine(
    std::unique_ptr<SandboxedZipArchiver>* archiver) {
  base::FilePath quarantine_folder;
  if (!InitializeQuarantineFolder(&quarantine_folder)) {
    LOG(ERROR) << "Failed to initialize quarantine folder.";
    return false;
  }
  ResultCode result_code = SpawnZipArchiverSandbox(
      quarantine_folder, kQuarantinePassword, mojo_task_runner_,
      connection_error_callback_, archiver);
  if (result_code != RESULT_CODE_SUCCESS) {
    LOG(ERROR) << "Zip archiver initialization returned an error code: "
               << result_code;
    return false;
  }
  return true;
}

EngineClient::~EngineClient() {
  // Delete Mojo objects on the mojo thread. They will assert if deleted from
  // the wrong thread.
  mojo_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::unique_ptr<mojom::EngineCommandsPtr> commands,
             std::unique_ptr<EngineScanResultsImpl> scan_results,
             std::unique_ptr<EngineCleanupResultsImpl> cleanup_results,
             std::unique_ptr<EngineFileRequestsImpl> file_requests,
             std::unique_ptr<EngineRequestsImpl> sandbox_requests,
             std::unique_ptr<CleanerEngineRequestsImpl>
                 sandbox_cleaner_requests,
             std::unique_ptr<InterfaceMetadataObserver> metadata_observer) {
            // |commands| must be destroyed last because all the others are
            // associated interfaces of it. If |commands| is closed first we get
            // a connection error on the associated interfaces.
            file_requests.reset();
            sandbox_requests.reset();
            sandbox_cleaner_requests.reset();
            scan_results.reset();
            cleanup_results.reset();
            commands.reset();
            // |metadata_observer| must be destroyed after sandbox_requests and
            // sandbox_cleaner_requests in order to avoid invalid references.
            metadata_observer.reset();
          },
          base::Passed(&engine_commands_ptr_),
          base::Passed(&scan_results_impl_),
          base::Passed(&cleanup_results_impl_),
          base::Passed(&sandbox_file_requests_),
          base::Passed(&sandbox_requests_),
          base::Passed(&sandbox_cleaner_requests_),
          base::Passed(&interface_metadata_observer_)));
}

uint32_t EngineClient::ScanningWatchdogTimeoutInSeconds() const {
  return kIncreasedWatchdogTimeoutInSeconds;
}

void EngineClient::PostBindEngineCommandsPtr(
    mojo::ScopedMessagePipeHandle pipe) {
  mojo_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&EngineClient::BindEngineCommandsPtr,
                                base::RetainedRef(this), std::move(pipe),
                                base::BindOnce(connection_error_callback_,
                                               SandboxType::kEngine)));
}

void EngineClient::BindEngineCommandsPtr(mojo::ScopedMessagePipeHandle pipe,
                                         base::OnceClosure error_handler) {
  engine_commands_ptr_->Bind(mojom::EngineCommandsPtrInfo(std::move(pipe), 0));
  engine_commands_ptr_->set_connection_error_handler(std::move(error_handler));
}

void EngineClient::MaybeLogResultCode(EngineClient::Operation operation,
                                      uint32_t result_code) {
  // Don't overwrite the first error we encountered.
  if (cached_result_code_ != EngineResultCode::kSuccess)
    return;

  cached_result_code_ = result_code;

  const int operation_as_int = static_cast<int>(operation);
  // The operation should fit in the upper 16 bits of an int.
  DCHECK_LE(operation_as_int, 0xFFFF);
  // The result code should fit in the lower 16 bits of an int.
  DCHECK_LE(result_code, 0xFFFFU);

  if (registry_logging_callback_)
    registry_logging_callback_.Run((operation_as_int << 16) + result_code);
}

bool EngineClient::needs_reboot() const {
  return needs_reboot_;
}

std::vector<UwSId> EngineClient::GetEnabledUwS() const {
  // Disabled UwS isn't put into the global PUPData structure, so all supported
  // UwS is enabled.
  std::vector<UwSId> supported_uws;
  for (UwSId uws_id : *PUPData::GetUwSIds()) {
    if (PUPData::GetEngine(uws_id) == engine_)
      supported_uws.push_back(uws_id);
  }
  return supported_uws;
}

uint32_t EngineClient::Initialize() {
  // TODO(joenotcharles): Refactor the caller to allow all EngineClient
  // functions to be called asynchronously, and then remove all WaitableEvents.
  uint32_t result_code;
  WaitableEvent event(WaitableEvent::ResetPolicy::MANUAL,
                      WaitableEvent::InitialState::NOT_SIGNALED);
  mojo_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &EngineClient::InitializeAsync, base::Unretained(this),
          base::BindOnce(&SaveResultCallback, &result_code, &event)));
  event.Wait();
  MaybeLogResultCode(Operation::Initialize, result_code);
  return result_code;
}

void EngineClient::InitializeAsync(InitializeCallback result_callback) {
  if (interface_metadata_observer_)
    interface_metadata_observer_->ObserveCall(CURRENT_FILE_AND_METHOD);

  // Create a binding to the EngineFileRequests interface that will receive file
  // reading requests from Initialize.
  mojom::EngineFileRequestsAssociatedPtrInfo file_requests_info;
  sandbox_file_requests_->Bind(&file_requests_info);

  // Expose the logging directory for writing debug logs.
  base::FilePath logging_path;
#if !defined(CHROME_CLEANER_OFFICIAL_BUILD)
  if (!GetAppDataProductDirectory(&logging_path))
    LOG(ERROR) << "Couldn't get development log directory for sandboxed engine";
#endif

  (*engine_commands_ptr_)
      ->Initialize(std::move(file_requests_info), logging_path,
                   CallbackWithErrorHandling(std::move(result_callback)));
}

uint32_t EngineClient::StartScan(
    const std::vector<UwSId>& enabled_uws,
    const std::vector<UwS::TraceLocation>& enabled_locations,
    bool include_details,
    EngineClient::FoundUwSCallback found_callback,
    EngineClient::DoneCallback done_callback) {
  if (operation_in_progress_) {
    LOG(ERROR) << "EngineClient::StartScan called while an operation was still "
                  "in progress.";
    return EngineResultCode::kWrongState;
  }
  operation_in_progress_ = true;

  uint32_t result_code;
  WaitableEvent event(WaitableEvent::ResetPolicy::MANUAL,
                      WaitableEvent::InitialState::NOT_SIGNALED);
  mojo_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &EngineClient::StartScanAsync, base::Unretained(this), enabled_uws,
          enabled_locations, include_details, found_callback,
          base::Passed(&done_callback),
          base::BindOnce(&SaveResultCallback, &result_code, &event)));
  event.Wait();
  MaybeLogResultCode(Operation::StartScan, result_code);
  if (result_code != EngineResultCode::kSuccess) {
    operation_in_progress_ = false;
  }
  return result_code;
}

void EngineClient::StartScanAsync(
    const std::vector<UwSId>& enabled_uws,
    const std::vector<UwS::TraceLocation>& enabled_locations,
    bool include_details,
    EngineClient::FoundUwSCallback found_callback,
    EngineClient::DoneCallback done_callback,
    StartScanCallback result_callback) {
  // Create bindings to receive the requests sent from the sandboxed
  // code.
  mojom::EngineFileRequestsAssociatedPtrInfo file_requests_info;
  sandbox_file_requests_->Bind(&file_requests_info);

  mojom::EngineRequestsAssociatedPtrInfo engine_requests_info;
  sandbox_requests_->Bind(&engine_requests_info);

  // Create a binding to the EngineScanResults interface that will receive
  // results and pass them on to |found_callback| and |done_callback|.
  mojom::EngineScanResultsAssociatedPtrInfo scan_results_info;

  scan_results_impl_->BindToCallbacks(&scan_results_info, found_callback,
                                      std::move(done_callback));
  if (interface_metadata_observer_)
    interface_metadata_observer_->ObserveCall(CURRENT_FILE_AND_METHOD);

  // Starts scan on the target process. |result_callback| will be called with
  // the return value of the start scan operation; if it is
  // EngineResultCode::kSuccess, scan_results_impl_->FoundUwS (which in turn
  // calls |found_callback|) and scan_results_impl_->Done (which in turn calls
  // |done_callback|) with further results.
  (*engine_commands_ptr_)
      ->StartScan(enabled_uws, enabled_locations, include_details,
                  std::move(file_requests_info),
                  std::move(engine_requests_info), std::move(scan_results_info),
                  CallbackWithErrorHandling(std::move(result_callback)));
}

uint32_t EngineClient::StartCleanup(const std::vector<UwSId>& enabled_uws,
                                    EngineClient::DoneCallback done_callback) {
  if (!operation_in_progress_) {
    LOG(ERROR) << "EngineClient::StartCleanup called without an operation was "
                  "still in progress.";
    return EngineResultCode::kWrongState;
  }

  if (!InitializeCleaningCallbacks(enabled_uws)) {
    LOG(ERROR) << "Failed to initialize cleaning callbacks.";
    return EngineResultCode::kCleanupInitializationFailed;
  }

  uint32_t result_code;
  WaitableEvent event(WaitableEvent::ResetPolicy::MANUAL,
                      WaitableEvent::InitialState::NOT_SIGNALED);
  mojo_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &EngineClient::StartCleanupAsync, base::Unretained(this), enabled_uws,
          base::Passed(&done_callback),
          base::BindOnce(&SaveResultCallback, &result_code, &event)));
  event.Wait();

  MaybeLogResultCode(Operation::StartCleanup, result_code);
  if (result_code != EngineResultCode::kSuccess) {
    operation_in_progress_ = false;
  }
  return result_code;
}

void EngineClient::StartCleanupAsync(const std::vector<UwSId>& enabled_uws,
                                     EngineClient::DoneCallback done_callback,
                                     StartCleanupCallback result_callback) {
  // Create bindings to receive the requests sent from the sandboxed
  // code.
  mojom::EngineFileRequestsAssociatedPtrInfo file_requests_info;
  sandbox_file_requests_->Bind(&file_requests_info);

  mojom::EngineRequestsAssociatedPtrInfo engine_requests_info;
  sandbox_requests_->Bind(&engine_requests_info);

  mojom::CleanerEngineRequestsAssociatedPtrInfo cleaner_engine_requests_info;
  sandbox_cleaner_requests_->Bind(&cleaner_engine_requests_info);

  // Create a binding to the EngineCleanupResults interface that will
  // receive results and pass them on to |done_callback|.
  mojom::EngineCleanupResultsAssociatedPtrInfo cleanup_results_info;
  cleanup_results_impl_->BindToCallbacks(&cleanup_results_info,
                                         std::move(done_callback));

  if (interface_metadata_observer_)
    interface_metadata_observer_->ObserveCall(CURRENT_FILE_AND_METHOD);

  (*engine_commands_ptr_)
      ->StartCleanup(enabled_uws, std::move(file_requests_info),
                     std::move(engine_requests_info),
                     std::move(cleaner_engine_requests_info),
                     std::move(cleanup_results_info),
                     CallbackWithErrorHandling(std::move(result_callback)));
}

uint32_t EngineClient::Finalize() {
  DCHECK(operation_in_progress_);
  uint32_t result_code;
  WaitableEvent event(WaitableEvent::ResetPolicy::MANUAL,
                      WaitableEvent::InitialState::NOT_SIGNALED);
  mojo_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &EngineClient::FinalizeAsync, base::Unretained(this),
          base::BindOnce(&SaveResultCallback, &result_code, &event)));
  event.Wait();
  MaybeLogResultCode(Operation::Finalize, result_code);
  operation_in_progress_ = false;
  return result_code;
}

void EngineClient::FinalizeAsync(FinalizeCallback result_callback) {
  if (interface_metadata_observer_)
    interface_metadata_observer_->ObserveCall(CURRENT_FILE_AND_METHOD);
  (*engine_commands_ptr_)
      ->Finalize(CallbackWithErrorHandling(std::move(result_callback)));
}

void EngineClient::SetRebootRequired() {
  needs_reboot_ = true;
}

}  // namespace chrome_cleaner
