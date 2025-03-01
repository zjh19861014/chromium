// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_CORE_PLATFORM_HANDLE_DISPATCHER_H_
#define MOJO_CORE_PLATFORM_HANDLE_DISPATCHER_H_

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "mojo/core/dispatcher.h"
#include "mojo/core/system_impl_export.h"
#include "mojo/public/cpp/platform/platform_handle.h"

namespace mojo {
namespace core {

class MOJO_SYSTEM_IMPL_EXPORT PlatformHandleDispatcher : public Dispatcher {
 public:
  static scoped_refptr<PlatformHandleDispatcher> Create(
      PlatformHandle platform_handle);

  PlatformHandle TakePlatformHandle();

  // Dispatcher:
  Type GetType() const override;
  MojoResult Close() override;
  void StartSerialize(uint32_t* num_bytes,
                      uint32_t* num_ports,
                      uint32_t* num_handles) override;
  bool EndSerialize(void* destination,
                    ports::UserMessageEvent::PortAttachment* ports,
                    PlatformHandle* handles) override;
  bool BeginTransit() override;
  void CompleteTransitAndClose() override;
  void CancelTransit() override;

  static scoped_refptr<PlatformHandleDispatcher> Deserialize(
      const void* bytes,
      size_t num_bytes,
      const ports::UserMessageEvent::PortAttachment* ports,
      size_t num_ports,
      PlatformHandle* handles,
      size_t num_handles);

 private:
  PlatformHandleDispatcher(PlatformHandle platform_handle);
  ~PlatformHandleDispatcher() override;

  base::Lock lock_;
  bool in_transit_ = false;
  bool is_closed_ = false;
  PlatformHandle platform_handle_;

  DISALLOW_COPY_AND_ASSIGN(PlatformHandleDispatcher);
};

}  // namespace core
}  // namespace mojo

#endif  // MOJO_CORE_PLATFORM_HANDLE_DISPATCHER_H_
