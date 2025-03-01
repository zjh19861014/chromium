// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/sync/device_info/local_device_info_provider_impl.h"

#include "base/bind.h"
#include "components/sync/device_info/local_device_info_util.h"
#include "components/sync/driver/sync_util.h"

namespace syncer {

LocalDeviceInfoProviderImpl::LocalDeviceInfoProviderImpl(
    version_info::Channel channel,
    const std::string& version,
    const SigninScopedDeviceIdCallback& signin_scoped_device_id_callback)
    : channel_(channel),
      version_(version),
      signin_scoped_device_id_callback_(signin_scoped_device_id_callback),
      weak_factory_(this) {
  DCHECK(signin_scoped_device_id_callback_);
}

LocalDeviceInfoProviderImpl::~LocalDeviceInfoProviderImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

version_info::Channel LocalDeviceInfoProviderImpl::GetChannel() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return channel_;
}

const DeviceInfo* LocalDeviceInfoProviderImpl::GetLocalDeviceInfo() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return local_device_info_.get();
}

std::string LocalDeviceInfoProviderImpl::GetSyncUserAgent() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return MakeUserAgentForSync(channel_);
}

std::unique_ptr<LocalDeviceInfoProvider::Subscription>
LocalDeviceInfoProviderImpl::RegisterOnInitializedCallback(
    const base::RepeatingClosure& callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!local_device_info_);
  return callback_list_.Add(callback);
}

void LocalDeviceInfoProviderImpl::Initialize(const std::string& cache_guid,
                                             const std::string& session_name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!cache_guid.empty());

  local_device_info_ = std::make_unique<DeviceInfo>(
      cache_guid, session_name, version_, GetSyncUserAgent(),
      GetLocalDeviceType(), signin_scoped_device_id_callback_.Run());

  // Notify observers.
  callback_list_.Notify();
}

void LocalDeviceInfoProviderImpl::Clear() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  local_device_info_.reset();
}

}  // namespace syncer
