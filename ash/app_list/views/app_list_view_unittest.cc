// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/app_list/views/app_list_view.h"

#include <stddef.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ash/app_list/model/search/search_box_model.h"
#include "ash/app_list/pagination_model.h"
#include "ash/app_list/test/app_list_test_model.h"
#include "ash/app_list/test/app_list_test_view_delegate.h"
#include "ash/app_list/test/test_search_result.h"
#include "ash/app_list/views/app_list_folder_view.h"
#include "ash/app_list/views/app_list_item_view.h"
#include "ash/app_list/views/app_list_main_view.h"
#include "ash/app_list/views/apps_container_view.h"
#include "ash/app_list/views/apps_grid_view.h"
#include "ash/app_list/views/contents_view.h"
#include "ash/app_list/views/expand_arrow_view.h"
#include "ash/app_list/views/folder_header_view.h"
#include "ash/app_list/views/search_box_view.h"
#include "ash/app_list/views/search_result_answer_card_view.h"
#include "ash/app_list/views/search_result_list_view.h"
#include "ash/app_list/views/search_result_page_view.h"
#include "ash/app_list/views/search_result_suggestion_chip_view.h"
#include "ash/app_list/views/search_result_tile_item_list_view.h"
#include "ash/app_list/views/search_result_tile_item_view.h"
#include "ash/app_list/views/search_result_view.h"
#include "ash/app_list/views/suggestion_chip_container_view.h"
#include "ash/app_list/views/suggestion_chip_view.h"
#include "ash/app_list/views/test/apps_grid_view_test_api.h"
#include "ash/public/cpp/app_list/app_list_config.h"
#include "ash/public/cpp/app_list/app_list_features.h"
#include "ash/public/cpp/presentation_time_recorder.h"
#include "base/macros.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/icu_test_util.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "chromeos/constants/chromeos_switches.h"
#include "services/content/public/cpp/test/fake_navigable_contents.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/models/simple_menu_model.h"
#include "ui/chromeos/search_box/search_box_constants.h"
#include "ui/compositor/layer_animator.h"
#include "ui/events/event_utils.h"
#include "ui/keyboard/keyboard_controller.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/test/views_test_base.h"
#include "ui/views/view_model.h"

