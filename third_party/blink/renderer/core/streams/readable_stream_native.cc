// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/streams/readable_stream_native.h"

#include "base/stl_util.h"
#include "third_party/blink/renderer/bindings/core/v8/script_function.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_iterator_result_value.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/use_counter.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/core/streams/miscellaneous_operations.h"
#include "third_party/blink/renderer/core/streams/readable_stream_default_controller.h"
#include "third_party/blink/renderer/core/streams/readable_stream_default_reader.h"
#include "third_party/blink/renderer/core/streams/stream_algorithms.h"
#include "third_party/blink/renderer/core/streams/stream_promise_resolver.h"
#include "third_party/blink/renderer/core/streams/stream_script_function.h"
#include "third_party/blink/renderer/core/streams/underlying_source_base.h"
#include "third_party/blink/renderer/core/streams/writable_stream_default_controller.h"
#include "third_party/blink/renderer/core/streams/writable_stream_default_writer.h"
#include "third_party/blink/renderer/core/streams/writable_stream_native.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/visitor.h"
#include "third_party/blink/renderer/platform/wtf/assertions.h"
#include "third_party/blink/renderer/platform/wtf/deque.h"

namespace blink {

struct ReadableStreamNative::PipeOptions {
  PipeOptions() = default;
  bool prevent_close = false;
  bool prevent_abort = false;
  bool prevent_cancel = false;
};

// PipeToEngine implements PipeTo(). All standard steps in this class come from
// https://streams.spec.whatwg.org/#readable-stream-pipe-to
//
// This implementation is simple but suboptimal because it uses V8 promises to
// drive its asynchronous state machine, allocating a lot of temporary V8
// objects as a result.
//
// TODO(ricea): Create internal versions of ReadableStreamDefaultReader::Read()
// and WritableStreamDefaultWriter::Write() to bypass promise creation and so
// reduce the number of allocations on the hot path.
class ReadableStreamNative::PipeToEngine
    : public GarbageCollectedFinalized<PipeToEngine> {
 public:
  PipeToEngine(ScriptState* script_state, PipeOptions pipe_options)
      : script_state_(script_state), pipe_options_(pipe_options) {}

  // This is the main entrypoint for ReadableStreamPipeTo().
  ScriptPromise Start(ReadableStreamNative* readable,
                      WritableStreamNative* destination) {
    // 1. Assert: ! IsReadableStream(source) is true.
    DCHECK(readable);

    // 2. Assert: ! IsWritableStream(dest) is true.
    DCHECK(destination);

    // Not relevant to C++ implementation:
    // 3. Assert: Type(preventClose) is Boolean, Type(preventAbort) is Boolean,
    //    and Type(preventCancel) is Boolean.

    // TODO(ricea): Implement |signal|.
    // 4. Assert: signal is undefined or signal is an instance of the
    //    AbortSignal interface.

    // 5. Assert: ! IsReadableStreamLocked(source) is false.
    DCHECK(!ReadableStreamNative::IsLocked(readable));

    // 6. Assert: ! IsWritableStreamLocked(dest) is false.
    DCHECK(!WritableStreamNative::IsLocked(destination));

    auto* isolate = script_state_->GetIsolate();
    ExceptionState exception_state(isolate, ExceptionState::kUnknownContext, "",
                                   "");

    // 7. If !
    //    IsReadableByteStreamController(source.[[readableStreamController]]) is
    //    true, let reader be either ! AcquireReadableStreamBYOBReader(source)
    //    or ! AcquireReadableStreamDefaultReader(source), at the user agent’s
    //    discretion.
    // 8. Otherwise, let reader be ! AcquireReadableStreamDefaultReader(source).
    reader_ = ReadableStreamNative::AcquireDefaultReader(
        script_state_, readable, false, exception_state);
    DCHECK(!exception_state.HadException());

    // 9. Let writer be ! AcquireWritableStreamDefaultWriter(dest).
    writer_ = WritableStreamNative::AcquireDefaultWriter(
        script_state_, destination, exception_state);
    DCHECK(!exception_state.HadException());

    // 10. Let shuttingDown be false.
    DCHECK(!is_shutting_down_);

    // 11. Let promise be a new promise.
    promise_ = MakeGarbageCollected<StreamPromiseResolver>(script_state_);

    // TODO(ricea): Implement abort:
    // 12. If signal is not undefined, ...

    // 13. In parallel ...
    // The rest of the algorithm is described in terms of a series of
    // constraints rather than as explicit steps.
    if (CheckInitialState()) {
      // Need to detect closing and error when we are not reading. This
      // corresponds to the following conditions from the standard:
      //     1. Errors must be propagated forward: if source.[[state]] is or
      //        becomes "errored", ...
      // and
      //     3. Closing must be propagated forward: if source.[[state]] is or
      //        becomes "closed", ...
      ThenPromise(reader_->ClosedPromise()->V8Promise(isolate),
                  &PipeToEngine::OnReaderClosed, &PipeToEngine::ReadableError);

      // Need to detect error when we are not writing. This corresponds to this
      // condition from the standard:
      //    2. Errors must be propagated backward: if dest.[[state]] is or
      //       becomes "errored", ...
      // We do not need to detect closure of the writable end of the pipe,
      // because we have it locked and so it can only be closed by us.
      ThenPromise(writer_->ClosedPromise()->V8Promise(isolate), nullptr,
                  &PipeToEngine::WritableError);

      // Start the main read / write loop.
      HandleNextEvent(Undefined());
    }

    // 14. Return promise.
    return promise_->GetScriptPromise(script_state_);
  }

  StreamPromiseResolver* Promise() { return promise_; }

  void Trace(Visitor* visitor) {
    visitor->Trace(script_state_);
    visitor->Trace(reader_);
    visitor->Trace(writer_);
    visitor->Trace(promise_);
    visitor->Trace(last_write_);
    visitor->Trace(shutdown_error_);
  }

 private:
  // The implementation uses method pointers to maximise code reuse.

  // |Action| represents an action that can be passed to the "Shutdown with an
  // action" operation. Each Action is implemented as a method which delegates
  // to some abstract operation, inferring the arguments from the state of
  // |this|.
  using Action = v8::Local<v8::Promise> (PipeToEngine::*)();

  // This implementation uses ThenPromise() 7 times. Instead of creating a dozen
  // separate subclasses of ScriptFunction, we use a single implementation and
  // pass a method pointer at runtime to control the behaviour. Most
  // PromiseReaction methods don't need to return a value, but because some do,
  // the rest have to return undefined so that they can have the same method
  // signature. Similarly, many of the methods ignore the argument that is
  // passed to them.
  using PromiseReaction =
      v8::Local<v8::Value> (PipeToEngine::*)(v8::Local<v8::Value>);

  class WrappedPromiseReaction : public ScriptFunction {
   public:
    static v8::Local<v8::Function> Create(ScriptState* script_state,
                                          PipeToEngine* instance,
                                          PromiseReaction method) {
      auto* reaction = MakeGarbageCollected<WrappedPromiseReaction>(
          script_state, instance, method);
      return reaction->BindToV8Function();
    }

    WrappedPromiseReaction(ScriptState* script_state,
                           PipeToEngine* instance,
                           PromiseReaction method)
        : ScriptFunction(script_state), instance_(instance), method_(method) {}

    void CallRaw(const v8::FunctionCallbackInfo<v8::Value>& args) override {
      DCHECK_EQ(args.Length(), 1);
      auto result = (instance_->*method_)(args[0]);
      args.GetReturnValue().Set(result);
    }

    void Trace(Visitor* visitor) override {
      visitor->Trace(instance_);
      ScriptFunction::Trace(visitor);
    }

   private:
    Member<PipeToEngine> instance_;
    PromiseReaction method_;
  };

  // Checks the state of the streams and executes the shutdown handlers if
  // necessary. Returns true if piping can continue.
  bool CheckInitialState() {
    auto* isolate = script_state_->GetIsolate();
    const auto state = Readable()->state_;

    // Both streams can be errored or closed. To perform the right action the
    // order of the checks must match the standard: "the following conditions
    // must be applied in order." This method only checks the initial state;
    // detection of state changes elsewhere is done through checking promise
    // reactions.

    // a. Errors must be propagated forward: if source.[[state]] is or
    //    becomes "errored",
    if (state == kErrored) {
      ReadableError(Readable()->GetStoredError(isolate));
      return false;
    }

    // 2. Errors must be propagated backward: if dest.[[state]] is or becomes
    //    "errored",
    if (Destination()->IsErrored()) {
      WritableError(Destination()->GetStoredError(isolate));
      return false;
    }

    // 3. Closing must be propagated forward: if source.[[state]] is or
    //    becomes "closed", then
    if (state == kClosed) {
      ReadableClosed();
      return false;
    }

    // 4. Closing must be propagated backward: if !
    //    WritableStreamCloseQueuedOrInFlight(dest) is true or dest.[[state]]
    //    is "closed",
    if (Destination()->IsClosingOrClosed()) {
      WritableStartedClosed();
      return false;
    }

    return true;
  }

  // HandleNextEvent() has an unused argument and return value because it is a
  // PromiseReaction. HandleNextEvent() and ReadFulfilled() call each other
  // asynchronously in a loop until the pipe completes.
  v8::Local<v8::Value> HandleNextEvent(v8::Local<v8::Value>) {
    DCHECK(!is_reading_);
    if (is_shutting_down_) {
      return Undefined();
    }

    base::Optional<double> desired_size = writer_->GetDesiredSizeInternal();
    if (!desired_size.has_value()) {
      // This can happen if abort() is queued but not yet started when
      // pipeTo() is called. In that case [[storedError]] is not set yet, and
      // we need to wait until it is before we can cancel the pipe. Once
      // [[storedError]] has been set, the rejection handler set on the writer
      // closed promise above will detect it, so all we need to do here is
      // nothing.
      return Undefined();
    }

    if (desired_size.value() <= 0) {
      // Need to wait for backpressure to go away.
      ThenPromise(
          writer_->ReadyPromise()->V8Promise(script_state_->GetIsolate()),
          &PipeToEngine::HandleNextEvent, &PipeToEngine::WritableError);
      return Undefined();
    }

    is_reading_ = true;
    ThenPromise(ReadableStreamDefaultReader::Read(script_state_, reader_)
                    ->V8Promise(script_state_->GetIsolate()),
                &PipeToEngine::ReadFulfilled, &PipeToEngine::ReadRejected);
    return Undefined();
  }

  v8::Local<v8::Value> ReadFulfilled(v8::Local<v8::Value> result) {
    is_reading_ = false;
    DCHECK(result->IsObject());
    auto* isolate = script_state_->GetIsolate();
    v8::Local<v8::Value> value;
    bool done = false;
    bool unpack_succeeded =
        V8UnpackIteratorResult(script_state_, result.As<v8::Object>(), &done)
            .ToLocal(&value);
    DCHECK(unpack_succeeded);
    if (done) {
      ReadableClosed();
      return Undefined();
    }
    const auto write =
        WritableStreamDefaultWriter::Write(script_state_, writer_, value);
    last_write_.Set(isolate, write);
    ThenPromise(write, nullptr, &PipeToEngine::WritableError);
    HandleNextEvent(Undefined());
    return Undefined();
  }

  v8::Local<v8::Value> ReadRejected(v8::Local<v8::Value>) {
    is_reading_ = false;
    ReadableError(Readable()->GetStoredError(script_state_->GetIsolate()));
    return Undefined();
  }

  // If read() is in progress, then wait for it to tell us that the stream is
  // closed so that we write all the data before shutdown.
  v8::Local<v8::Value> OnReaderClosed(v8::Local<v8::Value>) {
    if (!is_reading_) {
      ReadableClosed();
    }
    return Undefined();
  }

  // 1. Errors must be propagated forward: if source.[[state]] is or
  //    becomes "errored", then
  v8::Local<v8::Value> ReadableError(v8::Local<v8::Value> error) {
    // This function can be called during shutdown when the lock is released.
    // Exit early in that case.
    if (is_shutting_down_) {
      return Undefined();
    }

    // a. If preventAbort is false, shutdown with an action of !
    //    WritableStreamAbort(dest, source.[[storedError]]) and with
    //    source.[[storedError]].
    DCHECK(error->SameValue(
        Readable()->GetStoredError(script_state_->GetIsolate())));
    if (!pipe_options_.prevent_abort) {
      ShutdownWithAction(&PipeToEngine::WritableStreamAbortAction, error);
    } else {
      // b. Otherwise, shutdown with source.[[storedError]].
      Shutdown(error);
    }
    return Undefined();
  }

  // 2. Errors must be propagated backward: if dest.[[state]] is or becomes
  //    "errored", then
  v8::Local<v8::Value> WritableError(v8::Local<v8::Value> error) {
    // This function can be called during shutdown when the lock is released.
    // Exit early in that case.
    if (is_shutting_down_) {
      return Undefined();
    }

    // a. If preventCancel is false, shutdown with an action of !
    //    ReadableStreamCancel(source, dest.[[storedError]]) and with
    //    dest.[[storedError]].
    DCHECK(error->SameValue(
        Destination()->GetStoredError(script_state_->GetIsolate())));
    if (!pipe_options_.prevent_cancel) {
      ShutdownWithAction(&PipeToEngine::ReadableStreamCancelAction, error);
    } else {
      // b. Otherwise, shutdown with dest.[[storedError]].
      Shutdown(error);
    }
    return Undefined();
  }

  // 3. Closing must be propagated forward: if source.[[state]] is or
  //    becomes "closed", then
  void ReadableClosed() {
    // a. If preventClose is false, shutdown with an action of !
    //    WritableStreamDefaultWriterCloseWithErrorPropagation(writer).
    if (!pipe_options_.prevent_close) {
      ShutdownWithAction(
          &PipeToEngine::
              WritableStreamDefaultWriterCloseWithErrorPropagationAction,
          v8::MaybeLocal<v8::Value>());
    } else {
      // b. Otherwise, shutdown.
      Shutdown(v8::MaybeLocal<v8::Value>());
    }
  }

  // 4. Closing must be propagated backward: if !
  //    WritableStreamCloseQueuedOrInFlight(dest) is true or dest.[[state]] is
  //    "closed", then
  void WritableStartedClosed() {
    // a. Assert: no chunks have been read or written.
    // This is trivially true because this method is only called from
    // CheckInitialState().

    // b. Let destClosed be a new TypeError.
    const auto dest_closed = v8::Exception::TypeError(
        V8String(script_state_->GetIsolate(), "Destination stream closed"));

    // c. If preventCancel is false, shutdown with an action of !
    //    ReadableStreamCancel(source, destClosed) and with destClosed.
    if (!pipe_options_.prevent_cancel) {
      ShutdownWithAction(&PipeToEngine::ReadableStreamCancelAction,
                         dest_closed);
    } else {
      // d. Otherwise, shutdown with destClosed.
      Shutdown(dest_closed);
    }
  }

  // * Shutdown with an action: if any of the above requirements ask to shutdown
  //   with an action |action|, optionally with an error |originalError|, then:
  void ShutdownWithAction(Action action,
                          v8::MaybeLocal<v8::Value> original_error) {
    // a. If shuttingDown is true, abort these substeps.
    if (is_shutting_down_) {
      return;
    }

    // b. Set shuttingDown to true.
    is_shutting_down_ = true;

    // Store the action in case we need to call it asynchronously. This is safe
    // because the |is_shutting_down_| guard flag ensures that we can only reach
    // this assignment once.
    shutdown_action_ = action;

    // Store |original_error| as |shutdown_error_| if it was supplied.
    v8::Local<v8::Value> original_error_local;
    if (original_error.ToLocal(&original_error_local)) {
      shutdown_error_.Set(script_state_->GetIsolate(), original_error_local);
    }
    v8::Local<v8::Promise> p;

    // c. If dest.[[state]] is "writable" and !
    //    WritableStreamCloseQueuedOrInFlight(dest) is false,
    if (ShouldWriteQueuedChunks()) {
      //  i. If any chunks have been read but not yet written, write them to
      //     dest.
      // ii. Wait until every chunk that has been read has been written
      //     (i.e. the corresponding promises have settled).
      p = ThenPromise(WriteQueuedChunks(), &PipeToEngine::InvokeShutdownAction);
    } else {
      // d. Let p be the result of performing action.
      p = InvokeShutdownAction();
    }

    // e. Upon fulfillment of p, finalize, passing along originalError if it
    //    was given.
    // f. Upon rejection of p with reason newError, finalize with newError.
    ThenPromise(p, &PipeToEngine::FinalizeWithOriginalErrorIfSet,
                &PipeToEngine::FinalizeWithNewError);
  }

  // * Shutdown: if any of the above requirements or steps ask to shutdown,
  //   optionally with an error error, then:
  void Shutdown(v8::MaybeLocal<v8::Value> error_maybe) {
    // a. If shuttingDown is true, abort these substeps.
    if (is_shutting_down_) {
      return;
    }

    // b. Set shuttingDown to true.
    is_shutting_down_ = true;

    // c. If dest.[[state]] is "writable" and !
    //    WritableStreamCloseQueuedOrInFlight(dest) is false,
    if (ShouldWriteQueuedChunks()) {
      // Need to stash the value of |error_maybe| since we are calling
      // Finalize() asynchronously.
      v8::Local<v8::Value> error;
      if (error_maybe.ToLocal(&error)) {
        shutdown_error_.Set(script_state_->GetIsolate(), error);
      }

      //  i. If any chunks have been read but not yet written, write them to
      //     dest.
      // ii. Wait until every chunk that has been read has been written
      //     (i.e. the corresponding promises have settled).
      // d. Finalize, passing along error if it was given.
      ThenPromise(WriteQueuedChunks(),
                  &PipeToEngine::FinalizeWithOriginalErrorIfSet);
    } else {
      // d. Finalize, passing along error if it was given.
      Finalize(error_maybe);
    }
  }

  // Calls Finalize(), using the stored shutdown error rather than the value
  // that was passed.
  v8::Local<v8::Value> FinalizeWithOriginalErrorIfSet(v8::Local<v8::Value>) {
    v8::MaybeLocal<v8::Value> error_maybe;
    if (!shutdown_error_.IsEmpty()) {
      error_maybe = shutdown_error_.NewLocal(script_state_->GetIsolate());
    }
    Finalize(error_maybe);
    return Undefined();
  }

  // Calls Finalize(), using the value that was passed as the error.
  v8::Local<v8::Value> FinalizeWithNewError(v8::Local<v8::Value> new_error) {
    Finalize(new_error);
    return Undefined();
  }

  // * Finalize: both forms of shutdown will eventually ask to finalize,
  //   optionally with an error error, which means to perform the following
  //   steps:
  void Finalize(v8::MaybeLocal<v8::Value> error_maybe) {
    // a. Perform ! WritableStreamDefaultWriterRelease(writer).
    WritableStreamDefaultWriter::Release(script_state_, writer_);

    // b. Perform ! ReadableStreamReaderGenericRelease(reader).
    ReadableStreamNative::ReaderGenericRelease(script_state_, reader_);

    // TODO(ricea): Implement signal.
    // c. If signal is not undefined, remove abortAlgorithm from signal.

    v8::Local<v8::Value> error;
    if (error_maybe.ToLocal(&error)) {
      // d. If error was given, reject promise with error.
      promise_->Reject(script_state_, error);
    } else {
      // e. Otherwise, resolve promise with undefined.
      promise_->ResolveWithUndefined(script_state_);
    }
  }

  bool ShouldWriteQueuedChunks() const {
    // "If dest.[[state]] is "writable" and !
    // WritableStreamCloseQueuedOrInFlight(dest) is false"
    return Destination()->IsWritable() &&
           !WritableStreamNative::CloseQueuedOrInFlight(Destination());
  }

  v8::Local<v8::Promise> WriteQueuedChunks() {
    if (!last_write_.IsEmpty()) {
      // "Wait until every chunk that has been read has been written (i.e.
      // the corresponding promises have settled)"
      // This implies that we behave the same whether the promise fulfills or
      // rejects. IgnoreErrors() will convert a rejection into a successful
      // resolution.
      return ThenPromise(last_write_.NewLocal(script_state_->GetIsolate()),
                         nullptr, &PipeToEngine::IgnoreErrors);
    }
    return PromiseResolveWithUndefined(script_state_);
  }

  v8::Local<v8::Value> IgnoreErrors(v8::Local<v8::Value>) {
    return Undefined();
  }

  // InvokeShutdownAction(), version for calling directly.
  v8::Local<v8::Promise> InvokeShutdownAction() {
    return (this->*shutdown_action_)();
  }

  // InvokeShutdownAction(), version for use as a PromiseReaction.
  v8::Local<v8::Value> InvokeShutdownAction(v8::Local<v8::Value>) {
    return InvokeShutdownAction();
  }

  v8::Local<v8::Value> ShutdownError() const {
    DCHECK(!shutdown_error_.IsEmpty());
    return shutdown_error_.NewLocal(script_state_->GetIsolate());
  }

  v8::Local<v8::Promise> WritableStreamAbortAction() {
    return WritableStreamNative::Abort(script_state_, Destination(),
                                       ShutdownError());
  }

  v8::Local<v8::Promise> ReadableStreamCancelAction() {
    return ReadableStreamNative::Cancel(script_state_, Readable(),
                                        ShutdownError());
  }

  v8::Local<v8::Promise>
  WritableStreamDefaultWriterCloseWithErrorPropagationAction() {
    return WritableStreamDefaultWriter::CloseWithErrorPropagation(script_state_,
                                                                  writer_);
  }

  // Reduces the visual noise when we are returning an undefined value.
  v8::Local<v8::Value> Undefined() {
    return v8::Undefined(script_state_->GetIsolate());
  }

  WritableStreamNative* Destination() { return writer_->OwnerWritableStream(); }

  const WritableStreamNative* Destination() const {
    return writer_->OwnerWritableStream();
  }

  ReadableStreamNative* Readable() { return reader_->owner_readable_stream_; }

  // Performs promise.then(on_fulfilled, on_rejected). The logic in this method
  // is identical to StreamPromiseThen(), but the types are different. It's not
  // possible to share the logic without using complex templates, which would
  // duplicate the object code anyway.
  v8::Local<v8::Promise> ThenPromise(v8::Local<v8::Promise> promise,
                                     PromiseReaction on_fulfilled,
                                     PromiseReaction on_rejected = nullptr) {
    auto context = script_state_->GetContext();
    v8::MaybeLocal<v8::Promise> result_maybe;
    if (!on_fulfilled) {
      DCHECK(on_rejected);
      result_maybe = promise->Catch(
          context,
          WrappedPromiseReaction::Create(script_state_, this, on_rejected));
    } else if (on_rejected) {
      result_maybe = promise->Then(
          context,
          WrappedPromiseReaction::Create(script_state_, this, on_fulfilled),
          WrappedPromiseReaction::Create(script_state_, this, on_rejected));
    } else {
      result_maybe = promise->Then(
          context,
          WrappedPromiseReaction::Create(script_state_, this, on_fulfilled));
    }

    v8::Local<v8::Promise> result;
    if (!result_maybe.ToLocal(&result)) {
      DVLOG(3) << "assuming that failure of promise->Then() is caused by "
                  "shutdown and ignoring it";
      // Try to create a dummy promise so that the calling code can continue. If
      // we can't create one, then we can't return to the calling context so we
      // have to crash. This shouldn't happen except on OOM.
      result =
          v8::Promise::Resolver::New(context).ToLocalChecked()->GetPromise();
    }
    return result;
  }

  Member<ScriptState> script_state_;
  PipeOptions pipe_options_;
  Member<ReadableStreamDefaultReader> reader_;
  Member<WritableStreamDefaultWriter> writer_;
  Member<StreamPromiseResolver> promise_;
  TraceWrapperV8Reference<v8::Promise> last_write_;
  Action shutdown_action_;
  TraceWrapperV8Reference<v8::Value> shutdown_error_;
  bool is_shutting_down_ = false;
  bool is_reading_ = false;

  DISALLOW_COPY_AND_ASSIGN(PipeToEngine);
};

class ReadableStreamNative::TeeEngine final
    : public GarbageCollectedFinalized<TeeEngine> {
 public:
  TeeEngine() = default;

  // Create the streams and start copying data.
  void Start(ScriptState*, ReadableStreamNative*, ExceptionState&);

  // Branch1() and Branch2() are null until Start() is called.
  ReadableStreamNative* Branch1() const { return branch_[0]; }
  ReadableStreamNative* Branch2() const { return branch_[1]; }

  void Trace(Visitor* visitor) {
    visitor->Trace(stream_);
    visitor->Trace(reader_);
    visitor->Trace(reason_[0]);
    visitor->Trace(reason_[1]);
    visitor->Trace(branch_[0]);
    visitor->Trace(branch_[1]);
    visitor->Trace(controller_[0]);
    visitor->Trace(controller_[1]);
    visitor->Trace(cancel_promise_);
  }

 private:
  class PullAlgorithm;
  class CancelAlgorithm;

  Member<ReadableStreamNative> stream_;
  Member<ReadableStreamDefaultReader> reader_;
  Member<StreamPromiseResolver> cancel_promise_;
  bool closed_ = false;

  // The standard contains a number of pairs of variables with one for each
  // stream. These are implemented as arrays here. While they are 1-indexed in
  // the standard, they are 0-indexed here; ie. "canceled_[0]" here corresponds
  // to "canceled1" in the standard.
  bool canceled_[2] = {false, false};
  TraceWrapperV8Reference<v8::Value> reason_[2];
  Member<ReadableStreamNative> branch_[2];
  Member<ReadableStreamDefaultController> controller_[2];

  DISALLOW_COPY_AND_ASSIGN(TeeEngine);
};

class ReadableStreamNative::TeeEngine::PullAlgorithm final
    : public StreamAlgorithm {
 public:
  explicit PullAlgorithm(TeeEngine* engine) : engine_(engine) {}

  v8::Local<v8::Promise> Run(ScriptState* script_state,
                             int,
                             v8::Local<v8::Value>[]) override {
    // https://streams.spec.whatwg.org/#readable-stream-tee
    // 12. Let pullAlgorithm be the following steps:
    //   a. Return the result of transforming ! ReadableStreamDefaultReaderRead(
    //      reader) with a fulfillment handler which takes the argument result
    //      and performs the following steps:
    return StreamThenPromise(
        script_state->GetContext(),
        ReadableStreamDefaultReader::Read(script_state, engine_->reader_)
            ->V8Promise(script_state->GetIsolate()),
        MakeGarbageCollected<ResolveFunction>(script_state, engine_));
  }

  void Trace(Visitor* visitor) override {
    visitor->Trace(engine_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  class ResolveFunction final : public StreamScriptFunction {
   public:
    ResolveFunction(ScriptState* script_state, TeeEngine* engine)
        : StreamScriptFunction(script_state), engine_(engine) {}

    void CallWithLocal(v8::Local<v8::Value> result) override {
      //    i. If closed is true, return.
      if (engine_->closed_) {
        return;
      }

      //   ii. Assert: Type(result) is Object.
      DCHECK(result->IsObject());

      auto* script_state = GetScriptState();
      auto* isolate = script_state->GetIsolate();

      //  iii. Let done be ! Get(result, "done").
      //   vi. Let value be ! Get(result, "value").
      // The precise order of operations is not important here, because |result|
      // is guaranteed to have own properties of "value" and "done" and so the
      // "Get" operations cannot have side-effects.
      v8::Local<v8::Value> value;
      bool done = false;
      bool unpack_succeeded =
          V8UnpackIteratorResult(script_state, result.As<v8::Object>(), &done)
              .ToLocal(&value);
      CHECK(unpack_succeeded);

      //   vi. Assert: Type(done) is Boolean.
      //    v. If done is true,
      if (done) {
        //    1. If canceled1 is false,
        //        a. Perform ! ReadableStreamDefaultControllerClose(branch1.
        //           [[readableStreamController]]).
        //    2. If canceled2 is false,
        //        b. Perform ! ReadableStreamDefaultControllerClose(branch2.
        //           [[readableStreamController]]).
        for (int branch = 0; branch < 2; ++branch) {
          if (!engine_->canceled_[branch]) {
            ReadableStreamDefaultController::Close(
                script_state, engine_->controller_[branch]);
          }
        }
        //    3. Set closed to true.
        engine_->closed_ = true;

        //    4. Return.
        return;
      }
      ExceptionState exception_state(isolate, ExceptionState::kUnknownContext,
                                     "", "");
      //  vii. Let value1 and value2 be value.
      // viii. If canceled2 is false and cloneForBranch2 is true, set value2 to
      //       ? StructuredDeserialize(? StructuredSerialize(value2), the
      //       current Realm Record).
      // TODO(ricea): Support cloneForBranch2

      //   ix. If canceled1 is false, perform ?
      //       ReadableStreamDefaultControllerEnqueue(branch1.
      //       [[readableStreamController]], value1).
      //    x. If canceled2 is false, perform ?
      //       ReadableStreamDefaultControllerEnqueue(branch2.
      //       [[readableStreamController]], value2).
      for (int branch = 0; branch < 2; ++branch) {
        if (!engine_->canceled_[branch]) {
          ReadableStreamDefaultController::Enqueue(script_state,
                                                   engine_->controller_[branch],
                                                   value, exception_state);
          if (exception_state.HadException()) {
            // Instead of returning a rejection, which is inconvenient here,
            // call ControllerError(). The only difference this makes is that
            // it happens synchronously, but that should not be observable.
            ReadableStreamDefaultController::Error(
                script_state, engine_->controller_[branch],
                exception_state.GetException());
            exception_state.ClearException();
            return;
          }
        }
      }
    }

    void Trace(Visitor* visitor) override {
      visitor->Trace(engine_);
      StreamScriptFunction::Trace(visitor);
    }

   private:
    Member<TeeEngine> engine_;
  };

  Member<TeeEngine> engine_;
};

class ReadableStreamNative::TeeEngine::CancelAlgorithm final
    : public StreamAlgorithm {
 public:
  CancelAlgorithm(TeeEngine* engine, int branch)
      : engine_(engine), branch_(branch) {
    DCHECK(branch == 0 || branch == 1);
  }

  v8::Local<v8::Promise> Run(ScriptState* script_state,
                             int argc,
                             v8::Local<v8::Value> argv[]) override {
    // https://streams.spec.whatwg.org/#readable-stream-tee
    // This implements both cancel1Algorithm and cancel2Algorithm as they are
    // identical except for the index they operate on. Standard comments are
    // from cancel1Algorithm.
    // 13. Let cancel1Algorithm be the following steps, taking a reason
    //     argument:
    auto* isolate = script_state->GetIsolate();

    // a. Set canceled1 to true.
    engine_->canceled_[branch_] = true;
    DCHECK_EQ(argc, 1);

    // b. Set reason1 to reason.
    engine_->reason_[branch_].Set(isolate, argv[0]);

    const int other_branch = 1 - branch_;

    // c. If canceled2 is true,
    if (engine_->canceled_[other_branch]) {
      // i. Let compositeReason be ! CreateArrayFromList(« reason1, reason2 »).
      v8::Local<v8::Value> reason[] = {engine_->reason_[0].NewLocal(isolate),
                                       engine_->reason_[1].NewLocal(isolate)};
      v8::Local<v8::Value> composite_reason =
          v8::Array::New(script_state->GetIsolate(), reason, 2);

      // ii. Let cancelResult be ! ReadableStreamCancel(stream,
      //    compositeReason).
      auto cancel_result = ReadableStreamNative::Cancel(
          script_state, engine_->stream_, composite_reason);

      // iii. Resolve cancelPromise with cancelResult.
      engine_->cancel_promise_->Resolve(script_state, cancel_result);
    }
    return engine_->cancel_promise_->V8Promise(isolate);
  }

  void Trace(Visitor* visitor) override {
    visitor->Trace(engine_);
    StreamAlgorithm::Trace(visitor);
  }

 private:
  Member<TeeEngine> engine_;
  const int branch_;
};

void ReadableStreamNative::TeeEngine::Start(ScriptState* script_state,
                                            ReadableStreamNative* stream,
                                            ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#readable-stream-tee
  //  1. Assert: ! IsReadableStream(stream) is true.
  DCHECK(stream);

  // TODO(ricea):  2. Assert: Type(cloneForBranch2) is Boolean.

  stream_ = stream;

  // 3. Let reader be ? AcquireReadableStreamDefaultReader(stream).
  reader_ = ReadableStreamNative::AcquireDefaultReader(script_state, stream,
                                                       false, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // These steps are performed by the constructor:
  //  4. Let closed be false.
  DCHECK(!closed_);

  //  5. Let canceled1 be false.
  DCHECK(!canceled_[0]);

  //  6. Let canceled2 be false.
  DCHECK(!canceled_[1]);

  //  7. Let reason1 be undefined.
  DCHECK(reason_[0].IsEmpty());

  //  8. Let reason2 be undefined.
  DCHECK(reason_[1].IsEmpty());

  //  9. Let branch1 be undefined.
  DCHECK(!branch_[0]);

  // 10. Let branch2 be undefined.
  DCHECK(!branch_[1]);

  // 11. Let cancelPromise be a new promise.
  cancel_promise_ = MakeGarbageCollected<StreamPromiseResolver>(script_state);

  // 12. Let pullAlgorithm be the following steps:
  // (steps are defined in PullAlgorithm::Run()).
  auto* pull_algorithm = MakeGarbageCollected<PullAlgorithm>(this);

  // 13. Let cancel1Algorithm be the following steps, taking a reason argument:
  // (see CancelAlgorithm::Run()).
  auto* cancel1_algorithm = MakeGarbageCollected<CancelAlgorithm>(this, 0);

  // 14. Let cancel2Algorithm be the following steps, taking a reason argument:
  // (both algorithms share a single implementation).
  auto* cancel2_algorithm = MakeGarbageCollected<CancelAlgorithm>(this, 1);

  // 15. Let startAlgorithm be an algorithm that returns undefined.
  auto* start_algorithm = CreateTrivialStartAlgorithm();

  auto* size_algorithm = CreateDefaultSizeAlgorithm();

  // 16. Set branch1 to ! CreateReadableStream(startAlgorithm, pullAlgorithm,
  //   cancel1Algorithm).
  branch_[0] = ReadableStreamNative::Create(
      script_state, start_algorithm, pull_algorithm, cancel1_algorithm, 1.0,
      size_algorithm, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 17. Set branch2 to ! CreateReadableStream(startAlgorithm, pullAlgorithm,
  //   cancel2Algorithm).
  branch_[1] = ReadableStreamNative::Create(
      script_state, start_algorithm, pull_algorithm, cancel2_algorithm, 1.0,
      size_algorithm, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  for (int branch = 0; branch < 2; ++branch) {
    controller_[branch] = branch_[branch]->readable_stream_controller_;
  }

  class RejectFunction final : public StreamScriptFunction {
   public:
    RejectFunction(ScriptState* script_state, TeeEngine* engine)
        : StreamScriptFunction(script_state), engine_(engine) {}

    void CallWithLocal(v8::Local<v8::Value> r) override {
      // 18. Upon rejection of reader.[[closedPromise]] with reason r,
      //   a. Perform ! ReadableStreamDefaultControllerError(branch1.
      //      [[readableStreamController]], r).
      ReadableStreamDefaultController::Error(GetScriptState(),
                                             engine_->controller_[0], r);

      //   b. Perform ! ReadableStreamDefaultControllerError(branch2.
      //      [[readableStreamController]], r).
      ReadableStreamDefaultController::Error(GetScriptState(),
                                             engine_->controller_[1], r);
    }

    void Trace(Visitor* visitor) override {
      visitor->Trace(engine_);
      StreamScriptFunction::Trace(visitor);
    }

   private:
    Member<TeeEngine> engine_;
  };

  // 18. Upon rejection of reader.[[closedPromise]] with reason r,
  StreamThenPromise(
      script_state->GetContext(),
      reader_->closed_promise_->V8Promise(script_state->GetIsolate()), nullptr,
      MakeGarbageCollected<RejectFunction>(script_state, this));

  // Step "19. Return « branch1, branch2 »."
  // is performed by the caller.
}

class ReadableStreamNative::ReadHandleImpl final
    : public ReadableStream::ReadHandle {
 public:
  explicit ReadHandleImpl(ReadableStreamDefaultReader* reader)
      : reader_(reader) {}
  ~ReadHandleImpl() override = default;

  ScriptPromise Read(ScriptState* script_state) override {
    return ReadableStreamDefaultReader::Read(script_state, reader_)
        ->GetScriptPromise(script_state);
  }

  void Trace(Visitor* visitor) override {
    visitor->Trace(reader_);
    ReadHandle::Trace(visitor);
  }

 private:
  const Member<ReadableStreamDefaultReader> reader_;
};

ReadableStreamNative* ReadableStreamNative::Create(
    ScriptState* script_state,
    ScriptValue underlying_source,
    ScriptValue strategy,
    ExceptionState& exception_state) {
  auto* stream = MakeGarbageCollected<ReadableStreamNative>(
      script_state, underlying_source, strategy, false, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }

  return stream;
}

ReadableStreamNative* ReadableStreamNative::CreateWithCountQueueingStrategy(
    ScriptState* script_state,
    UnderlyingSourceBase* underlying_source,
    size_t high_water_mark) {
  auto* isolate = script_state->GetIsolate();

  // It's safer to use a workalike rather than a real CountQueuingStrategy
  // object. We use the default "size" function as it is implemented in C++ and
  // so much faster than calling into JavaScript. Since the create object has a
  // null prototype, there is no danger of us finding some other "size" function
  // via the prototype chain.
  v8::Local<v8::Name> high_water_mark_string =
      V8AtomicString(isolate, "highWaterMark");
  v8::Local<v8::Value> high_water_mark_value =
      v8::Number::New(isolate, high_water_mark);

  auto strategy_object =
      v8::Object::New(isolate, v8::Null(isolate), &high_water_mark_string,
                      &high_water_mark_value, 1);

  ExceptionState exception_state(script_state->GetIsolate(),
                                 ExceptionState::kConstructionContext,
                                 "ReadableStream");

  v8::Local<v8::Value> underlying_source_v8 =
      ToV8(underlying_source, script_state);

  auto* stream = MakeGarbageCollected<ReadableStreamNative>(
      script_state, ScriptValue(script_state, underlying_source_v8),
      ScriptValue(script_state, strategy_object), true, exception_state);

  if (exception_state.HadException()) {
    exception_state.ClearException();
    DLOG(WARNING)
        << "Ignoring an exception in CreateWithCountQueuingStrategy().";
  }

  return stream;
}

ReadableStreamNative* ReadableStreamNative::Create(
    ScriptState* script_state,
    StreamStartAlgorithm* start_algorithm,
    StreamAlgorithm* pull_algorithm,
    StreamAlgorithm* cancel_algorithm,
    double high_water_mark,
    StrategySizeAlgorithm* size_algorithm,
    ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#create-readable-stream
  // All arguments are compulsory in this implementation, so the first two steps
  // are skipped:
  // 1. If highWaterMark was not passed, set it to 1.
  // 2. If sizeAlgorithm was not passed, set it to an algorithm that returns 1.

  // 3. Assert: ! IsNonNegativeNumber(highWaterMark) is true.
  DCHECK_GE(high_water_mark, 0);

  // 4. Let stream be ObjectCreate(the original value of ReadableStream's
  //    prototype property).
  auto* stream = MakeGarbageCollected<ReadableStreamNative>();

  // 5. Perform ! InitializeReadableStream(stream).
  Initialize(stream);

  // 6. Let controller be ObjectCreate(the original value of
  //    ReadableStreamDefaultController's prototype property).
  auto* controller = MakeGarbageCollected<ReadableStreamDefaultController>();

  // 7. Perform ? SetUpReadableStreamDefaultController(stream, controller,
  //    startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark,
  //    sizeAlgorithm).
  ReadableStreamDefaultController::SetUp(
      script_state, stream, controller, start_algorithm, pull_algorithm,
      cancel_algorithm, high_water_mark, size_algorithm, false,
      exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }

  // 8. Return stream.
  return stream;
}

ReadableStreamNative::ReadableStreamNative() = default;

// TODO(ricea): Remove |enable_blink_lock_notifications| once
// blink::ReadableStreamOperations has been updated to use CreateReadableStream.
ReadableStreamNative::ReadableStreamNative(ScriptState* script_state,
                                           ScriptValue raw_underlying_source,
                                           ScriptValue raw_strategy,
                                           bool enable_blink_lock_notifications,
                                           ExceptionState& exception_state) {
  if (!enable_blink_lock_notifications) {
    // TODO(ricea): Move this to IDL once blink::ReadableStreamOperations is
    // no longer using the public constructor.
    UseCounter::Count(ExecutionContext::From(script_state),
                      WebFeature::kReadableStreamConstructor);
  }

  // https://streams.spec.whatwg.org/#rs-constructor
  //  1. Perform ! InitializeReadableStream(this).
  Initialize(this);

  // The next part of this constructor corresponds to the object conversions
  // that are implicit in the definition in the standard.
  DCHECK(!raw_underlying_source.IsEmpty());
  DCHECK(!raw_strategy.IsEmpty());

  auto context = script_state->GetContext();
  auto* isolate = script_state->GetIsolate();

  v8::Local<v8::Object> underlying_source;
  ScriptValueToObject(script_state, raw_underlying_source, &underlying_source,
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

  // 4. Let type be ? GetV(underlyingSource, "type").
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> type;
  if (!underlying_source->Get(context, V8AtomicString(isolate, "type"))
           .ToLocal(&type)) {
    exception_state.RethrowV8Exception(try_catch.Exception());
    return;
  }

  if (!type->IsUndefined()) {
    // 5. Let typeString be ? ToString(type).
    v8::Local<v8::String> type_string;
    if (!type->ToString(context).ToLocal(&type_string)) {
      exception_state.RethrowV8Exception(try_catch.Exception());
      return;
    }

    // 6. If typeString is "bytes",
    if (type_string == V8AtomicString(isolate, "bytes")) {
      // TODO(ricea): Implement bytes type.
      exception_state.ThrowRangeError("bytes type is not yet implemented");
      return;
    }

    // 8. Otherwise, throw a RangeError exception.
    exception_state.ThrowRangeError("Invalid type is specified");
    return;
  }

  // 7. Otherwise, if type is undefined,
  //   a. Let sizeAlgorithm be ? MakeSizeAlgorithmFromSizeFunction(size).
  auto* size_algorithm =
      strategy_unpacker.MakeSizeAlgorithm(script_state, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  DCHECK(size_algorithm);

  //   b. If highWaterMark is undefined, let highWaterMark be 1.
  //   c. Set highWaterMark to ? ValidateAndNormalizeHighWaterMark(
  //      highWaterMark).
  double high_water_mark =
      strategy_unpacker.GetHighWaterMark(script_state, 1, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // 4. Perform ? SetUpReadableStreamDefaultControllerFromUnderlyingSource
  //  (this, underlyingSource, highWaterMark, sizeAlgorithm).
  ReadableStreamDefaultController::SetUpFromUnderlyingSource(
      script_state, this, underlying_source, high_water_mark, size_algorithm,
      enable_blink_lock_notifications, exception_state);
}

ReadableStreamNative::~ReadableStreamNative() = default;

bool ReadableStreamNative::locked(ScriptState* script_state,
                                  ExceptionState& exception_state) const {
  // https://streams.spec.whatwg.org/#rs-locked
  // 2. Return ! IsReadableStreamLocked(this).
  return IsLocked(this);
}

ScriptPromise ReadableStreamNative::cancel(ScriptState* script_state,
                                           ExceptionState& exception_state) {
  return cancel(
      script_state,
      ScriptValue(script_state, v8::Undefined(script_state->GetIsolate())),
      exception_state);
}

ScriptPromise ReadableStreamNative::cancel(ScriptState* script_state,
                                           ScriptValue reason,
                                           ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rs-cancel
  // 2. If ! IsReadableStreamLocked(this) is true, return a promise rejected
  //    with a TypeError exception.
  if (IsLocked(this)) {
    exception_state.ThrowTypeError("Cannot cancel a locked stream");
    return ScriptPromise();
  }

  // 3. Return ! ReadableStreamCancel(this, reason).
  v8::Local<v8::Promise> result = Cancel(script_state, this, reason.V8Value());
  return ScriptPromise(script_state, result);
}

ScriptValue ReadableStreamNative::getReader(ScriptState* script_state,
                                            ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rs-get-reader
  // 2. If mode is undefined, return ? AcquireReadableStreamDefaultReader(this,
  //    true).
  auto* reader = ReadableStreamNative::AcquireDefaultReader(
      script_state, this, true, exception_state);
  if (!reader) {
    return ScriptValue();
  }

  return ScriptValue(script_state, ToV8(reader, script_state));
}

ScriptValue ReadableStreamNative::getReader(ScriptState* script_state,
                                            ScriptValue options,
                                            ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#rs-get-reader
  // Since we don't support byob readers, the only thing
  // GetReaderValidateOptions() needs to do is throw an exception if
  // |options.mode| is invalid.
  GetReaderValidateOptions(script_state, options, exception_state);
  if (exception_state.HadException()) {
    return ScriptValue();
  }

  return getReader(script_state, exception_state);
}

ScriptValue ReadableStreamNative::pipeThrough(ScriptState* script_state,
                                              ScriptValue transform_stream,
                                              ExceptionState& exception_state) {
  return pipeThrough(
      script_state, transform_stream,
      ScriptValue(script_state, v8::Undefined(script_state->GetIsolate())),
      exception_state);
}

// https://streams.spec.whatwg.org/#rs-pipe-through
ScriptValue ReadableStreamNative::pipeThrough(ScriptState* script_state,
                                              ScriptValue transform_stream,
                                              ScriptValue options,
                                              ExceptionState& exception_state) {
  // TODO(ricea): Get the order of operations to strictly match the standard.
  ScriptValue readable;
  WritableStream* writable;
  PipeThroughExtractReadableWritable(script_state, this, transform_stream,
                                     &readable, &writable, exception_state);
  if (exception_state.HadException()) {
    return ScriptValue();
  }

  PipeOptions pipe_options;
  UnpackPipeOptions(script_state, options, &pipe_options, exception_state);

  DCHECK(RuntimeEnabledFeatures::StreamsNativeEnabled());

  // This cast is safe because the following code will only be run when the
  // native version of WritableStream is in use.
  WritableStreamNative* writable_native =
      static_cast<WritableStreamNative*>(writable);

  // 8. Let _promise_ be ! ReadableStreamPipeTo(*this*, _writable_,
  //    _preventClose_, _preventAbort_, _preventCancel_,
  //   _signal_).

  ScriptPromise promise =
      PipeTo(script_state, this, writable_native, pipe_options);

  // 9. Set _promise_.[[PromiseIsHandled]] to *true*.
  promise.MarkAsHandled();

  // 10. Return _readable_.
  return readable;
}

ScriptPromise ReadableStreamNative::pipeTo(ScriptState* script_state,
                                           ScriptValue destination,
                                           ExceptionState& exception_state) {
  return pipeTo(
      script_state, destination,
      ScriptValue(script_state, v8::Undefined(script_state->GetIsolate())),
      exception_state);
}

ScriptPromise ReadableStreamNative::pipeTo(ScriptState* script_state,
                                           ScriptValue destination_value,
                                           ScriptValue options,
                                           ExceptionState& exception_state) {
  WritableStream* destination = PipeToCheckSourceAndDestination(
      script_state, this, destination_value, exception_state);
  if (exception_state.HadException()) {
    return ScriptPromise();
  }
  CHECK(destination);

  PipeOptions pipe_options;
  UnpackPipeOptions(script_state, options, &pipe_options, exception_state);

  DCHECK(RuntimeEnabledFeatures::StreamsNativeEnabled());

  // This cast is safe because the following code will only be run when the
  // native version of WritableStream is in use.
  WritableStreamNative* destination_native =
      static_cast<WritableStreamNative*>(destination);

  return PipeTo(script_state, this, destination_native, pipe_options);
}

ScriptValue ReadableStreamNative::tee(ScriptState* script_state,
                                      ExceptionState& exception_state) {
  return CallTeeAndReturnBranchArray(script_state, this, exception_state);
}

//
// Readable stream abstract operations
//
ReadableStreamDefaultReader* ReadableStreamNative::AcquireDefaultReader(
    ScriptState* script_state,
    ReadableStreamNative* stream,
    bool for_author_code,
    ExceptionState& exception_state) {
  // https://streams.spec.whatwg.org/#acquire-readable-stream-reader
  // for_author_code is compulsory in this implementation
  // 1. If forAuthorCode was not passed, set it to false.

  // 2. Let reader be ? Construct(ReadableStreamDefaultReader, « stream »).
  auto* reader = MakeGarbageCollected<ReadableStreamDefaultReader>(
      script_state, stream, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }

  // 3. Set reader.[[forAuthorCode]] to forAuthorCode.
  reader->for_author_code_ = for_author_code;

  // 4. Return reader.
  return reader;
}

void ReadableStreamNative::Initialize(ReadableStreamNative* stream) {
  // Fields are initialised by the constructor, so we only check that they were
  // initialised correctly.
  // https://streams.spec.whatwg.org/#initialize-readable-stream
  // 1. Set stream.[[state]] to "readable".
  DCHECK_EQ(stream->state_, kReadable);
  // 2. Set stream.[[reader]] and stream.[[storedError]] to undefined.
  DCHECK(!stream->reader_);
  DCHECK(stream->stored_error_.IsEmpty());
  // 3. Set stream.[[disturbed]] to false.
  DCHECK(!stream->is_disturbed_);
}

// TODO(domenic): cloneForBranch2 argument from spec not supported yet
void ReadableStreamNative::Tee(ScriptState* script_state,
                               ReadableStream** branch1,
                               ReadableStream** branch2,
                               ExceptionState& exception_state) {
  auto* engine = MakeGarbageCollected<TeeEngine>();
  engine->Start(script_state, this, exception_state);
  if (exception_state.HadException()) {
    return;
  }

  // Instead of returning a List like ReadableStreamTee in the standard, the
  // branches are returned via output parameters.
  *branch1 = engine->Branch1();
  *branch2 = engine->Branch2();
}

ReadableStream::ReadHandle* ReadableStreamNative::GetReadHandle(
    ScriptState* script_state,
    ExceptionState& exception_state) {
  auto* reader = ReadableStreamNative::AcquireDefaultReader(
      script_state, this, false, exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return MakeGarbageCollected<ReadHandleImpl>(reader);
}

void ReadableStreamNative::LockAndDisturb(ScriptState* script_state,
                                          ExceptionState& exception_state) {
  ScriptState::Scope scope(script_state);

  if (reader_) {
    return;
  }

  ReadableStreamDefaultReader* reader =
      AcquireDefaultReader(script_state, this, false, exception_state);
  if (!reader) {
    return;
  }

  is_disturbed_ = true;
}

void ReadableStreamNative::Serialize(ScriptState* script_state,
                                     MessagePort* port,
                                     ExceptionState& exception_state) {
  // TODO(ricea): Implement this.
}

v8::Local<v8::Value> ReadableStreamNative::GetStoredError(
    v8::Isolate* isolate) const {
  return stored_error_.NewLocal(isolate);
}

void ReadableStreamNative::Trace(Visitor* visitor) {
  visitor->Trace(readable_stream_controller_);
  visitor->Trace(reader_);
  visitor->Trace(stored_error_);
  ReadableStream::Trace(visitor);
}

ScriptPromise ReadableStreamNative::PipeTo(ScriptState* script_state,
                                           ReadableStreamNative* readable,
                                           WritableStreamNative* destination,
                                           PipeOptions pipe_options) {
  auto* engine = MakeGarbageCollected<PipeToEngine>(script_state, pipe_options);
  return engine->Start(readable, destination);
}

//
// Abstract Operations Used By Controllers
//

StreamPromiseResolver* ReadableStreamNative::AddReadRequest(
    ScriptState* script_state,
    ReadableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#readable-stream-add-read-request
  // 1. Assert: ! IsReadableStreamDefaultReader(stream.[[reader]]) is true.
  DCHECK(stream->reader_);

  // 2. Assert: stream.[[state]] is "readable".
  DCHECK_EQ(stream->state_, kReadable);

  // 3. Let promise be a new promise.
  auto* promise = MakeGarbageCollected<StreamPromiseResolver>(script_state);

  // This implementation stores promises directly in |read_requests_| rather
  // than wrapping them in a Record.
  // 4. Let readRequest be Record {[[promise]]: promise}.
  // 5. Append readRequest as the last element of stream.[[reader]].
  //  [[readRequests]].
  stream->reader_->read_requests_.push_back(promise);

  // 6. Return promise.
  return promise;
}

v8::Local<v8::Promise> ReadableStreamNative::Cancel(
    ScriptState* script_state,
    ReadableStreamNative* stream,
    v8::Local<v8::Value> reason) {
  // https://streams.spec.whatwg.org/#readable-stream-cancel
  // 1. Set stream.[[disturbed]] to true.
  stream->is_disturbed_ = true;

  // 2. If stream.[[state]] is "closed", return a promise resolved with
  //    undefined.
  const auto state = stream->state_;
  if (state == kClosed) {
    return PromiseResolveWithUndefined(script_state);
  }

  // 3. If stream.[[state]] is "errored", return a promise rejected with stream.
  //    [[storedError]].
  if (state == kErrored) {
    return PromiseReject(script_state,
                         stream->GetStoredError(script_state->GetIsolate()));
  }

  // 4. Perform ! ReadableStreamClose(stream).
  Close(script_state, stream);

  // 5. Let sourceCancelPromise be ! stream.[[readableStreamController]].
  //    [[CancelSteps]](reason).
  v8::Local<v8::Promise> source_cancel_promise =
      stream->readable_stream_controller_->CancelSteps(script_state, reason);

  class ReturnUndefinedFunction : public StreamScriptFunction {
   public:
    explicit ReturnUndefinedFunction(ScriptState* script_state)
        : StreamScriptFunction(script_state) {}

    // The method does nothing; the default value of undefined is returned to
    // JavaScript.
    void CallWithLocal(v8::Local<v8::Value>) override {}
  };

  // 6. Return the result of transforming sourceCancelPromise with a
  //    fulfillment handler that returns undefined.
  return StreamThenPromise(
      script_state->GetContext(), source_cancel_promise,
      MakeGarbageCollected<ReturnUndefinedFunction>(script_state));
}

void ReadableStreamNative::Close(ScriptState* script_state,
                                 ReadableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#readable-stream-close
  // 1. Assert: stream.[[state]] is "readable".
  DCHECK_EQ(stream->state_, kReadable);

  // 2. Set stream.[[state]] to "closed".
  stream->state_ = kClosed;

  // 3. Let reader be stream.[[reader]].
  ReadableStreamDefaultReader* reader = stream->reader_;

  // 4. If reader is undefined, return.
  if (!reader) {
    return;
  }

  // TODO(ricea): Support BYOB readers.
  // 5. If ! IsReadableStreamDefaultReader(reader) is true,
  //   a. Repeat for each readRequest that is an element of reader.
  //      [[readRequests]],
  for (StreamPromiseResolver* promise : reader->read_requests_) {
    //   i. Resolve readRequest.[[promise]] with !
    //      ReadableStreamCreateReadResult(undefined, true, reader.
    //      [[forAuthorCode]]).
    promise->Resolve(script_state,
                     CreateReadResult(script_state,
                                      v8::Undefined(script_state->GetIsolate()),
                                      true, reader->for_author_code_));
  }

  //   b. Set reader.[[readRequests]] to an empty List.
  reader->read_requests_.clear();

  // 6. Resolve reader.[[closedPromise]] with undefined.
  reader->closed_promise_->ResolveWithUndefined(script_state);
}

v8::Local<v8::Value> ReadableStreamNative::CreateReadResult(
    ScriptState* script_state,
    v8::Local<v8::Value> value,
    bool done,
    bool for_author_code) {
  // https://streams.spec.whatwg.org/#readable-stream-create-read-result
  auto* isolate = script_state->GetIsolate();
  auto context = script_state->GetContext();
  auto value_string = V8AtomicString(isolate, "value");
  auto done_string = V8AtomicString(isolate, "done");
  auto done_value = v8::Boolean::New(isolate, done);
  // 1. Let prototype be null.
  // 2. If forAuthorCode is true, set prototype to %ObjectPrototype%.
  // This implementation doesn't use a |prototype| variable, instead using
  // different code paths depending on the value of |for_author_code|.
  if (for_author_code) {
    // 4. Let obj be ObjectCreate(prototype).
    auto obj = v8::Object::New(isolate);

    // 5. Perform CreateDataProperty(obj, "value", value).
    obj->CreateDataProperty(context, value_string, value).Check();

    // 6. Perform CreateDataProperty(obj, "done", done).
    obj->CreateDataProperty(context, done_string, done_value).Check();

    // 7. Return obj.
    return obj;
  }

  // When |for_author_code| is false, we can perform all the steps in a single
  // call to V8.

  // 4. Let obj be ObjectCreate(prototype).
  // 5. Perform CreateDataProperty(obj, "value", value).
  // 6. Perform CreateDataProperty(obj, "done", done).
  // 7. Return obj.
  // TODO(ricea): Is it possible to use this optimised API in both cases?
  v8::Local<v8::Name> names[2] = {value_string, done_string};
  v8::Local<v8::Value> values[2] = {value, done_value};

  static_assert(base::size(names) == base::size(values),
                "names and values arrays must be the same size");
  return v8::Object::New(isolate, v8::Null(isolate), names, values,
                         base::size(names));
}

void ReadableStreamNative::Error(ScriptState* script_state,
                                 ReadableStreamNative* stream,
                                 v8::Local<v8::Value> e) {
  // https://streams.spec.whatwg.org/#readable-stream-error
  // 2. Assert: stream.[[state]] is "readable".
  DCHECK_EQ(stream->state_, kReadable);
  auto* isolate = script_state->GetIsolate();

  // 3. Set stream.[[state]] to "errored".
  stream->state_ = kErrored;

  // 4. Set stream.[[storedError]] to e.
  stream->stored_error_.Set(isolate, e);

  // 5. Let reader be stream.[[reader]].
  ReadableStreamDefaultReader* reader = stream->reader_;

  // 6. If reader is undefined, return.
  if (!reader) {
    return;
  }

  // 7. If ! IsReadableStreamDefaultReader(reader) is true,
  // TODO(ricea): Support BYOB readers.
  //   a. Repeat for each readRequest that is an element of reader.
  //      [[readRequests]],
  for (StreamPromiseResolver* promise : reader->read_requests_) {
    //   i. Reject readRequest.[[promise]] with e.
    promise->Reject(script_state, e);
  }

  //   b. Set reader.[[readRequests]] to a new empty List.
  reader->read_requests_.clear();

  // 9. Reject reader.[[closedPromise]] with e.
  reader->closed_promise_->Reject(script_state, e);

  // 10. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
  reader->closed_promise_->MarkAsHandled(isolate);
}

void ReadableStreamNative::FulfillReadRequest(ScriptState* script_state,
                                              ReadableStreamNative* stream,
                                              v8::Local<v8::Value> chunk,
                                              bool done) {
  // https://streams.spec.whatwg.org/#readable-stream-fulfill-read-request
  // 1. Let reader be stream.[[reader]].
  ReadableStreamDefaultReader* reader = stream->reader_;

  // 2. Let readRequest be the first element of reader.[[readRequests]].
  StreamPromiseResolver* read_request = reader->read_requests_.front();

  // 3. Remove readIntoRequest from reader.[[readIntoRequests]], shifting all
  //    other elements downward (so that the second becomes the first, and so
  //    on).
  reader->read_requests_.pop_front();

  // 4. Resolve readIntoRequest.[[promise]] with !
  //    ReadableStreamCreateReadResult(chunk, done, reader.[[forAuthorCode]]).
  read_request->Resolve(
      script_state, ReadableStreamNative::CreateReadResult(
                        script_state, chunk, done, reader->for_author_code_));
}

int ReadableStreamNative::GetNumReadRequests(
    const ReadableStreamNative* stream) {
  // https://streams.spec.whatwg.org/#readable-stream-get-num-read-requests
  // 1. Return the number of elements in stream.[[reader]].[[readRequests]].
  return stream->reader_->read_requests_.size();
}

//
//  Readable Stream Reader Generic Abstract Operations
//

v8::Local<v8::Promise> ReadableStreamNative::ReaderGenericCancel(
    ScriptState* script_state,
    ReadableStreamDefaultReader* reader,
    v8::Local<v8::Value> reason) {
  // https://streams.spec.whatwg.org/#readable-stream-reader-generic-cancel
  // 1. Let stream be reader.[[ownerReadableStream]].
  ReadableStreamNative* stream = reader->owner_readable_stream_;

  // 2. Assert: stream is not undefined.
  DCHECK(stream);

  // 3. Return ! ReadableStreamCancel(stream, reason).
  return ReadableStreamNative::Cancel(script_state, stream, reason);
}

void ReadableStreamNative::ReaderGenericInitialize(
    ScriptState* script_state,
    ReadableStreamDefaultReader* reader,
    ReadableStreamNative* stream) {
  auto* isolate = script_state->GetIsolate();
  // TODO(yhirano): Remove this when we don't need hasPendingActivity in
  // blink::UnderlyingSourceBase.
  ReadableStreamDefaultController* controller =
      stream->readable_stream_controller_;
  if (controller->enable_blink_lock_notifications_) {
    // The stream is created with an external controller (i.e. made in
    // Blink).
    v8::Local<v8::Object> lock_notify_target =
        controller->lock_notify_target_.NewLocal(isolate);
    CallNullaryMethod(script_state, lock_notify_target, "notifyLockAcquired");
  }

  // https://streams.spec.whatwg.org/#readable-stream-reader-generic-initialize
  // 1. Set reader.[[forAuthorCode]] to true.
  DCHECK(reader->for_author_code_);

  // 2. Set reader.[[ownerReadableStream]] to stream.
  reader->owner_readable_stream_ = stream;

  // 3. Set stream.[[reader]] to reader.
  stream->reader_ = reader;

  switch (stream->state_) {
    // 4. If stream.[[state]] is "readable",
    case kReadable:
      // a. Set reader.[[closedPromise]] to a new promise.
      reader->closed_promise_ =
          MakeGarbageCollected<StreamPromiseResolver>(script_state);
      break;

    // 5. Otherwise, if stream.[[state]] is "closed",
    case kClosed:
      // a. Set reader.[[closedPromise]] to a promise resolved with undefined.
      reader->closed_promise_ =
          StreamPromiseResolver::CreateResolvedWithUndefined(script_state);
      break;

    // 6. Otherwise,
    case kErrored:
      // a. Assert: stream.[[state]] is "errored".
      DCHECK_EQ(stream->state_, kErrored);

      // b. Set reader.[[closedPromise]] to a promise rejected with stream.
      //    [[storedError]].
      reader->closed_promise_ = StreamPromiseResolver::CreateRejected(
          script_state, stream->GetStoredError(isolate));

      // c. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
      reader->closed_promise_->MarkAsHandled(isolate);
      break;
  }
}

void ReadableStreamNative::ReaderGenericRelease(
    ScriptState* script_state,
    ReadableStreamDefaultReader* reader) {
  // https://streams.spec.whatwg.org/#readable-stream-reader-generic-release
  // 1. Assert: reader.[[ownerReadableStream]] is not undefined.
  DCHECK(reader->owner_readable_stream_);

  // 2. Assert: reader.[[ownerReadableStream]].[[reader]] is reader.
  DCHECK_EQ(reader->owner_readable_stream_->reader_, reader);

  auto* isolate = script_state->GetIsolate();
  // TODO(yhirano): Remove this when we don"t need hasPendingActivity in
  // blink::UnderlyingSourceBase.
  ReadableStreamDefaultController* controller =
      reader->owner_readable_stream_->readable_stream_controller_;
  if (controller->enable_blink_lock_notifications_) {
    // The stream is created with an external controller (i.e. made in
    // Blink).
    auto lock_notify_target = controller->lock_notify_target_.NewLocal(isolate);
    CallNullaryMethod(script_state, lock_notify_target, "notifyLockReleased");
  }

  // 3. If reader.[[ownerReadableStream]].[[state]] is "readable", reject
  //    reader.[[closedPromise]] with a TypeError exception.
  if (reader->owner_readable_stream_->state_ == kReadable) {
    reader->closed_promise_->Reject(
        script_state,
        v8::Exception::TypeError(V8String(
            isolate,
            "This readable stream reader has been released and cannot be used "
            "to monitor the stream's state")));
  } else {
    // 4. Otherwise, set reader.[[closedPromise]] to a promise rejected with a
    //    TypeError exception.
    reader->closed_promise_ = StreamPromiseResolver::CreateRejected(
        script_state, v8::Exception::TypeError(V8String(
                          isolate,
                          "This readable stream reader has been released and "
                          "cannot be used to monitor the stream's state")));
  }

  // 5. Set reader.[[closedPromise]].[[PromiseIsHandled]] to true.
  reader->closed_promise_->MarkAsHandled(isolate);

  // 6. Set reader.[[ownerReadableStream]].[[reader]] to undefined.
  reader->owner_readable_stream_->reader_ = nullptr;

  // 7. Set reader.[[ownerReadableStream]] to undefined.
  reader->owner_readable_stream_ = nullptr;
}

//
// TODO(ricea): Functions for transferable streams.
//

void ReadableStreamNative::UnpackPipeOptions(ScriptState* script_state,
                                             ScriptValue options,
                                             PipeOptions* pipe_options,
                                             ExceptionState& exception_state) {
  auto* isolate = script_state->GetIsolate();
  v8::TryCatch block(isolate);
  v8::Local<v8::Value> options_value = options.V8Value();
  v8::Local<v8::Object> options_object;
  if (options_value->IsUndefined()) {
    options_object = v8::Object::New(isolate);
  } else if (!options_value->ToObject(script_state->GetContext())
                  .ToLocal(&options_object)) {
    exception_state.RethrowV8Exception(block.Exception());
    return;
  }

  // 4. Set preventClose to ! ToBoolean(preventClose), set preventAbort to !
  // ToBoolean(preventAbort), and set preventCancel to !
  // ToBoolean(preventCancel).
  pipe_options->prevent_close =
      GetBoolean(script_state, options_object, "preventClose", exception_state);
  if (exception_state.HadException()) {
    return;
  }

  pipe_options->prevent_abort =
      GetBoolean(script_state, options_object, "preventAbort", exception_state);
  if (exception_state.HadException()) {
    return;
  }

  pipe_options->prevent_cancel = GetBoolean(script_state, options_object,
                                            "preventCancel", exception_state);
  if (exception_state.HadException()) {
    return;
  }
}

bool ReadableStreamNative::GetBoolean(ScriptState* script_state,
                                      v8::Local<v8::Object> dictionary,
                                      const char* property_name,
                                      ExceptionState& exception_state) {
  auto* isolate = script_state->GetIsolate();
  v8::TryCatch block(isolate);
  v8::Local<v8::Value> property_value;
  if (!dictionary
           ->Get(script_state->GetContext(),
                 V8AtomicString(isolate, property_name))
           .ToLocal(&property_value)) {
    exception_state.RethrowV8Exception(block.Exception());
    return false;
  }
  return property_value->ToBoolean(isolate)->Value();
}

void ReadableStreamNative::CallNullaryMethod(ScriptState* script_state,
                                             v8::Local<v8::Object> object,
                                             const char* method_name) {
  auto* isolate = script_state->GetIsolate();
  auto context = script_state->GetContext();
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::Value> method;
  if (!object->Get(context, V8AtomicString(isolate, method_name))
           .ToLocal(&method)) {
    DLOG(WARNING) << "Ignored failed lookup of '" << method_name
                  << "' in CallNullaryMethod";
    return;
  }

  if (!method->IsFunction()) {
    DLOG(WARNING) << "Didn't call '" << method_name
                  << "' in CallNullaryMethod because it was the wrong type";
    return;
  }

  v8::MaybeLocal<v8::Value> result =
      method.As<v8::Function>()->Call(context, object, 0, nullptr);
  if (result.IsEmpty()) {
    DLOG(WARNING) << "Ignored failure of '" << method_name
                  << "' in CallNullaryMethod";
  }
}

}  // namespace blink
