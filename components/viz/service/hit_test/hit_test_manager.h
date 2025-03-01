// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_HIT_TEST_HIT_TEST_MANAGER_H_
#define COMPONENTS_VIZ_SERVICE_HIT_TEST_HIT_TEST_MANAGER_H_

#include "base/optional.h"
#include "base/timer/elapsed_timer.h"
#include "components/viz/common/hit_test/aggregated_hit_test_region.h"
#include "components/viz/common/surfaces/surface_id.h"
#include "components/viz/service/surfaces/surface_manager.h"
#include "components/viz/service/surfaces/surface_observer.h"
#include "components/viz/service/viz_service_export.h"
#include "services/viz/public/interfaces/hit_test/hit_test_region_list.mojom.h"

namespace viz {

class LatestLocalSurfaceIdLookupDelegate;

// HitTestManager manages the collection of HitTestRegionList objects
// submitted in calls to SubmitCompositorFrame.  This collection is
// used by HitTestAggregator.
class VIZ_SERVICE_EXPORT HitTestManager : public SurfaceObserver {
 public:
  explicit HitTestManager(SurfaceManager* surface_manager);
  virtual ~HitTestManager();

  // SurfaceObserver:
  void OnFirstSurfaceActivation(const SurfaceInfo& surface_info) override {}
  void OnSurfaceActivated(const SurfaceId& surface_id,
                          base::Optional<base::TimeDelta> duration) override;
  void OnSurfaceMarkedForDestruction(const SurfaceId& surface_id) override {}
  bool OnSurfaceDamaged(const SurfaceId& surface_id,
                        const BeginFrameAck& ack) override;
  void OnSurfaceDestroyed(const SurfaceId& surface_id) override;
  void OnSurfaceDamageExpected(const SurfaceId& surface_id,
                               const BeginFrameArgs& args) override {}

  // Called when HitTestRegionList is submitted along with every call
  // to SubmitCompositorFrame.
  void SubmitHitTestRegionList(
      const SurfaceId& surface_id,
      const uint64_t frame_index,
      base::Optional<HitTestRegionList> hit_test_region_list);

  // Returns the HitTestRegionList corresponding to the given
  // |frame_sink_id| and the active CompositorFrame matched by frame_index.
  // The returned pointer is not stable and should not be stored or used after
  // calling any non-const methods on this class. ActiveFrameIndex is stored
  // if |store_active_frame_index| is given, which is used to detect updates.
  const HitTestRegionList* GetActiveHitTestRegionList(
      LatestLocalSurfaceIdLookupDelegate* delegate,
      const FrameSinkId& frame_sink_id,
      uint64_t* store_active_frame_index = nullptr) const;

  int64_t GetTraceId(const SurfaceId& id) const;

  const base::flat_set<FrameSinkId>* GetHitTestAsyncQueriedDebugRegions(
      const FrameSinkId& root_frame_sink_id) const;
  void SetHitTestAsyncQueriedDebugRegions(
      const FrameSinkId& root_frame_sink_id,
      const std::vector<FrameSinkId>& hit_test_async_queried_debug_queue);

  uint64_t submit_hit_test_region_list_index() const {
    return submit_hit_test_region_list_index_;
  }

 private:
  bool ValidateHitTestRegionList(const SurfaceId& surface_id,
                                 HitTestRegionList* hit_test_region_list);

  SurfaceManager* const surface_manager_;

  std::map<SurfaceId, base::flat_map<uint64_t, HitTestRegionList>>
      hit_test_region_lists_;

  struct HitTestAsyncQueriedDebugRegion {
    HitTestAsyncQueriedDebugRegion();
    explicit HitTestAsyncQueriedDebugRegion(
        base::flat_set<FrameSinkId> regions);
    ~HitTestAsyncQueriedDebugRegion();

    HitTestAsyncQueriedDebugRegion(HitTestAsyncQueriedDebugRegion&&);
    HitTestAsyncQueriedDebugRegion& operator=(HitTestAsyncQueriedDebugRegion&&);

    base::flat_set<FrameSinkId> regions;
    base::ElapsedTimer timer;
  };

  // We store the async queried regions for each |root_frame_sink_id|. If viz
  // hit-test debug is enabled, We will highlight the regions red in
  // HitTestAggregator for 2 seconds, or until the next async queried event.
  base::flat_map<FrameSinkId, HitTestAsyncQueriedDebugRegion>
      hit_test_async_queried_debug_regions_;

  // Keeps track of the number of submitted HitTestRegionLists. This allows the
  // HitTestAggregators to stay in sync with the HitTestManager and only
  // aggregate when there is new hit-test data.
  uint64_t submit_hit_test_region_list_index_ = 0;

  DISALLOW_COPY_AND_ASSIGN(HitTestManager);
};

}  // namespace viz

#endif  // COMPONENTS_VIZ_SERVICE_HIT_TEST_HIT_TEST_MANAGER_H_
