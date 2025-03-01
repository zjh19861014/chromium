// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_BACKGROUND_FETCH_STORAGE_GET_REQUEST_BLOB_TASK_H_
#define CONTENT_BROWSER_BACKGROUND_FETCH_STORAGE_GET_REQUEST_BLOB_TASK_H_

#include "base/callback_forward.h"
#include "base/memory/scoped_refptr.h"
#include "content/browser/background_fetch/background_fetch_request_info.h"
#include "content/browser/background_fetch/storage/database_task.h"
#include "storage/browser/blob/blob_data_handle.h"
#include "third_party/blink/public/common/service_worker/service_worker_status_code.h"

namespace content {
namespace background_fetch {

class GetRequestBlobTask : public DatabaseTask {
 public:
  using GetRequestBlobCallback =
      base::OnceCallback<void(blink::mojom::BackgroundFetchError,
                              blink::mojom::SerializedBlobPtr)>;

  GetRequestBlobTask(
      DatabaseTaskHost* host,
      const BackgroundFetchRegistrationId& registration_id,
      const scoped_refptr<BackgroundFetchRequestInfo>& request_info,
      GetRequestBlobCallback callback);

  ~GetRequestBlobTask() override;

  // DatabaseTask implementation:
  void Start() override;

 private:
  void DidOpenCache(int64_t trace_id,
                    CacheStorageCacheHandle handle,
                    blink::mojom::CacheStorageError error);

  void DidMatchRequest(CacheStorageCacheHandle handle,
                       int64_t trace_id,
                       blink::mojom::CacheStorageError error,
                       std::vector<CacheStorageCache::CacheEntry> entries);

  void FinishWithError(blink::mojom::BackgroundFetchError error) override;

  std::string HistogramName() const override;

  BackgroundFetchRegistrationId registration_id_;
  scoped_refptr<BackgroundFetchRequestInfo> request_info_;
  GetRequestBlobCallback callback_;

  blink::mojom::SerializedBlobPtr blob_;

  base::WeakPtrFactory<GetRequestBlobTask> weak_factory_;  // Keep as last.

  DISALLOW_COPY_AND_ASSIGN(GetRequestBlobTask);
};

}  // namespace background_fetch
}  // namespace content

#endif  // CONTENT_BROWSER_BACKGROUND_FETCH_STORAGE_GET_REQUEST_BLOB_TASK_H_
