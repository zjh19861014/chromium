// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/chrome_cleaner/executables/shutdown_sequence.h"
#include "base/task/task_scheduler/task_scheduler.h"

#include "base/task/task_scheduler/task_scheduler.h"

namespace chrome_cleaner {

ShutdownSequence::ShutdownSequence() = default;

ShutdownSequence::ShutdownSequence(ShutdownSequence&& other)
    : engine_client(std::move(other.engine_client)),
      mojo_task_runner(std::move(other.mojo_task_runner)),
      engine_facade(std::move(other.engine_facade)) {}

ShutdownSequence::~ShutdownSequence() {
  if (!mojo_task_runner)
    return;

  auto* task_scheduler = base::TaskScheduler::GetInstance();
  if (task_scheduler)
    task_scheduler->Shutdown();

  // Objects that post messages to themselves with base::Unretained must be
  // destroyed after TaskScheduler::Shutdown, otherwise some tasks might be
  // still referencing recently destroyed objects.

  engine_facade.reset();
  engine_client.reset();
  mojo_task_runner.reset();
}

}  // namespace chrome_cleaner
