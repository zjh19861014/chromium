/*
 * Copyright (C) 2011, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "third_party/blink/renderer/modules/gamepad/gamepad.h"

#include <algorithm>

#include "third_party/blink/renderer/modules/gamepad/navigator_gamepad.h"

namespace blink {

Gamepad::Gamepad(NavigatorGamepad* navigator_gamepad, unsigned index)
    : navigator_gamepad_(navigator_gamepad),
      index_(index),
      timestamp_(0.0),
      has_vibration_actuator_(false),
      vibration_actuator_type_(device::GamepadHapticActuatorType::kDualRumble),
      display_id_(0),
      is_axis_data_dirty_(true),
      is_button_data_dirty_(true) {}

Gamepad::~Gamepad() = default;

const Gamepad::DoubleVector& Gamepad::axes() {
  is_axis_data_dirty_ = false;
  return axes_;
}

void Gamepad::SetAxes(unsigned count, const double* data) {
  bool skip_update =
      axes_.size() == count && std::equal(data, data + count, axes_.begin());
  if (skip_update)
    return;

  axes_.resize(count);
  if (count)
    std::copy(data, data + count, axes_.begin());
  is_axis_data_dirty_ = true;
}

const GamepadButtonVector& Gamepad::buttons() {
  is_button_data_dirty_ = false;
  return buttons_;
}

void Gamepad::SetButtons(unsigned count, const device::GamepadButton* data) {
  bool skip_update =
      buttons_.size() == count &&
      std::equal(data, data + count, buttons_.begin(),
                 [](const device::GamepadButton& device_gamepad_button,
                    const Member<GamepadButton>& gamepad_button) {
                   return gamepad_button->IsEqual(device_gamepad_button);
                 });
  if (skip_update)
    return;

  if (buttons_.size() != count) {
    buttons_.resize(count);
    for (unsigned i = 0; i < count; ++i)
      buttons_[i] = MakeGarbageCollected<GamepadButton>();
  }
  for (unsigned i = 0; i < count; ++i)
    buttons_[i]->UpdateValuesFrom(data[i]);
  is_button_data_dirty_ = true;
}

GamepadHapticActuator* Gamepad::vibrationActuator() const {
  // A disconnected gamepad may share the same index as a newly-connected
  // gamepad. Return nullptr for disconnected gamepads to avoid returning the
  // actuator for the connected gamepad.
  return connected_ ? navigator_gamepad_->GetVibrationActuator(index_)
                    : nullptr;
}

void Gamepad::SetVibrationActuatorInfo(
    const device::GamepadHapticActuator& actuator) {
  has_vibration_actuator_ = actuator.not_null;
  vibration_actuator_type_ = actuator.type;
}

void Gamepad::SetPose(const device::GamepadPose& pose) {
  if (!pose.not_null) {
    if (pose_)
      pose_ = nullptr;
    return;
  }

  if (!pose_)
    pose_ = MakeGarbageCollected<GamepadPose>();

  pose_->SetPose(pose);
}

void Gamepad::SetHand(const device::GamepadHand& hand) {
  switch (hand) {
    case device::GamepadHand::kNone:
      hand_ = "";
      break;
    case device::GamepadHand::kLeft:
      hand_ = "left";
      break;
    case device::GamepadHand::kRight:
      hand_ = "right";
      break;
    default:
      NOTREACHED();
  }
}

void Gamepad::Trace(blink::Visitor* visitor) {
  visitor->Trace(navigator_gamepad_);
  visitor->Trace(buttons_);
  visitor->Trace(pose_);
  ScriptWrappable::Trace(visitor);
}

}  // namespace blink
