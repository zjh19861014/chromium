// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/streams/writable_stream_native.h"

#include "third_party/blink/renderer/bindings/core/v8/script_value.h"
#include "third_party/blink/renderer/core/streams/miscellaneous_operations.h"
#include "third_party/blink/renderer/core/streams/stream_promise_resolver.h"
#include "third_party/blink/renderer/core/streams/stream_script_function.h"
#include "third_party/blink/renderer/core/streams/writable_stream_default_controller.h"
#include "third_party/blink/renderer/core/streams/writable_stream_default_writer.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/bindings/to_v8.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/platform/heap/heap.h"
#include "third_party/blink/renderer/platform/heap/visitor.h"
#include "third_party/blink/renderer/platform/wtf/assertions.h"

// Implementation of WritableStream for Blink.  See
// https://streams.spec.whatwg.org/#ws. The implementation closely follows the
// standard, except where required for performance or integration with Blink.
// In particular, classes, methods and abstract operations are implemented in
// the same order as in the standard, to simplify side-by-side reading.

namespace blink {

// The PendingAbortRequest type corresponds to the Record {[[promise]],
// [[reason]], [[wasAlreadyErroring]]} from the standard.
class WritableStreamNative::PendingAbortRequest final
    : public GarbageCollectedFinalized<PendingAbortRequest> {
 public:
  PendingAbortRequest(v8::Isolate* isolate,
                      StreamPromiseResolver* promise,
                      v8::Local<v8::Value> reason,
                      bool was_already_erroring)
      : promise_(promise),
        reason_(isolate, reason),
        was_already_erroring_(was_already_erroring) {}

  StreamPromiseResolver* GetPromise() { return promise_; }
  v8::Local<v8::Value> Reason(v8::Isolate* isolate) {
    return reason_.NewLocal(isolate);
  }

  bool WasAlreadyErroring() { return was_already_erroring_; }

  void Trace(Visitor* visitor) {
    visitor->Trace(promise_);
    visitor->Trace(reason_);
  }

 private:
  Member<StreamPromiseResolver> promise_;
  TraceWrapperV8Reference<v8::Value> reason_;
  const bool was_already_erroring_;

  DISALLOW_COPY_AND_ASSIGN(PendingAbortRequest);
};

WritableStreamNative::WritableStreamNative() = default;

WritableStreamNative::WritableStreamNative(ScriptState* script_state,
                                           ScriptValue raw_underlying_sink,
                                           ScriptValue raw_strategy,
                                           ExceptionState& exception_state) {
  // The first parts of this constructor correspond to the object conversions
  // that are implicit in the definition in the standard:
  // https://streams.spec.whatwg.org/#ws-constructor
  DCHECK(!raw_underlying_sink.IsEmpty());
  DCHECK(!raw_strategy.IsEmpty());

  auto context = script_state->GetContext();
  auto* isolate = script_state->GetIsolate();

  v8::Local<v8::Object> underlying_sink;
  ScriptValueToObject(script_state, raw_underlying_sink, &underlying_sink,
                      exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 2. Let size be ? GetV(strategy, "size").
  // 3. Let highWaterMark be ? GetV(strategy, "highWaterMark").
  StrategyUnpacker strategy_unpacker(script_state, raw_strategy,
                                     exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 4. Let type be ? GetV(underlyingSink, "type").
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> type;
  if (!underlying_sink->Get(context, V8AtomicString(isolate, "type"))
           .ToLocal(&type)) {
    exception_state.RethrowV8Exception(try_catch.Exception());
    return;
  }

  // 5. If type is not undefined, throw a RangeError exception.
  if (!type->IsUndefined()) {
    exception_state.ThrowRangeError("Invalid type is specified");
    return;
  }

  // 6. Let sizeAlgorithm be ? MakeSizeAlgorithmFromSizeFunction(size).
  auto* size_algorithm =
      strategy_unpacker.MakeSizeAlgorithm(script_state, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  DCHECK(size_algorithm);

  // 7. If highWaterMark is undefined, let highWaterMark be 1.
  // 8. Set highWaterMark to ? ValidateAndNormalizeHighWaterMark(highWaterMark).
  double high_water_mark =
      strategy_unpacker.GetHighWaterMark(script_state, 1, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 9. Perform ? SetUpWritableStreamDefaultControllerFromUnderlyingSink(this,
  //    underlyingSink, highWaterMark, sizeAlgorithm).
  WritableStreamDefaultController::SetUpFromUnderlyingSink(
      script_state, this, underlying_sink, high_water_mark, size_algorithm,
      exception_state);
}

WritableStreamNative::~WritableStreamNative() = default;

bool WritableStreamNative::locked(ScriptState* script_state,
                                  ExceptionState& exception_state) const {
  // https://streams.spec.whatwg.org/#ws-locked
  // 2. Return ! IsWritableStreamLocked(this).
  return IsLocked(this);
}

ScriptPromise WritableStreamNative::abort(ScriptState* script_state,
                                          ExceptionState& exception_state) {
  return abort(
      script_state,
      ScriptValue(script_state, v8::Undefined(script_state->GetIsolate())),
      exception_state);
}

ScriptPromise WritableStreamNative::abort(ScriptState* script_state,
                                          ScriptValue reason,
                                          ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#ws-abort
  //  2. If ! IsWritableStreamLocked(this) is true, return a promise rejected
  //     with a TypeError exception.
  if (IsLocked(this)) {
    exception_state.ThrowTypeError("Cannot abort a locked stream");
    return ScriptPromise();
  }

  //  3. Return ! WritableStreamAbort(this, reason).
  return ScriptPromise(script_state,
                       Abort(script_state, this, reason.V8Value()));
}

ScriptValue WritableStreamNative::getWriter(ScriptState* script_state,
                                            ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#ws-get-writer
  //  2. Return ? AcquireWritableStreamDefaultWriter(this).

  auto* writer = AcquireDefaultWriter(script_state, this, exception_state);
  if (exception_state.HadException()) {
    return ScriptValue();
  }
  DCHECK(writer);

  // This call to ToV8() is only reached directly from JavaScript, and so
  // shouldn't be called while the execution context is being shutdown and so
  // shouldn't fail.
  return ScriptValue(script_state, ToV8(writer, script_state));
}

// General Writable Stream Abstract Operations

WritableStreamNative* WritableStreamNative::Create(
    ScriptState* script_state,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* write_algorithm,
    StreamAlgorithm* close_algorithm,
    StreamAlgorithm* abort_algorithm,
    double high_water_mark,
    StrategySizeAlgorithm* size_algorithm,
    ExceptionState& exception_state) {
  DCHECK(size_algorithm);

  // https://streams.spec.whatwg.org/#create-writable-stream
  //  3. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
  DCHECK_GE(high_water_mark, 0);

  //  4. Let stream be ObjectCreate(the original value of WritableStream's
  //     prototype property).
  //  5. Perform ! InitializeWritableStream(stream).
  auto* stream = MakeGarbageCollected<WritableStreamNative>();

  //  6. Let controller be ObjectCreate(the original value of
  //     WritableStreamDefaultController's prototype property).
  auto* controller = MakeGarbageCollected<WritableStreamDefaultController>();

  //  7. Perform ? SetUpWritableStreamDefaultController(stream, controller,
  //     startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm,
  //     highWaterMark, sizeAlgorithm).
  WritableStreamDefaultController::SetUp(
      script_state, stream, controller, start_algorithm, write_algorithm,
      close_algorithm, abort_algorithm, high_water_mark, size_algorithm,
      exception_state);

  //  8. Return stream.
  return stream;
}

WritableStreamDefaultWriter* WritableStreamNative::AcquireDefaultWriter(
    ScriptState* script_state,
    WritableStreamNative* stream,
    ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#acquire-writable-stream-default-writer
  //  1. Return ? Construct(WritableStreamDefaultWriter, « stream »).
  auto* writer = MakeGarbageCollected<WritableStreamDefaultWriter>(
      script_state, stream, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return writer;
}

v8::Local<v8::Promise> WritableStreamNative::Abort(
    ScriptState* script_state,
    WritableStreamNative* stream,
    v8::Local<v8::Value> reason) {
  // https://streams.spec.whatwg.org/#writable-stream-abort
  //  1. Let state be stream.[[state]].
  const auto state = stream->state_;

  //  2. If state is "closed" or "errored", return a promise resolved with
  //     undefined.
  if (state == kClosed || state == kErrored) {
    return PromiseResolveWithUndefined(script_state);
  }

  //  3. If stream.[[pendingAbortRequest]] is not undefined, return
  //     stream.[[pendingAbortRequest]].[[promise]].
  auto* isolate = script_state->GetIsolate();
  if (stream->pending_abort_request_) {
    return stream->pending_abort_request_->GetPromise()->V8Promise(isolate);
  }

  //  4. Assert: state is "writable" or "erroring".
  DCHECK(state == kWritable || state == kErroring);

  //  5. Let wasAlreadyErroring be false.
  //  6. If state is "erroring",
  //      a. Set wasAlreadyErroring to true.
  //      b. Set reason to undefined.
  const bool was_already_erroring = state == kErroring;
  if (was_already_erroring) {
    reason = v8::Undefined(isolate);
  }

  //  7. Let promise be a new promise.
  auto* promise = MakeGarbageCollected<StreamPromiseResolver>(script_state);

  //  8. Set stream.[[pendingAbortRequest]] to Record {[[promise]]: promise,
  //     [[reason]]: reason, [[wasAlreadyErroring]]: wasAlreadyErroring}.
  stream->pending_abort_request_ = MakeGarbageCollected<PendingAbortRequest>(
      isolate, promise, reason, was_already_erroring);

  //  9. If wasAlreadyErroring is false, perform ! WritableStreamStartErroring(
  //     stream, reason).
  if (!was_already_erroring) {
    StartErroring(script_state, stream, reason);
  }

  // 10. Return promise.
  return promise->V8Promise(isolate);
}

// Writable Stream Abstract Operations Used by Controllers

v8::Local<v8::Promise> WritableStreamNative::AddWriteRequest(
    ScriptState* script_state,
    WritableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-add-write-request
  //  1. Assert: ! IsWritableStreamLocked(stream) is true.
  DCHECK(IsLocked(stream));

  //  2. Assert: stream.[[state]] is "writable".
  DCHECK_EQ(stream->state_, kWritable);

  //  3. Let promise be a new promise.
  auto* promise = MakeGarbageCollected<StreamPromiseResolver>(script_state);

  //  4. Append promise as the last element of stream.[[writeRequests]]
  stream->write_requests_.push_back(promise);

  //  5. Return promise.
  return promise->V8Promise(script_state->GetIsolate());
}

bool WritableStreamNative::CloseQueuedOrInFlight(
    const WritableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-close-queued-or-in-flight
  //  1. If stream.[[closeRequest]] is undefined and
  //     stream.[[inFlightCloseRequest]] is undefined, return false.
  //  2. Return true.
  return stream->close_request_ || stream->in_flight_close_request_;
}

void WritableStreamNative::DealWithRejection(ScriptState* script_state,
                                             WritableStreamNative* stream,
                                             v8::Local<v8::Value> error) {
  // https://streams.spec.whatwg.org/#writable-stream-deal-with-rejection
  //  1. Let state be stream.[[state]].
  const auto state = stream->state_;

  //  2. If state is "writable",
  if (state == kWritable) {
    //      a. Perform ! WritableStreamStartErroring(stream, error).
    StartErroring(script_state, stream, error);

    //      b. Return.
    return;
  }

  //  3. Assert: state is "erroring".
  DCHECK_EQ(state, kErroring);

  //  4. Perform ! WritableStreamFinishErroring(stream).
  FinishErroring(script_state, stream);
}

void WritableStreamNative::StartErroring(ScriptState* script_state,
                                         WritableStreamNative* stream,
                                         v8::Local<v8::Value> reason) {
  // https://streams.spec.whatwg.org/#writable-stream-start-erroring
  //  1. Assert: stream.[[storedError]] is undefined.
  DCHECK(stream->stored_error_.IsEmpty());

  //  2. Assert: stream.[[state]] is "writable".
  DCHECK_EQ(stream->state_, kWritable);

  //  3. Let controller be stream.[[writableStreamController]].
  WritableStreamDefaultController* controller =
      stream->writable_stream_controller_;

  //  4. Assert: controller is not undefined.
  DCHECK(controller);

  //  5. Set stream.[[state]] to "erroring".
  stream->state_ = kErroring;

  //  6. Set stream.[[storedError]] to reason.
  stream->stored_error_.Set(script_state->GetIsolate(), reason);

  //  7. Let writer be stream.[[writer]].
  WritableStreamDefaultWriter* writer = stream->writer_;

  //  8. If writer is not undefined, perform !
  //     WritableStreamDefaultWriterEnsureReadyPromiseRejected(writer, reason).
  if (writer) {
    WritableStreamDefaultWriter::EnsureReadyPromiseRejected(script_state,
                                                            writer, reason);
  }

  //  9. If ! WritableStreamHasOperationMarkedInFlight(stream) is false and
  //     controller.[[started]] is true, perform !
  //     WritableStreamFinishErroring(stream).
  if (!HasOperationMarkedInFlight(stream) && controller->Started()) {
    FinishErroring(script_state, stream);
  }
}

void WritableStreamNative::FinishErroring(ScriptState* script_state,
                                          WritableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-finish-erroring
  //  1. Assert: stream.[[state]] is "erroring".
  DCHECK_EQ(stream->state_, kErroring);

  //  2. Assert: ! WritableStreamHasOperationMarkedInFlight(stream) is false.
  DCHECK(!HasOperationMarkedInFlight(stream));

  //  3. Set stream.[[state]] to "errored".
  stream->state_ = kErrored;

  //  4. Perform ! stream.[[writableStreamController]].[[ErrorSteps]]().
  stream->writable_stream_controller_->ErrorSteps();

  //  5. Let storedError be stream.[[storedError]].
  auto* isolate = script_state->GetIsolate();
  const auto stored_error = stream->stored_error_.NewLocal(isolate);

  //  6. Repeat for each writeRequest that is an element of
  //     stream.[[writeRequests]],
  //      a. Reject writeRequest with storedError.
  RejectPromises(script_state, &stream->write_requests_, stored_error);

  //  7. Set stream.[[writeRequests]] to an empty List.
  stream->write_requests_.clear();

  //  8. If stream.[[pendingAbortRequest]] is undefined,
  if (!stream->pending_abort_request_) {
    //      a. Perform !
    //         WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
    RejectCloseAndClosedPromiseIfNeeded(script_state, stream);

    //      b. Return.
    return;
  }

  //  9. Let abortRequest be stream.[[pendingAbortRequest]].
  auto* abort_request = stream->pending_abort_request_.Get();

  // 10. Set stream.[[pendingAbortRequest]] to undefined.
  stream->pending_abort_request_ = nullptr;

  // 11. If abortRequest.[[wasAlreadyErroring]] is true,
  if (abort_request->WasAlreadyErroring()) {
    //      a. Reject abortRequest.[[promise]] with storedError.
    abort_request->GetPromise()->Reject(script_state, stored_error);

    //      b. Perform !
    //         WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream)
    RejectCloseAndClosedPromiseIfNeeded(script_state, stream);

    //      c. Return.
    return;
  }

  // 12. Let promise be ! stream.[[writableStreamController]].[[AbortSteps]](
  //     abortRequest.[[reason]]).
  auto promise = stream->writable_stream_controller_->AbortSteps(
      script_state, abort_request->Reason(isolate));

  class ResolvePromiseFunction final : public StreamScriptFunction {
   public:
    ResolvePromiseFunction(ScriptState* script_state,
                           WritableStreamNative* stream,
                           StreamPromiseResolver* promise)
        : StreamScriptFunction(script_state),
          stream_(stream),
          promise_(promise) {}

    void CallWithLocal(v8::Local<v8::Value>) override {
      // 13. Upon fulfillment of promise,
      //      a. Resolve abortRequest.[[promise]] with undefined.
      promise_->ResolveWithUndefined(GetScriptState());

      //      b. Perform !
      //         WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
      RejectCloseAndClosedPromiseIfNeeded(GetScriptState(), stream_);
    }

    void Trace(Visitor* visitor) override {
      visitor->Trace(stream_);
      visitor->Trace(promise_);
      StreamScriptFunction::Trace(visitor);
    }

   private:
    Member<WritableStreamNative> stream_;
    Member<StreamPromiseResolver> promise_;
  };

  class RejectPromiseFunction final : public StreamScriptFunction {
   public:
    RejectPromiseFunction(ScriptState* script_state,
                          WritableStreamNative* stream,
                          StreamPromiseResolver* promise)
        : StreamScriptFunction(script_state),
          stream_(stream),
          promise_(promise) {}

    void CallWithLocal(v8::Local<v8::Value> reason) override {
      // 14. Upon rejection of promise with reason reason,
      //      a. Reject abortRequest.[[promise]] with reason.
      promise_->Reject(GetScriptState(), reason);

      //      b. Perform !
      //         WritableStreamRejectCloseAndClosedPromiseIfNeeded(stream).
      RejectCloseAndClosedPromiseIfNeeded(GetScriptState(), stream_);
    }

    void Trace(Visitor* visitor) override {
      visitor->Trace(stream_);
      visitor->Trace(promise_);
      StreamScriptFunction::Trace(visitor);
    }

   private:
    Member<WritableStreamNative> stream_;
    Member<StreamPromiseResolver> promise_;
  };

  StreamThenPromise(script_state->GetContext(), promise,
                    MakeGarbageCollected<ResolvePromiseFunction>(
                        script_state, stream, abort_request->GetPromise()),
                    MakeGarbageCollected<RejectPromiseFunction>(
                        script_state, stream, abort_request->GetPromise()));
}

void WritableStreamNative::FinishInFlightWrite(ScriptState* script_state,
                                               WritableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-write
  //  1. Assert: stream.[[inFlightWriteRequest]] is not undefined.
  DCHECK(stream->in_flight_write_request_);

  //  2. Resolve stream.[[inFlightWriteRequest]] with undefined.
  stream->in_flight_write_request_->ResolveWithUndefined(script_state);

  //  3. Set stream.[[inFlightWriteRequest]] to undefined.
  stream->in_flight_write_request_ = nullptr;
}

void WritableStreamNative::FinishInFlightWriteWithError(
    ScriptState* script_state,
    WritableStreamNative* stream,
    v8::Local<v8::Value> error) {
  // https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-write-with-error
  //  1. Assert: stream.[[inFlightWriteRequest]] is not undefined.
  DCHECK(stream->in_flight_write_request_);

  //  2. Reject stream.[[inFlightWriteRequest]] with error.
  stream->in_flight_write_request_->Reject(script_state, error);

  //  3. Set stream.[[inFlightWriteRequest]] to undefined.
  stream->in_flight_write_request_ = nullptr;

  //  4. Assert: stream.[[state]] is "writable" or "erroring".
  const auto state = stream->state_;
  DCHECK(state == kWritable || state == kErroring);

  //  5. Perform ! WritableStreamDealWithRejection(stream, error).
  DealWithRejection(script_state, stream, error);
}

void WritableStreamNative::FinishInFlightClose(ScriptState* script_state,
                                               WritableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-close
  //  1. Assert: stream.[[inFlightCloseRequest]] is not undefined.
  DCHECK(stream->in_flight_close_request_);

  //  2. Resolve stream.[[inFlightCloseRequest]] with undefined.
  stream->in_flight_close_request_->ResolveWithUndefined(script_state);

  //  3. Set stream.[[inFlightCloseRequest]] to undefined.
  stream->in_flight_close_request_ = nullptr;

  //  4. Let state be stream.[[state]].
  const auto state = stream->state_;

  //  5. Assert: stream.[[state]] is "writable" or "erroring".
  DCHECK(state == kWritable || state == kErroring);

  //  6. If state is "erroring",
  if (state == kErroring) {
    //      a. Set stream.[[storedError]] to undefined.
    stream->stored_error_.Clear();

    //      b. If stream.[[pendingAbortRequest]] is not undefined,
    if (stream->pending_abort_request_) {
      //          i. Resolve stream.[[pendingAbortRequest]].[[promise]] with
      //             undefined.
      stream->pending_abort_request_->GetPromise()->ResolveWithUndefined(
          script_state);

      //         ii. Set stream.[[pendingAbortRequest]] to undefined.
      stream->pending_abort_request_ = nullptr;
    }
  }

  //  7. Set stream.[[state]] to "closed".
  stream->state_ = kClosed;

  //  8. Let writer be stream.[[writer]].
  const auto writer = stream->writer_;

  //  9. If writer is not undefined, resolve writer.[[closedPromise]] with
  //     undefined.
  if (writer) {
    writer->ClosedPromise()->ResolveWithUndefined(script_state);
  }

  // 10. Assert: stream.[[pendingAbortRequest]] is undefined.
  DCHECK(!stream->pending_abort_request_);

  // 11. Assert: stream.[[storedError]] is undefined.
  DCHECK(stream->stored_error_.IsEmpty());
}

void WritableStreamNative::FinishInFlightCloseWithError(
    ScriptState* script_state,
    WritableStreamNative* stream,
    v8::Local<v8::Value> error) {
  // https://streams.spec.whatwg.org/#writable-stream-finish-in-flight-close-with-error
  //  1. Assert: stream.[[inFlightCloseRequest]] is not undefined.
  DCHECK(stream->in_flight_close_request_);

  //  2. Reject stream.[[inFlightCloseRequest]] with error.
  stream->in_flight_close_request_->Reject(script_state, error);

  //  3. Set stream.[[inFlightCloseRequest]] to undefined.
  stream->in_flight_close_request_ = nullptr;

  //  4. Assert: stream.[[state]] is "writable" or "erroring".
  const auto state = stream->state_;
  DCHECK(state == kWritable || state == kErroring);

  //  5. If stream.[[pendingAbortRequest]] is not undefined,
  if (stream->pending_abort_request_) {
    //      a. Reject stream.[[pendingAbortRequest]].[[promise]] with error.
    stream->pending_abort_request_->GetPromise()->Reject(script_state, error);

    //      b. Set stream.[[pendingAbortRequest]] to undefined.
    stream->pending_abort_request_ = nullptr;
  }

  //  6. Perform ! WritableStreamDealWithRejection(stream, error).
  DealWithRejection(script_state, stream, error);
}

void WritableStreamNative::MarkCloseRequestInFlight(
    WritableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-mark-close-request-in-flight
  //  1. Assert: stream.[[inFlightCloseRequest]] is undefined.
  DCHECK(!stream->in_flight_close_request_);

  //  2. Assert: stream.[[closeRequest]] is not undefined.
  DCHECK(stream->close_request_);

  //  3. Set stream.[[inFlightCloseRequest]] to stream.[[closeRequest]].
  stream->in_flight_close_request_ = stream->close_request_;

  //  4. Set stream.[[closeRequest]] to undefined.
  stream->close_request_ = nullptr;
}

void WritableStreamNative::MarkFirstWriteRequestInFlight(
    WritableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-mark-first-write-request-in-flight
  //  1. Assert: stream.[[inFlightWriteRequest]] is undefined.
  DCHECK(!stream->in_flight_write_request_);

  //  2. Assert: stream.[[writeRequests]] is not empty.
  DCHECK(!stream->write_requests_.empty());

  //  3. Let writeRequest be the first element of stream.[[writeRequests]].
  StreamPromiseResolver* write_request = stream->write_requests_.front();

  //  4. Remove writeRequest from stream.[[writeRequests]], shifting all other
  //     elements downward (so that the second becomes the first, and so on).
  stream->write_requests_.pop_front();

  //  5. Set stream.[[inFlightWriteRequest]] to writeRequest.
  stream->in_flight_write_request_ = write_request;
}

void WritableStreamNative::UpdateBackpressure(ScriptState* script_state,
                                              WritableStreamNative* stream,
                                              bool backpressure) {
  // https://streams.spec.whatwg.org/#writable-stream-update-backpressure
  //  1. Assert: stream.[[state]] is "writable".
  DCHECK_EQ(stream->state_, kWritable);

  //  2. Assert: ! WritableStreamCloseQueuedOrInFlight(stream) is false.
  DCHECK(!CloseQueuedOrInFlight(stream));

  //  3. Let writer be stream.[[writer]].
  WritableStreamDefaultWriter* writer = stream->writer_;

  //  4. If writer is not undefined and backpressure is not
  //     stream.[[backpressure]],
  if (writer && backpressure != stream->has_backpressure_) {
    //      a. If backpressure is true, set writer.[[readyPromise]] to a new
    //         promise.
    if (backpressure) {
      writer->SetReadyPromise(
          MakeGarbageCollected<StreamPromiseResolver>(script_state));
    } else {
      //      b. Otherwise,
      //          i. Assert: backpressure is false.
      DCHECK(!backpressure);

      //         ii. Resolve writer.[[readyPromise]] with undefined.
      writer->ReadyPromise()->ResolveWithUndefined(script_state);
    }
  }

  //  5. Set stream.[[backpressure]] to backpressure.
  stream->has_backpressure_ = backpressure;
}

v8::Local<v8::Value> WritableStreamNative::GetStoredError(
    v8::Isolate* isolate) const {
  return stored_error_.NewLocal(isolate);
}

void WritableStreamNative::SetCloseRequest(
    StreamPromiseResolver* close_request) {
  close_request_ = close_request;
}

void WritableStreamNative::SetController(
    WritableStreamDefaultController* controller) {
  writable_stream_controller_ = controller;
}

void WritableStreamNative::SetWriter(WritableStreamDefaultWriter* writer) {
  writer_ = writer;
}

void WritableStreamNative::Trace(Visitor* visitor) {
  visitor->Trace(close_request_);
  visitor->Trace(in_flight_write_request_);
  visitor->Trace(in_flight_close_request_);
  visitor->Trace(pending_abort_request_);
  visitor->Trace(stored_error_);
  visitor->Trace(writable_stream_controller_);
  visitor->Trace(writer_);
  visitor->Trace(write_requests_);
  WritableStream::Trace(visitor);
}

bool WritableStreamNative::HasOperationMarkedInFlight(
    const WritableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-has-operation-marked-in-flight
  //  1. If stream.[[inFlightWriteRequest]] is undefined and
  //     controller.[[inFlightCloseRequest]] is undefined, return false.
  //  2. Return true.
  return stream->in_flight_write_request_ || stream->in_flight_close_request_;
}

void WritableStreamNative::RejectCloseAndClosedPromiseIfNeeded(
    ScriptState* script_state,
    WritableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#writable-stream-reject-close-and-closed-promise-if-needed
  // //  1. Assert: stream.[[state]] is "errored".
  DCHECK_EQ(stream->state_, kErrored);

  auto* isolate = script_state->GetIsolate();

  //  2. If stream.[[closeRequest]] is not undefined,
  if (stream->close_request_) {
    //      a. Assert: stream.[[inFlightCloseRequest]] is undefined.
    DCHECK(!stream->in_flight_close_request_);

    //      b. Reject stream.[[closeRequest]] with stream.[[storedError]].
    stream->close_request_->Reject(script_state,
                                   stream->stored_error_.NewLocal(isolate));

    //      c. Set stream.[[closeRequest]] to undefined.
    stream->close_request_ = nullptr;
  }

  //  3. Let writer be stream.[[writer]].
  const auto writer = stream->writer_;

  //  4. If writer is not undefined,
  if (writer) {
    //      a. Reject writer.[[closedPromise]] with stream.[[storedError]].
    writer->ClosedPromise()->Reject(script_state,
                                    stream->stored_error_.NewLocal(isolate));

    //      b. Set writer.[[closedPromise]].[[PromiseIsHandled]] to true.
    writer->ClosedPromise()->MarkAsHandled(isolate);
  }
}

// TODO(ricea): Functions for transferable streams.

// Utility functions (not from the standard).

void WritableStreamNative::RejectPromises(ScriptState* script_state,
                                          PromiseQueue* queue,
                                          v8::Local<v8::Value> e) {
  for (auto promise : *queue) {
    promise->Reject(script_state, e);
  }
}

}  // namespace blink