namespace app_list {
namespace test {

namespace {

constexpr int kInitialItems = 34;

template <class T>
size_t GetVisibleViews(const std::vector<T*>& tiles) {
  size_t count = 0;
  for (const auto& tile : tiles) {
    if (tile->visible())
      count++;
  }
  return count;
}

// A standard set of checks on a view, e.g., ensuring it is drawn and visible.
void CheckView(views::View* subview) {
  ASSERT_TRUE(subview);
  EXPECT_TRUE(subview->parent());
  EXPECT_TRUE(subview->visible());
  EXPECT_TRUE(subview->IsDrawn());
  EXPECT_FALSE(subview->bounds().IsEmpty());
}

class TestStartPageSearchResult : public TestSearchResult {
 public:
  TestStartPageSearchResult() {
    set_display_type(ash::SearchResultDisplayType::kRecommendation);
  }
  ~TestStartPageSearchResult() override = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(TestStartPageSearchResult);
};

class AppListViewTest : public views::ViewsTestBase,
                        public testing::WithParamInterface<bool> {
 public:
  AppListViewTest() = default;
  ~AppListViewTest() override = default;

  void SetUp() override {
    AppListView::SetShortAnimationForTesting(true);
    if (testing::UnitTest::GetInstance()->current_test_info()->value_param()) {
      // Setup right to left environment if necessary.
      is_rtl_ = GetParam();
      if (is_rtl_)
        base::i18n::SetICUDefaultLocale("he");
    }
    views::ViewsTestBase::SetUp();
    ash::PresentationTimeRecorder::SetReportPresentationTimeImmediatelyForTest(
        true);
  }

  void TearDown() override {
    ash::PresentationTimeRecorder::SetReportPresentationTimeImmediatelyForTest(
        false);
    view_->GetWidget()->Close();
    views::ViewsTestBase::TearDown();
    AppListView::SetShortAnimationForTesting(false);
  }

 protected:
  void Show() { view_->ShowWhenReady(); }

  void Initialize(int initial_apps_page,
                  bool is_tablet_mode,
                  bool is_side_shelf) {
    delegate_ = std::make_unique<AppListTestViewDelegate>();
    view_ = new AppListView(delegate_.get());
    AppListView::InitParams params;
    params.parent = GetContext();
    params.initial_apps_page = initial_apps_page;
    params.is_tablet_mode = is_tablet_mode;
    params.is_side_shelf = is_side_shelf;
    view_->Initialize(params);
    test_api_.reset(new AppsGridViewTestApi(apps_grid_view()));
    EXPECT_FALSE(view_->GetWidget()->IsVisible());
  }

  // Switches the launcher to |state| and lays out to ensure all launcher pages
  // are in the correct position. Checks that the state is where it should be
  // and returns false on failure.
  bool SetAppListState(ash::AppListState state) {
    ContentsView* contents_view = view_->app_list_main_view()->contents_view();

    // The default method of changing the state to |kStateSearchResults| is via
    // |ShowSearchResults|
    if (state == ash::AppListState::kStateSearchResults)
      contents_view->ShowSearchResults(true);
    else
      contents_view->SetActiveState(state);

    contents_view->Layout();
    return IsStateShown(state);
  }

  // Returns true if all of the pages are in their correct position for |state|.
  bool IsStateShown(ash::AppListState state) {
    ContentsView* contents_view = view_->app_list_main_view()->contents_view();
    bool success = true;
    for (int i = 0; i < contents_view->NumLauncherPages(); ++i) {
      success = success &&
                (contents_view->GetPageView(i)->GetPageBoundsForState(state) ==
                 contents_view->GetPageView(i)->bounds());
    }
    return success && state == delegate_->GetModel()->state();
  }

  // Checks the search box widget is at |expected| in the contents view's
  // coordinate space.
  bool CheckSearchBoxWidget(const gfx::Rect& expected) {
    ContentsView* contents_view = view_->app_list_main_view()->contents_view();
    // Adjust for the search box view's shadow.
    gfx::Rect expected_with_shadow =
        view_->app_list_main_view()
            ->search_box_view()
            ->GetViewBoundsForSearchBoxContentsBounds(expected);
    gfx::Point point = expected_with_shadow.origin();
    views::View::ConvertPointToScreen(contents_view, &point);

    return gfx::Rect(point, expected_with_shadow.size()) ==
           view_->search_box_widget()->GetWindowBoundsInScreen();
  }

  // Gets the PaginationModel owned by |view_|.
  PaginationModel* GetPaginationModel() const {
    return view_->GetAppsPaginationModel();
  }

  SearchBoxView* search_box_view() {
    return view_->app_list_main_view()->search_box_view();
  }

  ContentsView* contents_view() {
    return view_->app_list_main_view()->contents_view();
  }

  AppsGridView* apps_grid_view() {
    return contents_view()->GetAppsContainerView()->apps_grid_view();
  }

  gfx::Point GetPointBetweenTwoApps() {
    const views::ViewModelT<AppListItemView>* view_model =
        apps_grid_view()->view_model();
    const gfx::Rect bounds_1 = view_model->view_at(0)->GetBoundsInScreen();
    const gfx::Rect bounds_2 = view_model->view_at(1)->GetBoundsInScreen();

    return gfx::Point(bounds_1.right() + (bounds_2.x() - bounds_1.right()) / 2,
                      bounds_1.y());
  }

  int show_wallpaper_context_menu_count() {
    return delegate_->show_wallpaper_context_menu_count();
  }

  AppListView* view_ = nullptr;  // Owned by native widget.
  std::unique_ptr<AppListTestViewDelegate> delegate_;
  std::unique_ptr<AppsGridViewTestApi> test_api_;

  // Used by AppListFolderView::UpdatePreferredBounds.
  keyboard::KeyboardController keyboard_controller_;

  bool is_rtl_ = false;

  DISALLOW_COPY_AND_ASSIGN(AppListViewTest);
};

class AppListViewFocusTest : public views::ViewsTestBase,
                             public testing::WithParamInterface<bool> {
 public:
  AppListViewFocusTest() = default;
  ~AppListViewFocusTest() override = default;

  // testing::Test
  void SetUp() override {
    if (testing::UnitTest::GetInstance()->current_test_info()->value_param()) {
      // Setup right to left environment if necessary.
      is_rtl_ = GetParam();
      if (is_rtl_)
        base::i18n::SetICUDefaultLocale("he");
    }

    views::ViewsTestBase::SetUp();

    // Initialize app list view.
    fake_card_contents_.set_default_response_headers(
        SearchResultAnswerCardView::CreateAnswerCardResponseHeadersForTest(
            "weather", "Unimportant Title"));
    delegate_ = std::make_unique<AppListTestViewDelegate>();
    view_ = new AppListView(delegate_.get());
    AppListView::InitParams params;
    params.parent = GetContext();
    view_->Initialize(params);
    test_api_.reset(new AppsGridViewTestApi(apps_grid_view()));
    suggestions_container_ = contents_view()
                                 ->GetAppsContainerView()
                                 ->suggestion_chip_container_view_for_test();
    expand_arrow_view_ = contents_view()->expand_arrow_view();

    // Add suggestion apps, a folder with apps and other app list items.
    const int kSuggestionAppNum = 3;
    const int kItemNumInFolder = 25;
    const int kAppListItemNum = test_api_->TilesPerPage(0) + 1;
    AppListTestModel* model = delegate_->GetTestModel();
    SearchModel* search_model = delegate_->GetSearchModel();
    for (size_t i = 0; i < kSuggestionAppNum; i++) {
      search_model->results()->Add(
          std::make_unique<TestStartPageSearchResult>());
    }
    AppListFolderItem* folder_item =
        model->CreateAndPopulateFolderWithApps(kItemNumInFolder);
    model->PopulateApps(kAppListItemNum);
    suggestions_container()->Update();
    EXPECT_EQ(static_cast<size_t>(kAppListItemNum + 1),
              model->top_level_item_list()->item_count());
    EXPECT_EQ(folder_item->id(),
              model->top_level_item_list()->item_at(0)->id());

    // Disable animation timer.
    view_->GetWidget()->GetLayer()->GetAnimator()->set_disable_timer_for_test(
        true);

    // The Update above will elicit a navigation. Wait for it.
    delegate_->fake_navigable_contents_factory()
        .WaitForAndBindNextContentsRequest(&fake_card_contents_);
  }

  void TearDown() override {
    view_->GetWidget()->Close();
    views::ViewsTestBase::TearDown();
  }

  void SetAppListState(ash::mojom::AppListViewState state) {
    if (state == ash::mojom::AppListViewState::kClosed) {
      view_->Dismiss();
      return;
    }
    view_->SetState(state);
  }

  void Show() { view_->ShowWhenReady(); }

  void SimulateKeyPress(ui::KeyboardCode key_code,
                        bool shift_down,
                        bool ctrl_down = false) {
    ui::KeyEvent key_event(ui::ET_KEY_PRESSED, key_code,
                           shift_down
                               ? ui::EF_SHIFT_DOWN
                               : ctrl_down ? ui::EF_CONTROL_DOWN : ui::EF_NONE);
    view_->GetWidget()->OnKeyEvent(&key_event);
  }

  // Add search results for test on focus movement.
  void SetUpSearchResults(int tile_results_num,
                          int list_results_num,
                          bool card_result) {
    std::vector<std::pair<SearchResult::DisplayType, int>> result_types;
    result_types.push_back(
        std::make_pair(ash::SearchResultDisplayType::kTile, tile_results_num));
    if (card_result)
      result_types.push_back(
          std::make_pair(ash::SearchResultDisplayType::kCard, 1));
    result_types.push_back(
        std::make_pair(ash::SearchResultDisplayType::kList, list_results_num));

    SearchModel::SearchResults* results =
        delegate_->GetSearchModel()->results();
    results->DeleteAll();
    double display_score = result_types.size();
    for (const auto& data : result_types) {
      // Set the display score of the results in each group in decreasing order
      // (so the earlier groups have higher display score, and therefore appear
      // first).
      display_score -= 0.5;
      for (int i = 0; i < data.second; ++i) {
        std::unique_ptr<TestSearchResult> result =
            std::make_unique<TestSearchResult>();
        result->set_display_type(data.first);
        result->set_display_score(display_score);
        result->set_title(base::ASCIIToUTF16("Test"));
        if (data.first == ash::SearchResultDisplayType::kCard) {
          const GURL kFakeCardUrl = GURL("https://www.google.com/coac?q=fake");
          result->set_query_url(kFakeCardUrl);
        }
        results->Add(std::move(result));
      }
    }

    // Adding results will schedule Update().
    RunPendingMessages();
  }

  // Add search results for test on embedded Assistant UI.
  void SetUpSearchResultsForAssistantUI(int list_results_num,
                                        int index_open_assistant_ui) {
    SearchModel::SearchResults* results =
        delegate_->GetSearchModel()->results();
    results->DeleteAll();
    double display_score = list_results_num;
    for (int i = 0; i < list_results_num; ++i) {
      // Set the display score of the results in decreasing order
      // (so the earlier groups have higher display score, and therefore appear
      // first).
      display_score -= 1;
      std::unique_ptr<TestSearchResult> result =
          std::make_unique<TestSearchResult>();
      result->set_display_type(ash::SearchResultDisplayType::kList);
      result->set_display_score(display_score);
      result->set_title(base::ASCIIToUTF16("Test" + base::NumberToString(i)));
      result->set_result_id("Test" + base::NumberToString(i));
      if (i == index_open_assistant_ui)
        result->set_is_omnibox_search(true);

      results->Add(std::move(result));
    }

    // Adding results will schedule Update().
    RunPendingMessages();
  }

  void ClearSearchResults() {
    delegate_->GetSearchModel()->results()->DeleteAll();
  }

  void AddSearchResultWithTitleAndScore(const base::StringPiece& title,
                                        double score) {
    std::unique_ptr<TestSearchResult> result =
        std::make_unique<TestSearchResult>();
    result->set_display_type(ash::SearchResultDisplayType::kList);
    result->set_display_score(score);
    result->set_title(ASCIIToUTF16(title));
    delegate_->GetSearchModel()->results()->Add(std::move(result));
    RunPendingMessages();
  }

  int GetOpenFirstSearchResultCount() {
    std::map<size_t, int>& counts = delegate_->open_search_result_counts();
    if (counts.size() == 0)
      return 0;
    return counts[0];
  }

  int GetTotalOpenSearchResultCount() {
    return delegate_->open_search_result_count();
  }

  int GetTotalOpenAssistantUICount() {
    return delegate_->open_assistant_ui_count();
  }

  // Test focus traversal across all the views in |view_list|. The initial focus
  // is expected to be on the first view in |view_list|. The final focus is
  // expected to be on the last view in |view_list| after |view_list.size()-1|
  // key events are pressed.
  void TestFocusTraversal(const std::vector<views::View*>& view_list,
                          ui::KeyboardCode key_code,
                          bool shift_down) {
    EXPECT_EQ(view_list[0], focused_view());
    for (size_t i = 1; i < view_list.size(); ++i) {
      SimulateKeyPress(key_code, shift_down);
      EXPECT_EQ(view_list[i], focused_view());
    }
  }

  // Test the behavior triggered by left and right key when focus is on the
  // |textfield|. Does not insert text.
  void TestLeftAndRightKeyTraversalOnTextfield(views::Textfield* textfield) {
    EXPECT_TRUE(textfield->text().empty());
    EXPECT_EQ(textfield, focused_view());

    views::View* next_view =
        view_->GetWidget()->GetFocusManager()->GetNextFocusableView(
            textfield, view_->GetWidget(), false, false);
    views::View* prev_view =
        view_->GetWidget()->GetFocusManager()->GetNextFocusableView(
            textfield, view_->GetWidget(), true, false);

    // Only need to hit left or right key once to move focus outside the
    // textfield when it is empty.
    SimulateKeyPress(ui::VKEY_RIGHT, false);
    EXPECT_EQ(is_rtl_ ? prev_view : next_view, focused_view());

    SimulateKeyPress(ui::VKEY_LEFT, false);
    EXPECT_EQ(textfield, focused_view());

    SimulateKeyPress(ui::VKEY_LEFT, false);
    EXPECT_EQ(is_rtl_ ? next_view : prev_view, focused_view());

    SimulateKeyPress(ui::VKEY_RIGHT, false);
    EXPECT_EQ(textfield, focused_view());
  }

  // Test the behavior triggered by left and right key when focus is on the
  // |textfield|. This includes typing text into the field.
  void TestLeftAndRightKeyOnTextfieldWithText(views::Textfield* textfield,
                                              bool text_rtl) {
    // Test initial traversal
    TestLeftAndRightKeyTraversalOnTextfield(textfield);

    // Type something in textfield.
    base::string16 text =
        text_rtl
            // Arabic word of "test".
            ? base::UTF8ToUTF16(
                  "\xd8\xa7\xd8\xae\xd8\xaa\xd8\xa8\xd8\xa7\xd8\xb1")
            : base::UTF8ToUTF16("test");
    textfield->InsertText(text);
    views::View* next_view = next_view =
        view_->GetWidget()->GetFocusManager()->GetNextFocusableView(
            textfield, view_->GetWidget(), false, false);
    views::View* prev_view = prev_view =
        view_->GetWidget()->GetFocusManager()->GetNextFocusableView(
            textfield, view_->GetWidget(), true, false);
    EXPECT_EQ(text.length(), textfield->GetCursorPosition());
    EXPECT_FALSE(textfield->HasSelection());
    EXPECT_EQ(textfield, focused_view());

    const ui::KeyboardCode backward_key =
        text_rtl ? ui::VKEY_RIGHT : ui::VKEY_LEFT;
    const ui::KeyboardCode forward_key =
        text_rtl ? ui::VKEY_LEFT : ui::VKEY_RIGHT;

    // Move cursor backward.
    SimulateKeyPress(backward_key, false);
    EXPECT_EQ(text.length() - 1, textfield->GetCursorPosition());
    EXPECT_EQ(textfield, focused_view());

    // Move cursor forward.
    SimulateKeyPress(forward_key, false);
    EXPECT_EQ(text.length(), textfield->GetCursorPosition());
    EXPECT_EQ(textfield, focused_view());

    // Hit forward key to move focus outside the textfield.
    SimulateKeyPress(forward_key, false);
    EXPECT_EQ((!is_rtl_ && !text_rtl) || (is_rtl_ && text_rtl) ? next_view
                                                               : prev_view,
              focused_view());

    // Hit backward key to move focus back to textfield and select all text.
    SimulateKeyPress(backward_key, false);
    EXPECT_EQ(text, textfield->GetSelectedText());
    EXPECT_EQ(textfield, focused_view());

    // Hit backward key to move cursor to the beginning.
    SimulateKeyPress(backward_key, false);
    EXPECT_EQ(0U, textfield->GetCursorPosition());
    EXPECT_FALSE(textfield->HasSelection());
    EXPECT_EQ(textfield, focused_view());

    // Hit backward key to move focus outside the textfield.
    SimulateKeyPress(backward_key, false);
    EXPECT_EQ((!is_rtl_ && !text_rtl) || (is_rtl_ && text_rtl) ? prev_view
                                                               : next_view,
              focused_view());

    // Hit forward key to move focus back to textfield and select all text.
    SimulateKeyPress(forward_key, false);
    EXPECT_EQ(text, textfield->GetSelectedText());
    EXPECT_EQ(textfield, focused_view());

    // Hit forward key to move cursor to the end.
    SimulateKeyPress(forward_key, false);
    EXPECT_EQ(text.length(), textfield->GetCursorPosition());
    EXPECT_FALSE(textfield->HasSelection());
    EXPECT_EQ(textfield, focused_view());

    // Hitt forward key to move focus outside the textfield.
    SimulateKeyPress(forward_key, false);
    EXPECT_EQ((!is_rtl_ && !text_rtl) || (is_rtl_ && text_rtl) ? next_view
                                                               : prev_view,
              focused_view());

    // Clean up
    textfield->RequestFocus();
    textfield->SetText(base::UTF8ToUTF16(""));
  }

  AppListView* app_list_view() { return view_; }

  AppListMainView* main_view() { return view_->app_list_main_view(); }

  ContentsView* contents_view() {
    return view_->app_list_main_view()->contents_view();
  }

  AppsGridView* apps_grid_view() {
    return main_view()
        ->contents_view()
        ->GetAppsContainerView()
        ->apps_grid_view();
  }

  AppListFolderView* app_list_folder_view() {
    return main_view()
        ->contents_view()
        ->GetAppsContainerView()
        ->app_list_folder_view();
  }

  SearchResultContainerView* suggestions_container() {
    return suggestions_container_;
  }

  std::vector<views::View*> GetAllSuggestions() {
    std::vector<views::View*> suggestions;
    for (int i = 0; i < suggestions_container()->child_count(); ++i) {
      SearchResultSuggestionChipView* view =
          static_cast<SearchResultSuggestionChipView*>(
              suggestions_container()->child_at(i));
      if (view->visible())
        suggestions.emplace_back(view->suggestion_chip_view());
    }
    return suggestions;
  }

  SearchBoxView* search_box_view() { return main_view()->search_box_view(); }

  AppListItemView* folder_item_view() {
    return apps_grid_view()->view_model()->view_at(0);
  }

  views::View* focused_view() {
    return view_->GetWidget()->GetFocusManager()->GetFocusedView();
  }

  ExpandArrowView* expand_arrow_view() { return expand_arrow_view_; }

 protected:
  bool is_rtl_ = false;
  base::test::ScopedFeatureList scoped_feature_list_;

 private:
  AppListView* view_ = nullptr;  // Owned by native widget.
  SearchResultContainerView* suggestions_container_ =
      nullptr;                                    // Owned by view hierarchy.
  ExpandArrowView* expand_arrow_view_ = nullptr;  // Owned by view hierarchy.

  std::unique_ptr<AppListTestViewDelegate> delegate_;
  std::unique_ptr<AppsGridViewTestApi> test_api_;
  // Restores the locale to default when destructor is called.
  base::test::ScopedRestoreICUDefaultLocale restore_locale_;

  // Used by AppListFolderView::UpdatePreferredBounds.
  keyboard::KeyboardController keyboard_controller_;

  // A fake NavigableContents implementation to back card navigation requests.
  content::FakeNavigableContents fake_card_contents_;

  DISALLOW_COPY_AND_ASSIGN(AppListViewFocusTest);
};

INSTANTIATE_TEST_SUITE_P(, AppListViewFocusTest, testing::Bool());

}  // namespace

// Tests that the initial focus is on search box.
TEST_F(AppListViewFocusTest, InitialFocus) {
  Show();
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
}

// Tests the linear focus traversal in PEEKING state.
TEST_P(AppListViewFocusTest, LinearFocusTraversalInPeekingState) {
  Show();
  SetAppListState(ash::mojom::AppListViewState::kPeeking);

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  for (auto* v : GetAllSuggestions())
    forward_view_list.push_back(v);
  forward_view_list.push_back(expand_arrow_view());
  forward_view_list.push_back(search_box_view()->search_box());
  std::vector<views::View*> backward_view_list = forward_view_list;
  std::reverse(backward_view_list.begin(), backward_view_list.end());

  // Test traversal triggered by tab.
  TestFocusTraversal(forward_view_list, ui::VKEY_TAB, false);

  // Test traversal triggered by shift+tab.
  TestFocusTraversal(backward_view_list, ui::VKEY_TAB, true);

  // Test traversal triggered by right.
  TestFocusTraversal(is_rtl_ ? backward_view_list : forward_view_list,
                     ui::VKEY_RIGHT, false);

  // Test traversal triggered by left.
  TestFocusTraversal(is_rtl_ ? forward_view_list : backward_view_list,
                     ui::VKEY_LEFT, false);
}

// Tests the linear focus traversal in FULLSCREEN_ALL_APPS state.
TEST_P(AppListViewFocusTest, LinearFocusTraversalInFullscreenAllAppsState) {
  Show();
  SetAppListState(ash::mojom::AppListViewState::kFullscreenAllApps);

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  for (auto* v : GetAllSuggestions())
    forward_view_list.push_back(v);
  const views::ViewModelT<AppListItemView>* view_model =
      apps_grid_view()->view_model();
  for (int i = 0; i < view_model->view_size(); ++i)
    forward_view_list.push_back(view_model->view_at(i));
  forward_view_list.push_back(search_box_view()->search_box());
  std::vector<views::View*> backward_view_list = forward_view_list;
  std::reverse(backward_view_list.begin(), backward_view_list.end());

  // Test traversal triggered by tab.
  TestFocusTraversal(forward_view_list, ui::VKEY_TAB, false);

  // Test traversal triggered by shift+tab.
  TestFocusTraversal(backward_view_list, ui::VKEY_TAB, true);

  // Test traversal triggered by right.
  TestFocusTraversal(is_rtl_ ? backward_view_list : forward_view_list,
                     ui::VKEY_RIGHT, false);

  // Test traversal triggered by left.
  TestFocusTraversal(is_rtl_ ? forward_view_list : backward_view_list,
                     ui::VKEY_LEFT, false);
}

// Tests focus traversal in HALF state with opened search box using |VKEY_TAB|.
TEST_F(AppListViewFocusTest, TabFocusTraversalInHalfState) {
  Show();

  // Type something in search box to transition to HALF state and populate
  // fake search results.
  search_box_view()->search_box()->InsertText(base::ASCIIToUTF16("test"));
  EXPECT_EQ(app_list_view()->app_list_state(),
            ash::mojom::AppListViewState::kHalf);
  constexpr int kTileResults = 3;
  constexpr int kListResults = 2;
  SetUpSearchResults(kTileResults, kListResults, true);

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  forward_view_list.push_back(search_box_view()->close_button());
  const std::vector<SearchResultTileItemView*>& tile_views =
      contents_view()
          ->search_result_tile_item_list_view_for_test()
          ->tile_views_for_test();
  for (int i = 0; i < kTileResults; ++i)
    forward_view_list.push_back(tile_views[i]);
  forward_view_list.push_back(contents_view()
                                  ->search_result_answer_card_view_for_test()
                                  ->GetAnswerCardResultViewForTest());
  SearchResultListView* list_view =
      contents_view()->search_result_list_view_for_test();
  for (int i = 0; i < kListResults; ++i)
    forward_view_list.push_back(list_view->GetResultViewAt(i));
  forward_view_list.push_back(search_box_view()->search_box());
  std::vector<views::View*> backward_view_list = forward_view_list;
  std::reverse(backward_view_list.begin(), backward_view_list.end());

  // Test traversal triggered by tab.
  TestFocusTraversal(forward_view_list, ui::VKEY_TAB, false);

  // Test traversal triggered by shift+tab.
  TestFocusTraversal(backward_view_list, ui::VKEY_TAB, true);
}

// Tests focus traversal in HALF state with opened search box using |VKEY_LEFT|
// and |VKEY_RIGHT|.
TEST_P(AppListViewFocusTest, LeftRightFocusTraversalInHalfState) {
  Show();

  // Type something in search box to transition to HALF state and populate
  // fake search results.
  // Type something in textfield.
  base::string16 text =
      is_rtl_
          // Arabic word of "test".
          ? base::UTF8ToUTF16(
                "\xd8\xa7\xd8\xae\xd8\xaa\xd8\xa8\xd8\xa7\xd8\xb1")
          : base::UTF8ToUTF16("test");
  search_box_view()->search_box()->InsertText(text);
  EXPECT_EQ(app_list_view()->app_list_state(),
            ash::mojom::AppListViewState::kHalf);

  constexpr int kTileResults = 6;
  SetUpSearchResults(kTileResults, 0, false);

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  const std::vector<SearchResultTileItemView*>& tile_views =
      contents_view()
          ->search_result_tile_item_list_view_for_test()
          ->tile_views_for_test();
  for (int i = 1; i < kTileResults; ++i)
    forward_view_list.push_back(tile_views[i]);
  forward_view_list.push_back(search_box_view()->search_box());

  TestFocusTraversal(forward_view_list,
                     is_rtl_ ? ui::VKEY_LEFT : ui::VKEY_RIGHT, false);

  std::vector<views::View*> backward_view_list = forward_view_list;

  // Backwards traversal won't skip any items, as the first view won't be
  // highlighted.
  backward_view_list.insert(backward_view_list.begin() + 1, tile_views[0]);

  // The intuitive focus is where the highlight is, on the first result.
  // Because of this, the 'x' is effectively behind us and should only be
  // traversed in the backwards list. The view in front of us it the second
  // result, so that is what we should jump to next.
  backward_view_list.insert(backward_view_list.begin() + 1,
                            search_box_view()->close_button());
  std::reverse(backward_view_list.begin(), backward_view_list.end());

  // The text in the box will be highlighted, the first press should deselect.
  backward_view_list.insert(backward_view_list.begin(),
                            search_box_view()->search_box());

  TestFocusTraversal(backward_view_list,
                     is_rtl_ ? ui::VKEY_RIGHT : ui::VKEY_LEFT, false);
}

// Tests the linear focus traversal in FULLSCREEN_ALL_APPS state within folder.
TEST_P(AppListViewFocusTest, LinearFocusTraversalInFolder) {
  Show();

  // Transition to FULLSCREEN_ALL_APPS state and open the folder.
  SetAppListState(ash::mojom::AppListViewState::kFullscreenAllApps);
  folder_item_view()->RequestFocus();
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_TRUE(contents_view()->GetAppsContainerView()->IsInFolderView());

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  const views::ViewModelT<AppListItemView>* view_model =
      app_list_folder_view()->items_grid_view()->view_model();
  for (int i = 0; i < view_model->view_size(); ++i)
    forward_view_list.push_back(view_model->view_at(i));
  forward_view_list.push_back(
      app_list_folder_view()->folder_header_view()->GetFolderNameViewForTest());
  forward_view_list.push_back(search_box_view()->search_box());
  std::vector<views::View*> backward_view_list = forward_view_list;
  std::reverse(backward_view_list.begin(), backward_view_list.end());

  // Test traversal triggered by tab.
  TestFocusTraversal(forward_view_list, ui::VKEY_TAB, false);

  // Test traversal triggered by shift+tab.
  TestFocusTraversal(backward_view_list, ui::VKEY_TAB, true);

  // Test traversal triggered by right.
  TestFocusTraversal(is_rtl_ ? backward_view_list : forward_view_list,
                     ui::VKEY_RIGHT, false);

  // Test traversal triggered by left.
  TestFocusTraversal(is_rtl_ ? forward_view_list : backward_view_list,
                     ui::VKEY_LEFT, false);
}

// Tests the vertical focus traversal by in PEEKING state.
TEST_P(AppListViewFocusTest, VerticalFocusTraversalInPeekingState) {
  Show();
  SetAppListState(ash::mojom::AppListViewState::kPeeking);

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  const std::vector<views::View*> suggestions = GetAllSuggestions();
  forward_view_list.push_back(suggestions[0]);
  forward_view_list.push_back(expand_arrow_view());
  forward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by down.
  TestFocusTraversal(forward_view_list, ui::VKEY_DOWN, false);

  std::vector<views::View*> backward_view_list;
  backward_view_list.push_back(search_box_view()->search_box());
  backward_view_list.push_back(expand_arrow_view());
  backward_view_list.push_back(suggestions.back());
  backward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by up.
  TestFocusTraversal(backward_view_list, ui::VKEY_UP, false);
}

// Tests the vertical focus traversal in FULLSCREEN_ALL_APPS state.
TEST_P(AppListViewFocusTest, VerticalFocusTraversalInFullscreenAllAppsState) {
  Show();
  SetAppListState(ash::mojom::AppListViewState::kFullscreenAllApps);

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  const std::vector<views::View*> suggestions = GetAllSuggestions();
  forward_view_list.push_back(suggestions[0]);
  const views::ViewModelT<AppListItemView>* view_model =
      apps_grid_view()->view_model();
  for (int i = 0; i < view_model->view_size(); i += apps_grid_view()->cols())
    forward_view_list.push_back(view_model->view_at(i));
  forward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by down.
  TestFocusTraversal(forward_view_list, ui::VKEY_DOWN, false);

  std::vector<views::View*> backward_view_list;
  backward_view_list.push_back(search_box_view()->search_box());
  for (int i = view_model->view_size() - 1; i >= 0;
       i -= apps_grid_view()->cols())
    backward_view_list.push_back(view_model->view_at(i));
  // Up key will always move focus to the last suggestion chip from first row
  // apps.
  const int index = suggestions.size() - 1;
  backward_view_list.push_back(suggestions[index]);
  backward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by up.
  TestFocusTraversal(backward_view_list, ui::VKEY_UP, false);
}

// Tests the vertical focus traversal in HALF state with opened search box.
TEST_F(AppListViewFocusTest, VerticalFocusTraversalInHalfState) {
  Show();

  // Type something in search box to transition to HALF state and populate
  // fake search results.
  search_box_view()->search_box()->InsertText(base::ASCIIToUTF16("test"));
  EXPECT_EQ(app_list_view()->app_list_state(),
            ash::mojom::AppListViewState::kHalf);
  constexpr int kTileResults = 3;
  constexpr int kListResults = 2;
  SetUpSearchResults(kTileResults, kListResults, true);

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  const std::vector<SearchResultTileItemView*>& tile_views =
      contents_view()
          ->search_result_tile_item_list_view_for_test()
          ->tile_views_for_test();
  // We skip the first view when coming from the search box. This is because
  // the first view is initially highlighted, and would already be activated
  // upon pressing enter. Hence, we skip adding the tile view to the expected
  // view list.
  forward_view_list.push_back(contents_view()
                                  ->search_result_answer_card_view_for_test()
                                  ->GetAnswerCardResultViewForTest());
  SearchResultListView* list_view =
      contents_view()->search_result_list_view_for_test();
  for (int i = 0; i < kListResults; ++i)
    forward_view_list.push_back(list_view->GetResultViewAt(i));
  forward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by down.
  TestFocusTraversal(forward_view_list, ui::VKEY_DOWN, false);

  std::vector<views::View*> backward_view_list;
  backward_view_list.push_back(search_box_view()->search_box());
  for (int i = kListResults - 1; i >= 0; --i)
    backward_view_list.push_back(list_view->GetResultViewAt(i));
  backward_view_list.push_back(contents_view()
                                   ->search_result_answer_card_view_for_test()
                                   ->GetAnswerCardResultViewForTest());
  backward_view_list.push_back(tile_views[kTileResults - 1]);
  backward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by up.
  TestFocusTraversal(backward_view_list, ui::VKEY_UP, false);
}

// Tests the vertical focus traversal in FULLSCREEN_ALL_APPS state in the first
// page within folder.
TEST_F(AppListViewFocusTest, VerticalFocusTraversalInFirstPageOfFolder) {
  Show();

  // Transition to FULLSCREEN_ALL_APPS state and open the folder.
  SetAppListState(ash::mojom::AppListViewState::kFullscreenAllApps);
  folder_item_view()->RequestFocus();
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_TRUE(contents_view()->GetAppsContainerView()->IsInFolderView());

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  const views::ViewModelT<AppListItemView>* view_model =
      app_list_folder_view()->items_grid_view()->view_model();
  for (size_t i = 0; i < AppListConfig::instance().max_folder_items_per_page();
       i += app_list_folder_view()->items_grid_view()->cols()) {
    forward_view_list.push_back(view_model->view_at(i));
  }
  forward_view_list.push_back(
      app_list_folder_view()->folder_header_view()->GetFolderNameViewForTest());
  forward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by down.
  TestFocusTraversal(forward_view_list, ui::VKEY_DOWN, false);

  std::vector<views::View*> backward_view_list;
  backward_view_list.push_back(search_box_view()->search_box());
  backward_view_list.push_back(
      app_list_folder_view()->folder_header_view()->GetFolderNameViewForTest());
  for (int i = AppListConfig::instance().max_folder_items_per_page() - 1;
       i >= 0; i -= app_list_folder_view()->items_grid_view()->cols()) {
    backward_view_list.push_back(view_model->view_at(i));
  }
  backward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by up.
  TestFocusTraversal(backward_view_list, ui::VKEY_UP, false);
}

// Tests the vertical focus traversal in FULLSCREEN_ALL_APPS state in the second
// page within folder.
TEST_F(AppListViewFocusTest, VerticalFocusTraversalInSecondPageOfFolder) {
  Show();

  // Transition to FULLSCREEN_ALL_APPS state and open the folder.
  SetAppListState(ash::mojom::AppListViewState::kFullscreenAllApps);
  folder_item_view()->RequestFocus();
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_TRUE(contents_view()->GetAppsContainerView()->IsInFolderView());

  // Select the second page.
  app_list_folder_view()->items_grid_view()->pagination_model()->SelectPage(
      1, false /* animate */);

  std::vector<views::View*> forward_view_list;
  forward_view_list.push_back(search_box_view()->search_box());
  const views::ViewModelT<AppListItemView>* view_model =
      app_list_folder_view()->items_grid_view()->view_model();
  for (int i = AppListConfig::instance().max_folder_items_per_page();
       i < view_model->view_size();
       i += app_list_folder_view()->items_grid_view()->cols()) {
    forward_view_list.push_back(view_model->view_at(i));
  }
  forward_view_list.push_back(
      app_list_folder_view()->folder_header_view()->GetFolderNameViewForTest());
  forward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by down.
  TestFocusTraversal(forward_view_list, ui::VKEY_DOWN, false);

  std::vector<views::View*> backward_view_list;
  backward_view_list.push_back(search_box_view()->search_box());
  backward_view_list.push_back(
      app_list_folder_view()->folder_header_view()->GetFolderNameViewForTest());
  for (size_t i = view_model->view_size() - 1;
       i >= AppListConfig::instance().max_folder_items_per_page();
       i -= app_list_folder_view()->items_grid_view()->cols()) {
    backward_view_list.push_back(view_model->view_at(i));
  }
  backward_view_list.push_back(search_box_view()->search_box());

  // Test traversal triggered by up.
  TestFocusTraversal(backward_view_list, ui::VKEY_UP, false);
}

// Tests that the focus is set back onto search box after state transition.
TEST_F(AppListViewFocusTest, FocusResetAfterStateTransition) {
  Show();

  // Type something in search box to transition to HALF state and populate
  // fake search results.
  search_box_view()->search_box()->InsertText(base::ASCIIToUTF16("test"));
  const int kTileResults = 3;
  const int kListResults = 2;
  SetUpSearchResults(kTileResults, kListResults, true);
  EXPECT_EQ(app_list_view()->app_list_state(),
            ash::mojom::AppListViewState::kHalf);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());

