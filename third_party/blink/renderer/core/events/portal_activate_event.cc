// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/events/portal_activate_event.h"

#include <utility>
#include "third_party/blink/renderer/bindings/core/v8/script_value.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/portal/html_portal_element.h"
#include "third_party/blink/renderer/core/messaging/message_port.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/wtf/time.h"

namespace blink {

PortalActivateEvent* PortalActivateEvent::Create(
    LocalFrame* frame,
    const base::UnguessableToken& predecessor_portal_token,
    mojom::blink::PortalAssociatedPtr predecessor_portal_ptr,
    scoped_refptr<SerializedScriptValue> data,
    MessagePortArray* ports) {
  return MakeGarbageCollected<PortalActivateEvent>(
      frame->GetDocument(), predecessor_portal_token,
      std::move(predecessor_portal_ptr),
      SerializedScriptValue::Unpack(std::move(data)), ports);
}

PortalActivateEvent::PortalActivateEvent(
    Document* document,
    const base::UnguessableToken& predecessor_portal_token,
    mojom::blink::PortalAssociatedPtr predecessor_portal_ptr,
    UnpackedSerializedScriptValue* data,
    MessagePortArray* ports)
    : Event(event_type_names::kPortalactivate,
            Bubbles::kNo,
            Cancelable::kNo,
            CurrentTimeTicks()),
      document_(document),
      predecessor_portal_token_(predecessor_portal_token),
      predecessor_portal_ptr_(std::move(predecessor_portal_ptr)),
      data_(data),
      ports_(ports) {}

PortalActivateEvent::~PortalActivateEvent() = default;

ScriptValue PortalActivateEvent::data(ScriptState* script_state) {
  v8::Isolate* isolate = script_state->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  if (!data_)
    return ScriptValue(script_state, v8::Null(isolate));

  auto result =
      v8_data_.insert(script_state, TraceWrapperV8Reference<v8::Value>());
  TraceWrapperV8Reference<v8::Value>& relevant_data =
      result.stored_value->value;

  if (!result.is_new_entry)
    return ScriptValue(script_state, relevant_data.NewLocal(isolate));

  SerializedScriptValue::DeserializeOptions options;
  options.message_ports = ports_.Get();
  v8::Local<v8::Value> value = data_->Deserialize(isolate, options);
  relevant_data.Set(isolate, value);
  return ScriptValue(script_state, value);
}

void PortalActivateEvent::Trace(blink::Visitor* visitor) {
  Event::Trace(visitor);
  visitor->Trace(document_);
  visitor->Trace(data_);
  visitor->Trace(v8_data_);
  visitor->Trace(ports_);
}

const AtomicString& PortalActivateEvent::InterfaceName() const {
  return event_type_names::kPortalactivate;
}

HTMLPortalElement* PortalActivateEvent::adoptPredecessor(
    ExceptionState& exception_state) {
  if (!predecessor_portal_ptr_) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kInvalidStateError,
        "The PortalActivateEvent is not associated with a predecessor browsing "
        "context");
    return nullptr;
  }

  HTMLPortalElement* portal = MakeGarbageCollected<HTMLPortalElement>(
      *document_, predecessor_portal_token_,
      std::move(predecessor_portal_ptr_));
  return portal;
}

}  // namespace blink
