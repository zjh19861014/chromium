// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef REMOTING_SIGNALING_GRPC_SUPPORT_SCOPED_GRPC_SERVER_STREAM_H_
#define REMOTING_SIGNALING_GRPC_SUPPORT_SCOPED_GRPC_SERVER_STREAM_H_

#include "base/macros.h"
#include "base/memory/weak_ptr.h"

namespace remoting {

class GrpcAsyncServerStreamingRequestBase;

// A class that holds a gRPC server stream. The streaming channel will be closed
// once the holder object is deleted.
class ScopedGrpcServerStream {
 public:
  explicit ScopedGrpcServerStream(
      base::WeakPtr<GrpcAsyncServerStreamingRequestBase> request);
  virtual ~ScopedGrpcServerStream();

 private:
  base::WeakPtr<GrpcAsyncServerStreamingRequestBase> request_;
  DISALLOW_COPY_AND_ASSIGN(ScopedGrpcServerStream);
};

}  // namespace remoting

#endif  // REMOTING_SIGNALING_GRPC_SUPPORT_SCOPED_GRPC_SERVER_STREAM_H_