  // Move focus to the first search result, then transition to PEEKING state.
  SimulateKeyPress(ui::VKEY_TAB, false);
  SimulateKeyPress(ui::VKEY_TAB, false);
  SetAppListState(ash::mojom::AppListViewState::kPeeking);
  EXPECT_EQ(app_list_view()->app_list_state(),
            ash::mojom::AppListViewState::kPeeking);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());

  // Move focus to the first suggestion app, then transition to
  // FULLSCREEN_ALL_APPS state.
  SimulateKeyPress(ui::VKEY_TAB, false);
  SetAppListState(ash::mojom::AppListViewState::kFullscreenAllApps);
  EXPECT_EQ(app_list_view()->app_list_state(),
            ash::mojom::AppListViewState::kFullscreenAllApps);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());

  // Move focus to first suggestion app, then open the folder.
  folder_item_view()->RequestFocus();
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_TRUE(contents_view()->GetAppsContainerView()->IsInFolderView());
  EXPECT_EQ(search_box_view()->search_box(), focused_view());

  // Move focus to the first app, then transition to PEEKING state.
  SimulateKeyPress(ui::VKEY_TAB, false);
  SetAppListState(ash::mojom::AppListViewState::kPeeking);
  EXPECT_EQ(app_list_view()->app_list_state(),
            ash::mojom::AppListViewState::kPeeking);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
}

