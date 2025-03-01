// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/performance_manager/observers/metrics_collector.h"

#include "base/test/metrics/histogram_tester.h"
#include "base/test/simple_test_tick_clock.h"
#include "build/build_config.h"
#include "chrome/browser/performance_manager/graph/frame_node_impl.h"
#include "chrome/browser/performance_manager/graph/graph_test_harness.h"
#include "chrome/browser/performance_manager/graph/page_node_impl.h"
#include "chrome/browser/performance_manager/graph/process_node_impl.h"
#include "chrome/browser/performance_manager/performance_manager_clock.h"
#include "components/ukm/test_ukm_recorder.h"
#include "url/gurl.h"

namespace performance_manager {

const char kResponsivenessMeasurement[] = "ResponsivenessMeasurement";
const char kExpectedQueueingTime[] = "ExpectedTaskQueueingDuration";
const base::TimeDelta kTestMetricsReportDelayTimeout =
    kMetricsReportDelayTimeout + base::TimeDelta::FromSeconds(1);
const GURL kDummyUrl("http://www.example.org");

// TODO(crbug.com/759905) Enable on Windows once this bug is fixed.
#if defined(OS_WIN)
#define MAYBE_MetricsCollectorTest DISABLED_MetricsCollectorTest
#else
#define MAYBE_MetricsCollectorTest MetricsCollectorTest
#endif
class MAYBE_MetricsCollectorTest : public GraphTestHarness {
 public:
  MAYBE_MetricsCollectorTest() : GraphTestHarness() {}

  void SetUp() override {
    metrics_collector_ = std::make_unique<MetricsCollector>();
    PerformanceManagerClock::SetClockForTesting(&clock_);

    // Sets a valid starting time.
    clock_.SetNowTicks(base::TimeTicks::Now());
    graph()->RegisterObserver(metrics_collector_.get());
  }

  void TearDown() override {
    graph()->UnregisterObserver(metrics_collector_.get());
    PerformanceManagerClock::ResetClockForTesting();
  }

 protected:
  static constexpr uint64_t kDummyID = 1u;

  void AdvanceClock(base::TimeDelta delta) { clock_.Advance(delta); }

  base::HistogramTester histogram_tester_;
  base::SimpleTestTickClock clock_;

 private:
  std::unique_ptr<MetricsCollector> metrics_collector_;

