// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/web_thread_supporting_gc.h"

#include <memory>

#include "base/memory/ptr_util.h"
#include "third_party/blink/renderer/platform/memory_pressure_listener.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h"
#include "third_party/blink/renderer/platform/wtf/threading.h"

namespace blink {

WebThreadSupportingGC::WebThreadSupportingGC(
    const ThreadCreationParams& params) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
#if DCHECK_IS_ON()
  WTF::WillCreateThread();
#endif
  if (!thread_) {
    if (params.thread_type == WebThreadType::kAudioWorkletThread) {
      thread_ = Thread::CreateWebAudioThread();
    } else {
      thread_ = Thread::CreateThread(params);
    }
  }
  MemoryPressureListenerRegistry::Instance().RegisterThread(thread_.get());
}

WebThreadSupportingGC::~WebThreadSupportingGC() {
  DETACH_FROM_THREAD(thread_checker_);
  Thread* thread_pointer = thread_.get();
  // blink::Thread's destructor blocks until all the tasks are processed.
  thread_.reset();
  MemoryPressureListenerRegistry::Instance().UnregisterThread(thread_pointer);
}

void WebThreadSupportingGC::InitializeOnThread() {
  DCHECK(thread_->IsCurrentThread());
  ThreadState::AttachCurrentThread();
  gc_task_runner_ = std::make_unique<GCTaskRunner>(thread_.get());
}

void WebThreadSupportingGC::ShutdownOnThread() {
  DCHECK(thread_->IsCurrentThread());
#if defined(LEAK_SANITIZER)
  ThreadState::Current()->ReleaseStaticPersistentNodes();
#endif
  // Ensure no posted tasks will run from this point on.
  gc_task_runner_.reset();

  // Shutdown the thread (via its scheduler).
  thread_->Scheduler()->Shutdown();

  ThreadState::DetachCurrentThread();
}

}  // namespace blink