// Tests that key event which is not handled by focused view will be redirected
// to search box.
TEST_F(AppListViewFocusTest, RedirectFocusToSearchBox) {
  // UI behavior is different with Zero State enabled. This test is
  // the expected UI behavior with zero state feature being disabled.
  // TODO(jennyz): Add new test case for UI behavior for zero state.
  // crbug.com/925195.
  scoped_feature_list_.InitAndDisableFeature(
      app_list_features::kEnableZeroStateSuggestions);
  EXPECT_FALSE(app_list_features::IsZeroStateSuggestionsEnabled());

  Show();

  // Set focus to first suggestion app and type a character.
  GetAllSuggestions()[0]->RequestFocus();
  SimulateKeyPress(ui::VKEY_SPACE, false);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  EXPECT_EQ(search_box_view()->search_box()->text(), base::UTF8ToUTF16(" "));
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());

  // UI and Focus behavior is different with Zero State enabled.
  // Set focus to expand arrow and type a character.
  expand_arrow_view()->RequestFocus();
  SimulateKeyPress(ui::VKEY_A, false);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  EXPECT_EQ(search_box_view()->search_box()->text(), base::UTF8ToUTF16(" a"));
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());

  // Set focus to close button and type a character.
  search_box_view()->close_button()->RequestFocus();
  SimulateKeyPress(ui::VKEY_B, false);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  EXPECT_EQ(search_box_view()->search_box()->text(), base::UTF8ToUTF16(" ab"));
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());

  // Set focus to close button and hitting backspace.
  search_box_view()->close_button()->RequestFocus();
  SimulateKeyPress(ui::VKEY_BACK, false);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  EXPECT_EQ(search_box_view()->search_box()->text(), base::UTF8ToUTF16(" a"));
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());
}

// Tests that the search box textfield has no selection when the focus moves
// away from the SearchBoxView.
TEST_F(AppListViewFocusTest, SearchBoxTextfieldHasNoSelectionWhenFocusLeaves) {
  Show();

  search_box_view()->search_box()->InsertText(base::UTF8ToUTF16("test"));
  EXPECT_EQ(search_box_view()->search_box()->text(), base::UTF8ToUTF16("test"));

  // Move selection away from the searchbox.
  SimulateKeyPress(ui::VKEY_TAB, false);

  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());
}

// Tests that focus changes update the search box text.
TEST_F(AppListViewFocusTest, SearchBoxTextUpdatesOnResultFocus) {
  Show();
  views::Textfield* search_box = search_box_view()->search_box();
  search_box->InsertText(base::ASCIIToUTF16("TestText"));

  // Set up test results with unique titles
  ClearSearchResults();
  AddSearchResultWithTitleAndScore("TestResult1", 3);
  AddSearchResultWithTitleAndScore("TestResult2", 2);
  AddSearchResultWithTitleAndScore("TestResult3", 1);

  // Change focus to the first result
  SimulateKeyPress(ui::VKEY_TAB, false);
  SimulateKeyPress(ui::VKEY_TAB, false);

  EXPECT_EQ(search_box->text(), base::UTF8ToUTF16("TestResult1"));

  // Change focus to the next result
  SimulateKeyPress(ui::VKEY_TAB, false);

  EXPECT_EQ(search_box->text(), base::UTF8ToUTF16("TestResult2"));

  // Change focus to the final result
  SimulateKeyPress(ui::VKEY_TAB, false);

  EXPECT_EQ(search_box->text(), base::UTF8ToUTF16("TestResult3"));
}

// Tests that the search box selects the whole query when focus moves to the
// SearchBoxTextfield.
TEST_F(AppListViewFocusTest, SearchBoxSelectionCoversWholeQueryOnFocus) {
  Show();
  search_box_view()->search_box()->InsertText(base::ASCIIToUTF16("test"));
  EXPECT_EQ(app_list_view()->app_list_state(),
            ash::mojom::AppListViewState::kHalf);
  constexpr int kListResults = 1;
  SetUpSearchResults(0, kListResults, false);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  EXPECT_EQ(base::UTF8ToUTF16("test"), search_box_view()->search_box()->text());

  // Hit Tab to move focus away from the searchbox.
  SimulateKeyPress(ui::VKEY_TAB, false);
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());

  // Hit Shift+Tab to move focus back to the searchbox.
  SimulateKeyPress(ui::VKEY_TAB, true);
  EXPECT_EQ(gfx::Range(0, 4),
            search_box_view()->search_box()->GetSelectedRange());

  // Hit Shift+Tab to move focus away from the searchbox.
  SimulateKeyPress(ui::VKEY_TAB, true);
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());

  // Hit Tab to move focus back to the searchbox.
  SimulateKeyPress(ui::VKEY_TAB, false);
  EXPECT_EQ(gfx::Range(0, 4),
            search_box_view()->search_box()->GetSelectedRange());

  // Hit Up to move focus away from the searchbox.
  SimulateKeyPress(ui::VKEY_UP, false);
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());

  // Hit Down to move focus back to the searchbox.
  SimulateKeyPress(ui::VKEY_DOWN, false);
  EXPECT_EQ(gfx::Range(0, 4),
            search_box_view()->search_box()->GetSelectedRange());

  // Hit Down to move focus away from the searchbox.
  SimulateKeyPress(ui::VKEY_DOWN, false);
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());

  // Hit Up to move focus back to the searchbox.
  SimulateKeyPress(ui::VKEY_UP, false);
  EXPECT_EQ(gfx::Range(0, 4),
            search_box_view()->search_box()->GetSelectedRange());
}