  DISALLOW_COPY_AND_ASSIGN(MAYBE_MetricsCollectorTest);
};

TEST_F(MAYBE_MetricsCollectorTest, FromBackgroundedToFirstTitleUpdatedUMA) {
  auto page_node = CreateNode<PageNodeImpl>(nullptr /*TEST*/);

  page_node->OnMainFrameNavigationCommitted(PerformanceManagerClock::NowTicks(),
                                            kDummyID, kDummyUrl);
  AdvanceClock(kTestMetricsReportDelayTimeout);

  page_node->SetIsVisible(true);
  page_node->OnTitleUpdated();
  // The page is not backgrounded, thus no metrics recorded.
  histogram_tester_.ExpectTotalCount(kTabFromBackgroundedToFirstTitleUpdatedUMA,
                                     0);

  page_node->SetIsVisible(false);
  page_node->OnTitleUpdated();
  // The page is backgrounded, thus metrics recorded.
  histogram_tester_.ExpectTotalCount(kTabFromBackgroundedToFirstTitleUpdatedUMA,
                                     1);
  page_node->OnTitleUpdated();
  // Metrics should only be recorded once per background period, thus metrics
  // not recorded.
  histogram_tester_.ExpectTotalCount(kTabFromBackgroundedToFirstTitleUpdatedUMA,
                                     1);

  page_node->SetIsVisible(true);
  page_node->SetIsVisible(false);
  page_node->OnTitleUpdated();
  // The page is backgrounded from foregrounded, thus metrics recorded.
  histogram_tester_.ExpectTotalCount(kTabFromBackgroundedToFirstTitleUpdatedUMA,
                                     2);
}

TEST_F(MAYBE_MetricsCollectorTest,
       FromBackgroundedToFirstTitleUpdatedUMA5MinutesTimeout) {
  auto page_node = CreateNode<PageNodeImpl>(nullptr /*TEST*/);

  page_node->OnMainFrameNavigationCommitted(PerformanceManagerClock::NowTicks(),
                                            kDummyID, kDummyUrl);
  page_node->SetIsVisible(false);
  page_node->OnTitleUpdated();
  // The page is within 5 minutes after main frame navigation was committed,
  // thus no metrics recorded.
  histogram_tester_.ExpectTotalCount(kTabFromBackgroundedToFirstTitleUpdatedUMA,
                                     0);
  AdvanceClock(kTestMetricsReportDelayTimeout);
  page_node->OnTitleUpdated();
  histogram_tester_.ExpectTotalCount(kTabFromBackgroundedToFirstTitleUpdatedUMA,
                                     1);
}

TEST_F(MAYBE_MetricsCollectorTest,
       FromBackgroundedToFirstNonPersistentNotificationCreatedUMA) {
  auto process_node = CreateNode<ProcessNodeImpl>();
  auto page_node = CreateNode<PageNodeImpl>(nullptr /*TEST*/);
  auto frame_node = CreateNode<FrameNodeImpl>(process_node.get(),
                                              page_node.get(), nullptr, 0);

  page_node->OnMainFrameNavigationCommitted(PerformanceManagerClock::NowTicks(),
                                            kDummyID, kDummyUrl);
  AdvanceClock(kTestMetricsReportDelayTimeout);

  page_node->SetIsVisible(true);
  frame_node->OnNonPersistentNotificationCreated();
  // The page is not backgrounded, thus no metrics recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstNonPersistentNotificationCreatedUMA, 0);

  page_node->SetIsVisible(false);
  frame_node->OnNonPersistentNotificationCreated();
  // The page is backgrounded, thus metrics recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstNonPersistentNotificationCreatedUMA, 1);
  frame_node->OnNonPersistentNotificationCreated();
  // Metrics should only be recorded once per background period, thus metrics
  // not recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstNonPersistentNotificationCreatedUMA, 1);

  page_node->SetIsVisible(true);
  page_node->SetIsVisible(false);
  frame_node->OnNonPersistentNotificationCreated();
  // The page is backgrounded from foregrounded, thus metrics recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstNonPersistentNotificationCreatedUMA, 2);
}

TEST_F(
    MAYBE_MetricsCollectorTest,
    FromBackgroundedToFirstNonPersistentNotificationCreatedUMA5MinutesTimeout) {
  auto process_node = CreateNode<ProcessNodeImpl>();
  auto page_node = CreateNode<PageNodeImpl>(nullptr /*TEST*/);
  auto frame_node = CreateNode<FrameNodeImpl>(process_node.get(),
                                              page_node.get(), nullptr, 0);

  page_node->OnMainFrameNavigationCommitted(PerformanceManagerClock::NowTicks(),
                                            kDummyID, kDummyUrl);
  page_node->SetIsVisible(false);
  frame_node->OnNonPersistentNotificationCreated();
  // The page is within 5 minutes after main frame navigation was committed,
  // thus no metrics recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstNonPersistentNotificationCreatedUMA, 0);
  AdvanceClock(kTestMetricsReportDelayTimeout);
  frame_node->OnNonPersistentNotificationCreated();
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstNonPersistentNotificationCreatedUMA, 1);
}

