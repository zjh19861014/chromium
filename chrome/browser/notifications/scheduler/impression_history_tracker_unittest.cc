// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>
#include <vector>

#include "base/test/scoped_task_environment.h"
#include "chrome/browser/notifications/scheduler/impression_history_tracker.h"
#include "chrome/browser/notifications/scheduler/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace notifications {
namespace {

struct TestCase {
  // Input data that will be pushed to the target class.
  std::vector<test::ImpressionTestData> input;

  // Expected output data.
  std::vector<test::ImpressionTestData> expected;
};

// Verifies the |output|.
void VerifyClientStates(
    const std::vector<test::ImpressionTestData>& expected_test_data,
    const ImpressionHistoryTracker::ClientStates& output) {
  ImpressionHistoryTracker::ClientStates expected_client_states;
  test::AddImpressionTestData(expected_test_data, &expected_client_states);

  DCHECK_EQ(expected_client_states.size(), output.size());
  for (const auto& expected : expected_client_states) {
    auto output_it = output.find(expected.first);
    DCHECK(output_it != output.end());
    EXPECT_EQ(*expected.second, *output_it->second)
        << "Unmatch client states: \n"
        << "Expected: \n"
        << expected.second->DebugPrint() << " \n"
        << "Acutual: \n"
        << output_it->second->DebugPrint();
  }
}

// TODO(xingliu): Add more test cases following the test doc.
class ImpressionHistoryTrackerTest : public testing::Test {
 public:
  ImpressionHistoryTrackerTest() = default;
  ~ImpressionHistoryTrackerTest() override = default;

  void SetUp() override {
    config_.impression_expiration = base::TimeDelta::FromDays(28);
    config_.suppression_duration = base::TimeDelta::FromDays(56);
  }

 protected:
  void RunTestCase(TestCase test_case) {
    // Prepare test input data.
    ImpressionHistoryTracker::ClientStates input_states;
    test::AddImpressionTestData(test_case.input, &input_states);

    // Do stuff.
    CreateTracker(std::move(input_states));
    tracker()->AnalyzeImpressionHistory();

    // Verify output data.
    VerifyClientStates(test_case.expected, tracker()->GetClientStates());
  }

  // Create the test target and push in data.
  void CreateTracker(ImpressionHistoryTracker::ClientStates states) {
    impression_trakcer_ = std::make_unique<ImpressionHistoryTrackerImpl>(
        config_, std::move(states));
  }

  const SchedulerConfig& config() const { return config_; }
  ImpressionHistoryTracker* tracker() { return impression_trakcer_.get(); }

 private:
  base::test::ScopedTaskEnvironment scoped_task_environment_;
  SchedulerConfig config_;
  std::unique_ptr<ImpressionHistoryTracker> impression_trakcer_;

  DISALLOW_COPY_AND_ASSIGN(ImpressionHistoryTrackerTest);
};

// Verifies expired impression will be deleted.
TEST_F(ImpressionHistoryTrackerTest, DeleteExpiredImpression) {
  TestCase test_case;
  auto expired_create_time = base::Time::Now() - base::TimeDelta::FromDays(1) -
                             config().impression_expiration;
  auto not_expired_time = base::Time::Now() + base::TimeDelta::FromDays(1) -
                          config().impression_expiration;
  Impression expired{expired_create_time, UserFeedback::kNoFeedback,
                     ImpressionResult::kInvalid, false /* integrated */};
  Impression not_expired{not_expired_time, UserFeedback::kNoFeedback,
                         ImpressionResult::kInvalid, false /* integrated */};

  test_case.input = {{SchedulerClientType::kTest1,
                      2 /* current_max_daily_show */,
                      {expired, expired, not_expired},
                      base::nullopt /* suppression_info */}};

  // Expired impression created in |expired_create_time| should be deleted.
  // No change expected on the next impression, which is not expired and no user
  // feedback .
  test_case.expected = {{SchedulerClientType::kTest1,
                         2 /* current_max_daily_show */,
                         {not_expired},
                         base::nullopt /* suppression_info */}};

  RunTestCase(std::move(test_case));
}

// Verifies positive impression will increase the daily max.
TEST_F(ImpressionHistoryTrackerTest, PositiveImpression) {
  TestCase test_case;
  base::Time create_time = base::Time::Now() - base::TimeDelta::FromSeconds(1);

  test_case.input = {{SchedulerClientType::kTest1,
                      2 /* current_max_daily_show */,
                      {{create_time, UserFeedback::kHelpful,
                        ImpressionResult::kInvalid, false /* integrated */}},
                      base::nullopt /* suppression_info */}};

  // Positive impression should bump |the current_max_daily_show| and update
  // other data.
  test_case.expected = {{SchedulerClientType::kTest1,
                         3 /* current_max_daily_show */,
                         {{create_time, UserFeedback::kHelpful,
                           ImpressionResult::kPositive, true /* integrated */}},
                         base::nullopt /* suppression_info */}};

  RunTestCase(std::move(test_case));
}

}  // namespace
}  // namespace notifications