// Tests that ctrl-A selects all text in the searchbox when the SearchBoxView is
// not focused.
TEST_F(AppListViewFocusTest, CtrlASelectsAllTextInSearchbox) {
  Show();
  search_box_view()->search_box()->InsertText(base::ASCIIToUTF16("test"));
  EXPECT_EQ(app_list_view()->app_list_state(),
            ash::mojom::AppListViewState::kHalf);
  constexpr int kTileResults = 3;
  constexpr int kListResults = 2;
  SetUpSearchResults(kTileResults, kListResults, false);

  // Move focus to the first search result.
  SimulateKeyPress(ui::VKEY_TAB, false);
  SimulateKeyPress(ui::VKEY_TAB, false);

  // Focus left the searchbox, so the selected range should be at the end of the
  // search text.
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());
  EXPECT_EQ(gfx::Range(4, 4),
            search_box_view()->search_box()->GetSelectedRange());

  // Press Ctrl-A, everything should be selected and the selected range should
  // include the whole text.
  SimulateKeyPress(ui::VKEY_A, false, true);
  EXPECT_TRUE(search_box_view()->search_box()->HasSelection());
  EXPECT_EQ(gfx::Range(0, 4),
            search_box_view()->search_box()->GetSelectedRange());

  // Advance focus, Focus should leave the searchbox, and the selected range
  // should be at the end of the search text.
  SimulateKeyPress(ui::VKEY_TAB, false);
  EXPECT_FALSE(search_box_view()->search_box()->HasSelection());
  EXPECT_EQ(gfx::Range(4, 4),
            search_box_view()->search_box()->GetSelectedRange());
}

// Tests that the first search result's view is selected after search results
// are updated when the focus is on search box.
TEST_F(AppListViewFocusTest, FirstResultSelectedAfterSearchResultsUpdated) {
  Show();

  // Type something in search box to transition to HALF state and populate
  // fake list results.
  search_box_view()->search_box()->InsertText(base::ASCIIToUTF16("test"));
  const int kListResults = 2;
  SetUpSearchResults(0, kListResults, false);
  SearchResultListView* list_view =
      contents_view()->search_result_list_view_for_test();
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  EXPECT_EQ(list_view->GetResultViewAt(0),
            contents_view()->search_results_page_view()->first_result_view());
  EXPECT_TRUE(list_view->GetResultViewAt(0)->background_highlighted());

  // Populate both fake list results and tile results.
  const int kTileResults = 3;
  SetUpSearchResults(kTileResults, kListResults, false);
  const std::vector<SearchResultTileItemView*>& tile_views =
      contents_view()
          ->search_result_tile_item_list_view_for_test()
          ->tile_views_for_test();
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  EXPECT_EQ(tile_views[0],
            contents_view()->search_results_page_view()->first_result_view());
  EXPECT_TRUE(tile_views[0]->background_highlighted());

  // Populate only answer card.
  SetUpSearchResults(0, 0, true);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  SearchResultBaseView* answer_container = static_cast<SearchResultBaseView*>(
      contents_view()
          ->search_result_answer_card_view_for_test()
          ->GetAnswerCardResultViewForTest());
  EXPECT_EQ(answer_container,
            contents_view()->search_results_page_view()->first_result_view());
  EXPECT_TRUE(answer_container->background_highlighted());

  // Moving focus to views other than search box textfield removes the first
  // result's highlight.
  SimulateKeyPress(ui::VKEY_TAB, false);
  EXPECT_EQ(search_box_view()->close_button(), focused_view());
  EXPECT_EQ(answer_container,
            contents_view()->search_results_page_view()->first_result_view());
  EXPECT_FALSE(answer_container->background_highlighted());
  SimulateKeyPress(ui::VKEY_TAB, true);

  // Clear up all search results.
  SetUpSearchResults(0, 0, false);
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  EXPECT_EQ(nullptr,
            contents_view()->search_results_page_view()->first_result_view());
}

// Tests that the first search result's view is not selected after search
// results are updated when the focus is on one of the search results (This
// happens when the user quickly hits Tab key after typing query and before
// search results are updated for the new query).
TEST_F(AppListViewFocusTest, FirstResultNotSelectedAfterQuicklyHittingTab) {
  Show();

  // Type something in search box to transition to HALF state and populate
  // fake list results.
  search_box_view()->search_box()->InsertText(base::ASCIIToUTF16("test1"));
  const int kListResults = 2;
  SetUpSearchResults(0, kListResults, false);
  SearchResultListView* list_view =
      contents_view()->search_result_list_view_for_test();
  SearchResultBaseView* first_result_view =
      contents_view()->search_results_page_view()->first_result_view();
  EXPECT_EQ(search_box_view()->search_box(), focused_view());
  EXPECT_EQ(list_view->GetResultViewAt(0), first_result_view);
  EXPECT_TRUE(first_result_view->background_highlighted());

  // Type something else.
  search_box_view()->search_box()->InsertText(base::ASCIIToUTF16("test2"));
  EXPECT_EQ(search_box_view()->search_box(), focused_view());

  // Simulate hitting Tab key to move focus to the close button, then to the
  // first result before search results are updated.
  SimulateKeyPress(ui::VKEY_TAB, false);
  EXPECT_EQ(search_box_view()->close_button(), focused_view());
  SimulateKeyPress(ui::VKEY_TAB, false);
  EXPECT_EQ(list_view->GetResultViewAt(0), focused_view());
  EXPECT_TRUE(first_result_view->background_highlighted());

  // Update search results, both list and tile results are populated.
  const int kTileResults = 3;
  SetUpSearchResults(kTileResults, kListResults, false);
  const std::vector<SearchResultTileItemView*>& tile_views =
      contents_view()
          ->search_result_tile_item_list_view_for_test()
          ->tile_views_for_test();
  first_result_view =
      contents_view()->search_results_page_view()->first_result_view();
  EXPECT_EQ(list_view->GetResultViewAt(0), focused_view());
  EXPECT_EQ(tile_views[0], first_result_view);
  EXPECT_FALSE(first_result_view->HasFocus());
  EXPECT_TRUE(list_view->GetResultViewAt(0)->background_highlighted());
}

// Tests hitting Enter key when focus is on search box.
// There are two behaviors:
// 1. Activate the search box when it is inactive.
// 2. Open the first result when query exists.
TEST_F(AppListViewFocusTest, HittingEnterWhenFocusOnSearchBox) {
  Show();

  // Initially the search box is inactive, hitting Enter to activate it.
  EXPECT_FALSE(search_box_view()->is_search_box_active());
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_TRUE(search_box_view()->is_search_box_active());

  // Type something in search box to transition to HALF state and populate
  // fake list results. Then hit Enter key.
  search_box_view()->search_box()->InsertText(base::UTF8ToUTF16("test"));
  const int kListResults = 2;
  SetUpSearchResults(0, kListResults, false);
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_EQ(1, GetOpenFirstSearchResultCount());
  EXPECT_EQ(1, GetTotalOpenSearchResultCount());

  // Populate both fake list results and tile results. Then hit Enter key.
  const int kTileResults = 3;
  SetUpSearchResults(kTileResults, kListResults, false);
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_EQ(2, GetOpenFirstSearchResultCount());
  EXPECT_EQ(2, GetTotalOpenSearchResultCount());

  // Populate only answer card. Then hit Enter key.
  SetUpSearchResults(0, 0, true);
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_EQ(3, GetOpenFirstSearchResultCount());
  EXPECT_EQ(3, GetTotalOpenSearchResultCount());

  // Clear up all search results. Then hit Enter key.
  SetUpSearchResults(0, 0, false);
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_EQ(3, GetOpenFirstSearchResultCount());
  EXPECT_EQ(3, GetTotalOpenSearchResultCount());
}

// Tests that search box becomes focused when it is activated.
TEST_F(AppListViewFocusTest, SetFocusOnSearchboxWhenActivated) {
  Show();

  // Set focus to the first suggestion app.
  GetAllSuggestions()[0]->RequestFocus();
  EXPECT_FALSE(search_box_view()->search_box()->HasFocus());

  // Activate the search box.
  search_box_view()->SetSearchBoxActive(true, ui::ET_MOUSE_PRESSED);
  EXPECT_TRUE(search_box_view()->search_box()->HasFocus());

  // Deactivate the search box won't move focus away.
  search_box_view()->SetSearchBoxActive(false, ui::ET_MOUSE_PRESSED);
  EXPECT_TRUE(search_box_view()->search_box()->HasFocus());
}

// Tests the left and right key when focus is on the textfield.
TEST_P(AppListViewFocusTest, HittingLeftRightWhenFocusOnTextfield) {
  Show();

  // Transition to FULLSCREEN_ALL_APPS state and open the folder.
  SetAppListState(ash::mojom::AppListViewState::kFullscreenAllApps);
  folder_item_view()->RequestFocus();
  SimulateKeyPress(ui::VKEY_RETURN, false);

  // Set focus on the folder name.
  views::Textfield* folder_name_view = static_cast<views::Textfield*>(
      app_list_folder_view()->folder_header_view()->GetFolderNameViewForTest());
  folder_name_view->RequestFocus();

  // Test folder name.
  TestLeftAndRightKeyOnTextfieldWithText(folder_name_view, false);
  TestLeftAndRightKeyOnTextfieldWithText(folder_name_view, true);

  // Set focus on the search box.
  search_box_view()->search_box()->RequestFocus();

  // Test search box. Active traversal has been tested at this point. This will
  // specifically test inactive traversal with no search results set up.
  TestLeftAndRightKeyTraversalOnTextfield(search_box_view()->search_box());
}

// Tests that the focus is reset onto the search box and the folder exits after
// hitting enter on folder name.
TEST_P(AppListViewFocusTest, FocusResetAfterHittingEnterOnFolderName) {
  Show();

  // Transition to FULLSCREEN_ALL_APPS state and open the folder.
  SetAppListState(ash::mojom::AppListViewState::kFullscreenAllApps);
  folder_item_view()->RequestFocus();
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_TRUE(contents_view()->GetAppsContainerView()->IsInFolderView());

  // Set focus on the folder name.
  views::View* folder_name_view =
      app_list_folder_view()->folder_header_view()->GetFolderNameViewForTest();
  folder_name_view->RequestFocus();

  // Hit enter key.
  SimulateKeyPress(ui::VKEY_RETURN, false);
  search_box_view()->search_box()->RequestFocus();
  EXPECT_FALSE(contents_view()->GetAppsContainerView()->IsInFolderView());
}

// Tests that opening the app list opens in peeking mode by default.
TEST_F(AppListViewTest, ShowPeekingByDefault) {
  Initialize(0, false, false);

  Show();

  ASSERT_EQ(ash::mojom::AppListViewState::kPeeking, view_->app_list_state());
}

// Tests that in side shelf mode, the app list opens in fullscreen by default
// and verifies that the top rounded corners of the app list background are
// hidden (see https://crbug.com/920082).
TEST_F(AppListViewTest, ShowFullscreenWhenInSideShelfMode) {
  Initialize(0, false, true);

  Show();

  EXPECT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());

  // Get the end point of the rounded corner and transform it into screen
  // coordinates. It should be on the screen's bottom line.
  gfx::PointF end_of_rounded_corner(0, view_->get_background_radius_for_test());
  view_->GetAppListBackgroundShieldForTest()->GetTransform().TransformPoint(
      &end_of_rounded_corner);
  EXPECT_EQ(0.0f, end_of_rounded_corner.y());
}

// Tests that in tablet mode, the app list opens in fullscreen by default.
TEST_F(AppListViewTest, ShowFullscreenWhenInTabletMode) {
  Initialize(0, true, false);

  Show();

  ASSERT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
}