TEST_F(MAYBE_MetricsCollectorTest, FromBackgroundedToFirstFaviconUpdatedUMA) {
  auto page_node = CreateNode<PageNodeImpl>(nullptr /*TEST*/);

  page_node->OnMainFrameNavigationCommitted(PerformanceManagerClock::NowTicks(),
                                            kDummyID, kDummyUrl);
  AdvanceClock(kTestMetricsReportDelayTimeout);

  page_node->SetIsVisible(true);
  page_node->OnFaviconUpdated();
  // The page is not backgrounded, thus no metrics recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstFaviconUpdatedUMA, 0);

  page_node->SetIsVisible(false);
  page_node->OnFaviconUpdated();
  // The page is backgrounded, thus metrics recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstFaviconUpdatedUMA, 1);
  page_node->OnFaviconUpdated();
  // Metrics should only be recorded once per background period, thus metrics
  // not recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstFaviconUpdatedUMA, 1);

  page_node->SetIsVisible(true);
  page_node->SetIsVisible(false);
  page_node->OnFaviconUpdated();
  // The page is backgrounded from foregrounded, thus metrics recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstFaviconUpdatedUMA, 2);
}

TEST_F(MAYBE_MetricsCollectorTest,
       FromBackgroundedToFirstFaviconUpdatedUMA5MinutesTimeout) {
  auto page_node = CreateNode<PageNodeImpl>(nullptr /*TEST*/);

  page_node->OnMainFrameNavigationCommitted(PerformanceManagerClock::NowTicks(),
                                            kDummyID, kDummyUrl);
  page_node->SetIsVisible(false);
  page_node->OnFaviconUpdated();
  // The page is within 5 minutes after main frame navigation was committed,
  // thus no metrics recorded.
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstFaviconUpdatedUMA, 0);
  AdvanceClock(kTestMetricsReportDelayTimeout);
  page_node->OnFaviconUpdated();
  histogram_tester_.ExpectTotalCount(
      kTabFromBackgroundedToFirstFaviconUpdatedUMA, 1);
}

// Flaky test: https://crbug.com/833028
TEST_F(MAYBE_MetricsCollectorTest, ResponsivenessMetric) {
  auto process_node = CreateNode<ProcessNodeImpl>();
  auto page_node = CreateNode<PageNodeImpl>(nullptr /*TEST*/);
  auto frame_node = CreateNode<FrameNodeImpl>(process_node.get(),
                                              page_node.get(), nullptr, 0);

  ukm::TestUkmRecorder ukm_recorder;
  graph()->set_ukm_recorder(&ukm_recorder);

  ukm::SourceId id = ukm_recorder.GetNewSourceID();
  GURL url = GURL("https://google.com/foobar");
  ukm_recorder.UpdateSourceURL(id, url);
  page_node->SetUkmSourceId(id);
  page_node->OnMainFrameNavigationCommitted(PerformanceManagerClock::NowTicks(),
                                            kDummyID, kDummyUrl);

  for (int count = 1; count < kDefaultFrequencyUkmEQTReported; ++count) {
    process_node->SetExpectedTaskQueueingDuration(
        base::TimeDelta::FromMilliseconds(3));
    EXPECT_EQ(0U, ukm_recorder.entries_count());
    EXPECT_EQ(1U, ukm_recorder.sources_count());
  }
  process_node->SetExpectedTaskQueueingDuration(
      base::TimeDelta::FromMilliseconds(4));
  EXPECT_EQ(1U, ukm_recorder.sources_count());
  EXPECT_EQ(1U, ukm_recorder.entries_count());
  for (int count = 1; count < kDefaultFrequencyUkmEQTReported; ++count) {
    process_node->SetExpectedTaskQueueingDuration(
        base::TimeDelta::FromMilliseconds(3));
    EXPECT_EQ(1U, ukm_recorder.entries_count());
    EXPECT_EQ(1U, ukm_recorder.sources_count());
  }
  process_node->SetExpectedTaskQueueingDuration(
      base::TimeDelta::FromMilliseconds(4));
  EXPECT_EQ(1U, ukm_recorder.sources_count());
  EXPECT_EQ(2U, ukm_recorder.entries_count());

  const auto& entries =
      ukm_recorder.GetEntriesByName(kResponsivenessMeasurement);
  EXPECT_EQ(2U, entries.size());
  for (const auto* entry : entries) {
    ukm_recorder.ExpectEntrySourceHasUrl(entry, url);
    ukm_recorder.ExpectEntryMetric(entry, kExpectedQueueingTime, 4);
  }
}

}  // namespace performance_manager