// Tests that setting empty text in the search box does not change the state.
TEST_F(AppListViewTest, EmptySearchTextStillPeeking) {
  Initialize(0, false, false);
  views::Textfield* search_box =
      view_->app_list_main_view()->search_box_view()->search_box();

  Show();
  search_box->SetText(base::string16());

  ASSERT_EQ(ash::mojom::AppListViewState::kPeeking, view_->app_list_state());
}

TEST_F(AppListViewTest, MouseWheelScrollTransitionsToFullscreen) {
  base::HistogramTester histogram_tester;

  Initialize(0, false, false);
  delegate_->GetTestModel()->PopulateApps(kInitialItems);
  Show();

  view_->HandleScroll(gfx::Vector2d(0, -30), ui::ET_MOUSEWHEEL);
  EXPECT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
  // This should use animation instead of drag.
  // TODO(oshima): Test AnimationSmoothness.
  histogram_tester.ExpectTotalCount(
      "Apps.StateTransition.Drag.PresentationTime.ClamshellMode", 0);
}

TEST_F(AppListViewTest, GestureScrollTransitionsToFullscreen) {
  base::HistogramTester histogram_tester;
  Initialize(0, false, false);
  delegate_->GetTestModel()->PopulateApps(kInitialItems);
  Show();

  view_->HandleScroll(gfx::Vector2d(0, -30), ui::ET_SCROLL);
  EXPECT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
  // This should use animation instead of drag.
  // TODO(oshima): Test AnimationSmoothness.
  histogram_tester.ExpectTotalCount(
      "Apps.StateTransition.Drag.PresentationTime.ClamshellMode", 0);
}

// Tests that typing text after opening transitions from peeking to half.
TEST_F(AppListViewTest, TypingPeekingToHalf) {
  Initialize(0, false, false);
  views::Textfield* search_box =
      view_->app_list_main_view()->search_box_view()->search_box();

  Show();
  search_box->SetText(base::string16());
  search_box->InsertText(base::UTF8ToUTF16("nice"));

  ASSERT_EQ(ash::mojom::AppListViewState::kHalf, view_->app_list_state());
}

// Tests that typing when in fullscreen changes the state to fullscreen search.
TEST_F(AppListViewTest, TypingFullscreenToFullscreenSearch) {
  Initialize(0, false, false);
  view_->SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
  views::Textfield* search_box =
      view_->app_list_main_view()->search_box_view()->search_box();

  Show();
  search_box->SetText(base::string16());
  search_box->InsertText(base::UTF8ToUTF16("https://youtu.be/dQw4w9WgXcQ"));

  ASSERT_EQ(ash::mojom::AppListViewState::kFullscreenSearch,
            view_->app_list_state());
}

// Tests that in tablet mode, typing changes the state to fullscreen search.
TEST_F(AppListViewTest, TypingTabletModeFullscreenSearch) {
  Initialize(0, true, false);
  views::Textfield* search_box =
      view_->app_list_main_view()->search_box_view()->search_box();

  Show();
  search_box->SetText(base::string16());
  search_box->InsertText(base::UTF8ToUTF16("cool!"));

  ASSERT_EQ(ash::mojom::AppListViewState::kFullscreenSearch,
            view_->app_list_state());
}

// Tests that pressing escape when in peeking closes the app list.
TEST_F(AppListViewTest, EscapeKeyPeekingToClosed) {
  Initialize(0, false, false);

  Show();
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));

  ASSERT_EQ(ash::mojom::AppListViewState::kClosed, view_->app_list_state());
}

// Tests that pressing escape when in half screen changes the state to peeking.
TEST_F(AppListViewTest, EscapeKeyHalfToPeeking) {
  Initialize(0, false, false);
  views::Textfield* search_box =
      view_->app_list_main_view()->search_box_view()->search_box();

  Show();
  search_box->SetText(base::string16());
  search_box->InsertText(base::UTF8ToUTF16("doggie"));
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));

  ASSERT_EQ(ash::mojom::AppListViewState::kPeeking, view_->app_list_state());
}

// Tests that pressing escape when in fullscreen changes the state to closed.
TEST_F(AppListViewTest, EscapeKeyFullscreenToClosed) {
  Initialize(0, false, false);
  view_->SetState(ash::mojom::AppListViewState::kFullscreenAllApps);

  Show();
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));

  ASSERT_EQ(ash::mojom::AppListViewState::kClosed, view_->app_list_state());
}

// Tests that pressing escape when in fullscreen side-shelf closes the app list.
TEST_F(AppListViewTest, EscapeKeySideShelfFullscreenToClosed) {
  // Put into fullscreen by using side-shelf.
  Initialize(0, false, true);

  Show();
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));

  ASSERT_EQ(ash::mojom::AppListViewState::kClosed, view_->app_list_state());
}

// Tests that pressing escape when in tablet mode closes the app list.
TEST_F(AppListViewTest, EscapeKeyTabletModeStayFullscreen) {
  // Put into fullscreen by using tablet mode.
  Initialize(0, true, false);

  Show();
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));

  ASSERT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
}

// Tests that pressing escape when in fullscreen search changes to fullscreen.
TEST_F(AppListViewTest, EscapeKeyFullscreenSearchToFullscreen) {
  Initialize(0, false, false);
  view_->SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
  views::Textfield* search_box =
      view_->app_list_main_view()->search_box_view()->search_box();

  Show();
  search_box->SetText(base::string16());
  search_box->InsertText(base::UTF8ToUTF16("https://youtu.be/dQw4w9WgXcQ"));
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));

  ASSERT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
}

// Tests that pressing escape when in sideshelf search changes to fullscreen.
TEST_F(AppListViewTest, EscapeKeySideShelfSearchToFullscreen) {
  // Put into fullscreen using side-shelf.
  Initialize(0, false, true);
  views::Textfield* search_box =
      view_->app_list_main_view()->search_box_view()->search_box();

  Show();
  search_box->SetText(base::string16());
  search_box->InsertText(base::UTF8ToUTF16("kitty"));
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));

  ASSERT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
}

// Tests that in fullscreen, the app list has multiple pages with enough apps.
TEST_F(AppListViewTest, PopulateAppsCreatesAnotherPage) {
  Initialize(0, false, false);
  delegate_->GetTestModel()->PopulateApps(kInitialItems);

  Show();

  ASSERT_EQ(2, GetPaginationModel()->total_pages());
}

// Tests that even if initialize is called again with a different initial page,
// that for fullscreen we always select the first page.
TEST_F(AppListViewTest, MultiplePagesAlwaysReinitializeOnFirstPage) {
  Initialize(0, false, false);
  delegate_->GetTestModel()->PopulateApps(kInitialItems);

  // Show and close the widget once.
  Show();
  view_->GetWidget()->Close();
  // Set it up again with a nonzero initial page.
  view_ = new AppListView(delegate_.get());
  AppListView::InitParams params;
  params.parent = GetContext();
  params.initial_apps_page = 1;
  view_->Initialize(params);
  Show();

  ASSERT_EQ(0, view_->GetAppsPaginationModel()->selected_page());
}

// Tests that pressing escape when in tablet search changes to fullscreen.
TEST_F(AppListViewTest, EscapeKeyTabletModeSearchToFullscreen) {
  // Put into fullscreen using tablet mode.
  Initialize(0, true, false);
  views::Textfield* search_box =
      view_->app_list_main_view()->search_box_view()->search_box();

  Show();
  search_box->SetText(base::string16());
  search_box->InsertText(base::UTF8ToUTF16("yay"));
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));

  ASSERT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
}

// Tests that leaving tablet mode when in tablet search closes launcher.
TEST_F(AppListViewTest, LeaveTabletModeClosed) {
  // Put into fullscreen using tablet mode.
  Initialize(0, true, false);
  views::Textfield* search_box =
      view_->app_list_main_view()->search_box_view()->search_box();

  Show();
  search_box->SetText(base::string16());
  search_box->InsertText(base::UTF8ToUTF16("something"));
  view_->OnTabletModeChanged(false);

  ASSERT_EQ(ash::mojom::AppListViewState::kClosed, view_->app_list_state());
}

// Tests that opening in peeking mode sets the correct height.
TEST_P(AppListViewTest, OpenInPeekingCorrectHeight) {
  Initialize(0, false, false);

  Show();
  view_->SetState(ash::mojom::AppListViewState::kPeeking);
  ASSERT_EQ(AppListConfig::instance().peeking_app_list_height(),
            view_->GetCurrentAppListHeight());
}

// Tests that opening in peeking mode sets the correct height.
TEST_F(AppListViewTest, OpenInFullscreenCorrectHeight) {
  Initialize(0, false, false);

  Show();
  view_->SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
  const views::Widget* widget = view_->get_fullscreen_widget_for_test();
  const int y = widget->GetWindowBoundsInScreen().y();

  ASSERT_EQ(0, y);
}

// Tests that AppListView::SetState fails when the state has been set to CLOSED.
TEST_F(AppListViewTest, SetStateFailsWhenClosing) {
  Initialize(0, false, false);
  Show();
  view_->SetState(ash::mojom::AppListViewState::kClosed);

  view_->SetState(ash::mojom::AppListViewState::kFullscreenAllApps);

  ASSERT_EQ(ash::mojom::AppListViewState::kClosed, view_->app_list_state());
}

// Tests that going into a folder view, then setting the AppListState to PEEKING
// hides the folder view.
TEST_F(AppListViewTest, FolderViewToPeeking) {
  Initialize(0, false, false);
  AppListTestModel* model = delegate_->GetTestModel();
  model->PopulateApps(kInitialItems);
  const std::string folder_id =
      model->MergeItems(model->top_level_item_list()->item_at(0)->id(),
                        model->top_level_item_list()->item_at(1)->id());
  model->FindFolderItem(folder_id);
  Show();
  AppsGridViewTestApi test_api(view_->app_list_main_view()
                                   ->contents_view()
                                   ->GetAppsContainerView()
                                   ->apps_grid_view());
  test_api.PressItemAt(0);
  EXPECT_TRUE(view_->app_list_main_view()
                  ->contents_view()
                  ->GetAppsContainerView()
                  ->IsInFolderView());

  view_->SetState(ash::mojom::AppListViewState::kPeeking);

  EXPECT_FALSE(view_->app_list_main_view()
                   ->contents_view()
                   ->GetAppsContainerView()
                   ->IsInFolderView());
}

// Tests that a tap or click in an empty region of the AppsGridView closes the
// AppList.
TEST_F(AppListViewTest, TapAndClickWithinAppsGridView) {
  Initialize(0, false, false);
  // Populate the AppList with a small number of apps so there is an empty
  // region to click.
  delegate_->GetTestModel()->PopulateApps(6);
  Show();
  view_->SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
  EXPECT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
  AppsGridView* apps_grid_view = view_->app_list_main_view()
                                     ->contents_view()
                                     ->GetAppsContainerView()
                                     ->apps_grid_view();
  AppsGridViewTestApi test_api(apps_grid_view);

  // Get the point of the first empty region (where app #7 would be) and tap on
  // it, the AppList should close.
  const gfx::Point empty_region =
      test_api.GetItemTileRectOnCurrentPageAt(2, 2).CenterPoint();
  ui::GestureEvent tap(empty_region.x(), empty_region.y(), 0, base::TimeTicks(),
                       ui::GestureEventDetails(ui::ET_GESTURE_TAP));
  ui::Event::DispatcherApi tap_dispatcher_api(static_cast<ui::Event*>(&tap));
  tap_dispatcher_api.set_target(view_);
  view_->OnGestureEvent(&tap);
  EXPECT_EQ(ash::mojom::AppListViewState::kClosed, view_->app_list_state());

  Show();

  // Click on the same empty region, the AppList should close again.
  ui::MouseEvent mouse_click(ui::ET_MOUSE_PRESSED, empty_region, empty_region,
                             base::TimeTicks(), 0, 0);
  ui::Event::DispatcherApi mouse_click_dispatcher_api(
      static_cast<ui::Event*>(&mouse_click));
  mouse_click_dispatcher_api.set_target(view_);
  view_->OnMouseEvent(&mouse_click);
  EXPECT_EQ(ash::mojom::AppListViewState::kClosed, view_->app_list_state());
}

// Tests that search box should not become a rectangle during drag.
TEST_F(AppListViewTest, SearchBoxCornerRadiusDuringDragging) {
  base::HistogramTester histogram_tester;
  Initialize(0, false, false);
  delegate_->GetTestModel()->PopulateApps(kInitialItems);
  Show();
  view_->SetState(ash::mojom::AppListViewState::kFullscreenAllApps);
  EXPECT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
  histogram_tester.ExpectTotalCount(
      "Apps.StateTransition.Drag.PresentationTime.ClamshellMode", 0);

  // Send SCROLL_START and SCROLL_UPDATE events, simulating dragging the
  // launcher.
  base::TimeTicks timestamp = base::TimeTicks::Now();
  gfx::Point start = view_->get_fullscreen_widget_for_test()
                         ->GetWindowBoundsInScreen()
                         .top_right();
  int delta_y = 1;
  ui::GestureEvent start_event = ui::GestureEvent(
      start.x(), start.y(), ui::EF_NONE, timestamp,
      ui::GestureEventDetails(ui::ET_GESTURE_SCROLL_BEGIN, 0, delta_y));
  view_->OnGestureEvent(&start_event);
  histogram_tester.ExpectTotalCount(
      "Apps.StateTransition.Drag.PresentationTime.ClamshellMode", 0);

  // Drag down the launcher.
  timestamp += base::TimeDelta::FromMilliseconds(25);
  delta_y += 10;
  start.Offset(0, 1);
  ui::GestureEvent update_event = ui::GestureEvent(
      start.x(), start.y(), ui::EF_NONE, timestamp,
      ui::GestureEventDetails(ui::ET_GESTURE_SCROLL_UPDATE, 0, delta_y));
  view_->OnGestureEvent(&update_event);

  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateApps));
  EXPECT_EQ(search_box::kSearchBoxBorderCornerRadius,
            search_box_view()->GetSearchBoxBorderCornerRadiusForState(
                ash::AppListState::kStateApps));

  // Search box should keep |kSearchBoxCornerRadiusFullscreen| corner radius
  // during drag.
  EXPECT_TRUE(SetAppListState(ash::AppListState::kStateSearchResults));
  EXPECT_TRUE(view_->is_in_drag());
  EXPECT_EQ(search_box::kSearchBoxBorderCornerRadius,
            search_box_view()->GetSearchBoxBorderCornerRadiusForState(
                ash::AppListState::kStateSearchResults));
  histogram_tester.ExpectTotalCount(
      "Apps.StateTransition.Drag.PresentationTime.ClamshellMode", 1);
  histogram_tester.ExpectTotalCount(
      "Apps.StateTransition.Drag.PresentationTime.MaxLatency.ClamshellMode", 0);

  // Ends to drag the launcher.
  EXPECT_TRUE(SetAppListState(ash::AppListState::kStateApps));
  timestamp += base::TimeDelta::FromMilliseconds(25);
  start.Offset(0, 1);
  ui::GestureEvent end_event =
      ui::GestureEvent(start.x(), start.y() + delta_y, ui::EF_NONE, timestamp,
                       ui::GestureEventDetails(ui::ET_GESTURE_END));
  view_->OnGestureEvent(&end_event);

  // Search box should keep |kSearchBoxCornerRadiusFullscreen| corner radius
  // if launcher drag finished.
  EXPECT_FALSE(view_->is_in_drag());
  EXPECT_EQ(search_box::kSearchBoxBorderCornerRadius,
            search_box_view()->GetSearchBoxBorderCornerRadiusForState(
                ash::AppListState::kStateApps));
  histogram_tester.ExpectTotalCount(
      "Apps.StateTransition.Drag.PresentationTime.ClamshellMode", 1);
  histogram_tester.ExpectTotalCount(
      "Apps.StateTransition.Drag.PresentationTime.MaxLatency.ClamshellMode", 1);
}

// Tests displaying the app list and performs a standard set of checks on its
// top level views. Then closes the window.
TEST_F(AppListViewTest, DisplayTest) {
  Initialize(0, false, false);
  EXPECT_EQ(-1, GetPaginationModel()->total_pages());
  delegate_->GetTestModel()->PopulateApps(kInitialItems);

  Show();

  // |view_| bounds equal to the root window's size.
  EXPECT_EQ("800x600", view_->bounds().size().ToString());

  EXPECT_EQ(2, GetPaginationModel()->total_pages());
  EXPECT_EQ(0, GetPaginationModel()->selected_page());

  // Checks on the main view.
  AppListMainView* main_view = view_->app_list_main_view();
  EXPECT_NO_FATAL_FAILURE(CheckView(main_view));
  EXPECT_NO_FATAL_FAILURE(CheckView(main_view->contents_view()));

  ash::AppListState expected = ash::AppListState::kStateApps;
  EXPECT_TRUE(main_view->contents_view()->IsStateActive(expected));
  EXPECT_EQ(expected, delegate_->GetModel()->state());
}

// Tests switching rapidly between multiple pages of the launcher.
TEST_F(AppListViewTest, PageSwitchingAnimationTest) {
  Initialize(0, false, false);
  AppListMainView* main_view = view_->app_list_main_view();
  // Checks on the main view.
  EXPECT_NO_FATAL_FAILURE(CheckView(main_view));
  EXPECT_NO_FATAL_FAILURE(CheckView(main_view->contents_view()));

  ContentsView* contents_view = main_view->contents_view();

  contents_view->SetActiveState(ash::AppListState::kStateApps);
  contents_view->Layout();

  IsStateShown(ash::AppListState::kStateApps);

  // Change pages. View should not have moved without Layout().
  contents_view->ShowSearchResults(true);
  IsStateShown(ash::AppListState::kStateApps);

  // Change to a third page. This queues up the second animation behind the
  // first.
  contents_view->SetActiveState(ash::AppListState::kStateApps);
  IsStateShown(ash::AppListState::kStateApps);

  // Call Layout(). Should jump to the third page.
  contents_view->Layout();
  IsStateShown(ash::AppListState::kStateApps);
}

// Tests that the correct views are displayed for showing search results.
TEST_F(AppListViewTest, DISABLED_SearchResultsTest) {
  Initialize(0, false, false);
  // TODO(newcomer): this test needs to be reevaluated for the fullscreen app
  // list (http://crbug.com/759779).
  EXPECT_FALSE(view_->GetWidget()->IsVisible());
  EXPECT_EQ(-1, GetPaginationModel()->total_pages());
  AppListTestModel* model = delegate_->GetTestModel();
  model->PopulateApps(3);

  Show();

  AppListMainView* main_view = view_->app_list_main_view();
  ContentsView* contents_view = main_view->contents_view();
  EXPECT_TRUE(SetAppListState(ash::AppListState::kStateApps));

  // Show the search results.
  contents_view->ShowSearchResults(true);
  contents_view->Layout();
  EXPECT_TRUE(
      contents_view->IsStateActive(ash::AppListState::kStateSearchResults));

  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateSearchResults));

  // Hide the search results.
  contents_view->ShowSearchResults(false);
  contents_view->Layout();

  // Check that we return to the page that we were on before the search.
  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateApps));

  view_->Layout();
  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateApps));

  base::string16 search_text = base::UTF8ToUTF16("test");
  main_view->search_box_view()->search_box()->SetText(base::string16());
  main_view->search_box_view()->search_box()->InsertText(search_text);
  // Check that the current search is using |search_text|.
  EXPECT_EQ(search_text, delegate_->GetSearchModel()->search_box()->text());
  EXPECT_EQ(search_text, main_view->search_box_view()->search_box()->text());
  contents_view->Layout();
  EXPECT_TRUE(
      contents_view->IsStateActive(ash::AppListState::kStateSearchResults));
  EXPECT_TRUE(CheckSearchBoxWidget(contents_view->GetDefaultSearchBoxBounds()));

  // Check that typing into the search box triggers the search page.
  EXPECT_TRUE(SetAppListState(ash::AppListState::kStateApps));
  contents_view->Layout();
  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateApps));
  EXPECT_TRUE(CheckSearchBoxWidget(contents_view->GetDefaultSearchBoxBounds()));

  base::string16 new_search_text = base::UTF8ToUTF16("apple");
  main_view->search_box_view()->search_box()->SetText(base::string16());
  main_view->search_box_view()->search_box()->InsertText(new_search_text);
  // Check that the current search is using |new_search_text|.
  EXPECT_EQ(new_search_text, delegate_->GetSearchModel()->search_box()->text());
  EXPECT_EQ(new_search_text,
            main_view->search_box_view()->search_box()->text());
  contents_view->Layout();
  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateSearchResults));
  EXPECT_TRUE(CheckSearchBoxWidget(contents_view->GetDefaultSearchBoxBounds()));
}

// Tests that the back button navigates through the app list correctly.
TEST_F(AppListViewTest, DISABLED_BackTest) {
  Initialize(0, false, false);
  // TODO(newcomer): this test needs to be reevaluated for the fullscreen app
  // list (http://crbug.com/759779).
  EXPECT_FALSE(view_->GetWidget()->IsVisible());
  EXPECT_EQ(-1, GetPaginationModel()->total_pages());

  Show();

  AppListMainView* main_view = view_->app_list_main_view();
  ContentsView* contents_view = main_view->contents_view();
  SearchBoxView* search_box_view = main_view->search_box_view();

  // Show the apps grid.
  SetAppListState(ash::AppListState::kStateApps);
  EXPECT_NO_FATAL_FAILURE(CheckView(search_box_view->back_button()));

  // The back button should return to the apps page.
  EXPECT_TRUE(contents_view->Back());
  contents_view->Layout();
  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateApps));
  EXPECT_FALSE(search_box_view->back_button()->visible());

  // Show the apps grid again.
  SetAppListState(ash::AppListState::kStateApps);
  EXPECT_NO_FATAL_FAILURE(CheckView(search_box_view->back_button()));

  // Pressing ESC should return to the apps page.
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));
  contents_view->Layout();
  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateApps));
  EXPECT_FALSE(search_box_view->back_button()->visible());

  // Pressing ESC from the start page should close the app list.
  EXPECT_EQ(0, delegate_->dismiss_count());
  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));
  EXPECT_EQ(1, delegate_->dismiss_count());

  // Show the search results.
  base::string16 new_search_text = base::UTF8ToUTF16("apple");
  search_box_view->search_box()->SetText(base::string16());
  search_box_view->search_box()->InsertText(new_search_text);
  contents_view->Layout();
  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateSearchResults));
  EXPECT_NO_FATAL_FAILURE(CheckView(search_box_view->back_button()));

  // The back button should return to the start page.
  EXPECT_TRUE(contents_view->Back());
  contents_view->Layout();
  EXPECT_TRUE(IsStateShown(ash::AppListState::kStateApps));
  EXPECT_FALSE(search_box_view->back_button()->visible());
}

// Tests that even if initialize is called again with a different initial page,
// that different initial page is respected.
TEST_F(AppListViewTest, DISABLED_MultiplePagesReinitializeOnInputPage) {
  Initialize(0, false, false);
  // TODO(newcomer): this test needs to be reevaluated for the fullscreen app
  // list (http://crbug.com/759779).
  delegate_->GetTestModel()->PopulateApps(kInitialItems);

  // Show and close the widget once.
  Show();
  view_->GetWidget()->Close();
  // Set it up again with a nonzero initial page.
  view_ = new AppListView(delegate_.get());
  AppListView::InitParams params;
  params.parent = GetContext();
  params.initial_apps_page = 1;
  view_->Initialize(params);
  Show();

  ASSERT_EQ(1, view_->GetAppsPaginationModel()->selected_page());
}

// Tests that a context menu can be shown between app icons in tablet mode.
TEST_F(AppListViewTest, ShowContextMenuBetweenAppsInTabletMode) {
  Initialize(0, true /* enable tablet mode */, false);
  delegate_->GetTestModel()->PopulateApps(kInitialItems);
  Show();

  // Tap between two apps in tablet mode.
  const gfx::Point middle = GetPointBetweenTwoApps();
  ui::GestureEvent tap(middle.x(), middle.y(), 0, base::TimeTicks(),
                       ui::GestureEventDetails(ui::ET_GESTURE_TWO_FINGER_TAP));
  view_->OnGestureEvent(&tap);

  // The wallpaper context menu should show.
  EXPECT_EQ(1, show_wallpaper_context_menu_count());
  EXPECT_TRUE(view_->GetWidget()->IsVisible());

  // Click between two apps in tablet mode.
  ui::MouseEvent mouse_event(ui::ET_MOUSE_PRESSED, middle, middle,
                             ui::EventTimeForNow(), ui::EF_RIGHT_MOUSE_BUTTON,
                             ui::EF_RIGHT_MOUSE_BUTTON);
  view_->OnMouseEvent(&mouse_event);

  // The wallpaper context menu should show.
  EXPECT_EQ(2, show_wallpaper_context_menu_count());
  EXPECT_TRUE(view_->GetWidget()->IsVisible());
}

// Tests that context menus are not shown between app icons in clamshell mode.
TEST_F(AppListViewTest, DontShowContextMenuBetweenAppsInClamshellMode) {
  Initialize(0, false /* disable tablet mode */, false);
  delegate_->GetTestModel()->PopulateApps(kInitialItems);
  Show();

  // Tap between two apps in clamshell mode.
  const gfx::Point middle = GetPointBetweenTwoApps();
  ui::GestureEvent tap(middle.x(), middle.y(), 0, base::TimeTicks(),
                       ui::GestureEventDetails(ui::ET_GESTURE_TWO_FINGER_TAP));
  view_->OnGestureEvent(&tap);

  // The wallpaper menu should not show.
  EXPECT_EQ(0, show_wallpaper_context_menu_count());
  EXPECT_TRUE(view_->GetWidget()->IsVisible());

  // Right click between two apps in clamshell mode.
  ui::MouseEvent mouse_event(ui::ET_MOUSE_PRESSED, middle, middle,
                             ui::EventTimeForNow(), ui::EF_RIGHT_MOUSE_BUTTON,
                             ui::EF_RIGHT_MOUSE_BUTTON);
  view_->OnMouseEvent(&mouse_event);

  // The wallpaper menu should not show.
  EXPECT_EQ(0, show_wallpaper_context_menu_count());
  EXPECT_TRUE(view_->GetWidget()->IsVisible());
}

// Tests the back action in home launcher.
TEST_F(AppListViewTest, BackAction) {
  // Put into fullscreen using tablet mode.
  Initialize(0, true, false);

  // Populate apps to fill up the first page and add a folder in the second
  // page.
  AppListTestModel* model = delegate_->GetTestModel();
  const int kAppListItemNum = test_api_->TilesPerPage(0);
  const int kItemNumInFolder = 5;
  model->PopulateApps(kAppListItemNum);
  model->CreateAndPopulateFolderWithApps(kItemNumInFolder);

  // Show the app list
  Show();
  EXPECT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
  EXPECT_EQ(2, apps_grid_view()->pagination_model()->total_pages());

  // Select the second page and open the folder.
  apps_grid_view()->pagination_model()->SelectPage(1, false);
  test_api_->PressItemAt(kAppListItemNum);
  EXPECT_TRUE(contents_view()->GetAppsContainerView()->IsInFolderView());
  EXPECT_EQ(1, apps_grid_view()->pagination_model()->selected_page());

  // Back action will first close the folder.
  contents_view()->Back();
  EXPECT_FALSE(contents_view()->GetAppsContainerView()->IsInFolderView());
  EXPECT_EQ(1, apps_grid_view()->pagination_model()->selected_page());

  // Back action will then select the first page.
  contents_view()->Back();
  EXPECT_FALSE(contents_view()->GetAppsContainerView()->IsInFolderView());
  EXPECT_EQ(0, apps_grid_view()->pagination_model()->selected_page());

  // Select the second page and open search results page.
  apps_grid_view()->pagination_model()->SelectPage(1, false);
  search_box_view()->search_box()->InsertText(base::UTF8ToUTF16("A"));
  EXPECT_EQ(ash::mojom::AppListViewState::kFullscreenSearch,
            view_->app_list_state());
  EXPECT_EQ(1, apps_grid_view()->pagination_model()->selected_page());

  // Back action will first close the search results page.
  contents_view()->Back();
  EXPECT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
  EXPECT_EQ(1, apps_grid_view()->pagination_model()->selected_page());

  // Back action will then select the first page.
  contents_view()->Back();
  EXPECT_EQ(ash::mojom::AppListViewState::kFullscreenAllApps,
            view_->app_list_state());
  EXPECT_EQ(0, apps_grid_view()->pagination_model()->selected_page());
}

// Tests selecting search result to show embedded Assistant UI.
TEST_F(AppListViewFocusTest, ShowEmbeddedAssistantUI) {
  scoped_feature_list_.InitWithFeatures(
      {chromeos::switches::kAssistantFeature,
       app_list_features::kEnableEmbeddedAssistantUI},
      {});
  Show();

  // Initially the search box is inactive, hitting Enter to activate it.
  EXPECT_FALSE(search_box_view()->is_search_box_active());
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_TRUE(search_box_view()->is_search_box_active());

  // Type something in search box to transition to HALF state and populate
  // fake list results. Then hit Enter key.
  search_box_view()->search_box()->InsertText(base::UTF8ToUTF16("test"));
  const int kListResults = 2;
  const int kIndexOpenAssistantUi = 1;
  SetUpSearchResultsForAssistantUI(kListResults, kIndexOpenAssistantUi);
  SimulateKeyPress(ui::VKEY_RETURN, false);
  EXPECT_EQ(1, GetOpenFirstSearchResultCount());
  EXPECT_EQ(1, GetTotalOpenSearchResultCount());
  EXPECT_EQ(0, GetTotalOpenAssistantUICount());

  SearchResultListView* list_view =
      contents_view()->search_result_list_view_for_test();
  ui::KeyEvent key_event(ui::ET_KEY_PRESSED, ui::VKEY_RETURN, ui::EF_NONE);
  list_view->GetResultViewAt(kIndexOpenAssistantUi)->OnKeyEvent(&key_event);
  EXPECT_EQ(1, GetOpenFirstSearchResultCount());
  EXPECT_EQ(2, GetTotalOpenSearchResultCount());
  EXPECT_EQ(1, GetTotalOpenAssistantUICount());
}

// Tests that no answer card view when kEnableEmbeddedAssistantUI is enabled.
TEST_F(AppListViewTest, NoAnswerCardWhenEmbeddedAssistantUIEnabled) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatures(
      {chromeos::switches::kAssistantFeature,
       app_list_features::kEnableEmbeddedAssistantUI},
      {});
  ASSERT_TRUE(app_list_features::IsEmbeddedAssistantUIEnabled());

  Initialize(0, false, false);
  Show();

  EXPECT_FALSE(contents_view()->search_result_answer_card_view_for_test());
}

// Tests that pressing escape when in embedded Assistant UI to search page view.
TEST_F(AppListViewTest, EscapeKeyEmbeddedAssistantUIToSearch) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatures(
      {chromeos::switches::kAssistantFeature,
       app_list_features::kEnableEmbeddedAssistantUI},
      {});
  ASSERT_TRUE(app_list_features::IsEmbeddedAssistantUIEnabled());

  Initialize(0, false, false);
  Show();

  // Set search_box_view active.
  ui::KeyEvent key_event(ui::ET_KEY_PRESSED, ui::VKEY_RETURN, ui::EF_NONE);
  view_->GetWidget()->OnKeyEvent(&key_event);

  contents_view()->ShowEmbeddedAssistantUI(true);
  EXPECT_TRUE(contents_view()->IsShowingEmbeddedAssistantUI());

  view_->AcceleratorPressed(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));
  EXPECT_TRUE(contents_view()->IsShowingSearchResults());
}

// Tests that clicking empty region in AppListview when showing Assistant UI
// should go back to peeking state.
TEST_F(AppListViewTest, ClickOutsideEmbeddedAssistantUIToPeeking) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatures(
      {chromeos::switches::kAssistantFeature,
       app_list_features::kEnableEmbeddedAssistantUI},
      {});
  ASSERT_TRUE(app_list_features::IsEmbeddedAssistantUIEnabled());

  Initialize(0, false, false);
  Show();

  // Set search_box_view active.
  ui::KeyEvent key_event(ui::ET_KEY_PRESSED, ui::VKEY_RETURN, ui::EF_NONE);
  view_->GetWidget()->OnKeyEvent(&key_event);

  contents_view()->ShowEmbeddedAssistantUI(true);
  EXPECT_TRUE(contents_view()->IsShowingEmbeddedAssistantUI());

  // Click on the same empty region, the AppList should be peeking state.
  const gfx::Point empty_region = view_->GetBoundsInScreen().origin();
  ui::MouseEvent mouse_click(ui::ET_MOUSE_PRESSED, empty_region, empty_region,
                             base::TimeTicks(), 0, 0);
  ui::Event::DispatcherApi mouse_click_dispatcher_api(
      static_cast<ui::Event*>(&mouse_click));
  mouse_click_dispatcher_api.set_target(view_);
  view_->OnMouseEvent(&mouse_click);
  EXPECT_EQ(ash::mojom::AppListViewState::kPeeking, view_->app_list_state());
}

// Tests that expand arrow is not visible when showing embedded Assistant UI.
TEST_F(AppListViewTest, ExpandArrowNotVisibleInEmbeddedAssistantUI) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatures(
      {chromeos::switches::kAssistantFeature,
       app_list_features::kEnableEmbeddedAssistantUI},
      {});
  ASSERT_TRUE(app_list_features::IsEmbeddedAssistantUIEnabled());

  Initialize(0, false, false);
  Show();

  // Set search_box_view active.
  ui::KeyEvent key_event(ui::ET_KEY_PRESSED, ui::VKEY_RETURN, ui::EF_NONE);
  view_->GetWidget()->OnKeyEvent(&key_event);

  contents_view()->ShowEmbeddedAssistantUI(true);
  EXPECT_TRUE(contents_view()->IsShowingEmbeddedAssistantUI());
  EXPECT_TRUE(contents_view()->expand_arrow_view()->layer()->opacity() == 0.0f);
}

}  // namespace test
}  // namespace app_list
