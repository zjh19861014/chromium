// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill_assistant;

import android.content.Context;
import android.graphics.Color;
import android.support.annotation.Nullable;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;

import org.chromium.base.ApiCompatibilityUtils;
import org.chromium.base.Callback;
import org.chromium.base.VisibleForTesting;
import org.chromium.chrome.autofill_assistant.R;
import org.chromium.chrome.browser.autofill_assistant.carousel.AssistantCarouselCoordinator;
import org.chromium.chrome.browser.autofill_assistant.carousel.AssistantChip;
import org.chromium.chrome.browser.autofill_assistant.details.AssistantDetailsCoordinator;
import org.chromium.chrome.browser.autofill_assistant.header.AssistantHeaderCoordinator;
import org.chromium.chrome.browser.autofill_assistant.header.AssistantHeaderModel;
import org.chromium.chrome.browser.autofill_assistant.infobox.AssistantInfoBoxCoordinator;
import org.chromium.chrome.browser.autofill_assistant.overlay.AssistantOverlayModel;
import org.chromium.chrome.browser.autofill_assistant.overlay.AssistantOverlayState;
import org.chromium.chrome.browser.autofill_assistant.payment.AssistantPaymentRequestCoordinator;
import org.chromium.chrome.browser.widget.bottomsheet.BottomSheet;
import org.chromium.chrome.browser.widget.bottomsheet.BottomSheetController;
import org.chromium.chrome.browser.widget.bottomsheet.EmptyBottomSheetObserver;
import org.chromium.ui.modelutil.ListModel;

/**
 * Coordinator responsible for the Autofill Assistant bottom bar.
 */
class AssistantBottomBarCoordinator {
    private final AssistantModel mModel;
    private final BottomSheetController mBottomSheetController;
    private final AssistantBottomSheetContent mContent;

    // Child coordinators.
    private AssistantInfoBoxCoordinator mInfoBoxCoordinator;
    private final AssistantHeaderCoordinator mHeaderCoordinator;
    private final AssistantDetailsCoordinator mDetailsCoordinator;
    private final AssistantPaymentRequestCoordinator mPaymentRequestCoordinator;
    private final AssistantCarouselCoordinator mSuggestionsCoordinator;
    private final AssistantCarouselCoordinator mActionsCoordinator;

    @Nullable
    private ScrollView mOnboardingScrollView;

    AssistantBottomBarCoordinator(
            Context context, AssistantModel model, BottomSheetController controller) {
        mModel = model;
        mBottomSheetController = controller;
        mContent = new AssistantBottomSheetContent(context);

        // Instantiate child components.
        mHeaderCoordinator = new AssistantHeaderCoordinator(
                context, mContent.mBottomBarView, model.getHeaderModel());
        mInfoBoxCoordinator = new AssistantInfoBoxCoordinator(context, model.getInfoBoxModel());
        mDetailsCoordinator = new AssistantDetailsCoordinator(context, model.getDetailsModel());
        mPaymentRequestCoordinator =
                new AssistantPaymentRequestCoordinator(context, model.getPaymentRequestModel());
        mSuggestionsCoordinator =
                new AssistantCarouselCoordinator(context, model.getSuggestionsModel());
        mActionsCoordinator = new AssistantCarouselCoordinator(context, model.getActionsModel());

        // Add child views to bottom bar container.
        mContent.mBottomBarView.addView(mInfoBoxCoordinator.getView());
        mContent.mBottomBarView.addView(mDetailsCoordinator.getView());
        mContent.mBottomBarView.addView(mPaymentRequestCoordinator.getView());
        mContent.mBottomBarView.addView(mSuggestionsCoordinator.getView());
        mContent.mBottomBarView.addView(mActionsCoordinator.getView());

        // Set children top margins to have a spacing between them. For the carousels, we set their
        // margin only when they are not empty given that they are always shown, even if empty. We
        // do not hide them because there is an incompatibility bug between the animateLayoutChanges
        // attribute set on mBottomBarView and the animations ran by the carousels
        // RecyclerView.
        int childSpacing = context.getResources().getDimensionPixelSize(
                R.dimen.autofill_assistant_bottombar_vertical_spacing);
        setChildMarginTop(mDetailsCoordinator.getView(), childSpacing);
        setChildMarginTop(mPaymentRequestCoordinator.getView(), childSpacing);
        setCarouselMarginTop(mSuggestionsCoordinator.getView(),
                model.getSuggestionsModel().getChipsModel(), childSpacing);
        setCarouselMarginTop(mActionsCoordinator.getView(), model.getActionsModel().getChipsModel(),
                childSpacing);

        // We set the horizontal margins of the details and payment request. We don't set a padding
        // to the container as we want the carousels children to be scrolled at the limit of the
        // screen.
        setHorizontalMargins(mInfoBoxCoordinator.getView());
        setHorizontalMargins(mDetailsCoordinator.getView());
        setHorizontalMargins(mPaymentRequestCoordinator.getView());

        // Set the toolbar background color to white only in the PEEK state, to make sure it does
        // not hide parts of the content view (which it overlaps).
        controller.getBottomSheet().addObserver(new EmptyBottomSheetObserver() {
            @Override
            public void onSheetOpened(int reason) {
                mContent.mToolbarView.setBackgroundColor(Color.TRANSPARENT);
            }

            @Override
            public void onSheetClosed(int reason) {
                mContent.mToolbarView.setBackgroundColor(ApiCompatibilityUtils.getColor(
                        context.getResources(), R.color.modern_primary_color));
            }
        });

        // Show or hide the bottom sheet content when the Autofill Assistant visibility is changed.
        model.addObserver((source, propertyKey) -> {
            if (AssistantModel.VISIBLE == propertyKey) {
                if (model.get(AssistantModel.VISIBLE)) {
                    showAndExpand();
                } else {
                    hide();
                }
            }
        });
    }

    /**
     * Cleanup resources when this goes out of scope.
     */
    public void destroy() {
        mInfoBoxCoordinator.destroy();
        mInfoBoxCoordinator = null;
    }

    /**
     * Show the onboarding screen and call {@code callback} with {@code true} if the user agreed to
     * proceed, false otherwise.
     */
    public void showOnboarding(Callback<Boolean> callback) {
        mModel.getHeaderModel().set(AssistantHeaderModel.VISIBLE, false);

        // Show overlay to prevent user from interacting with the page during onboarding.
        mModel.getOverlayModel().set(AssistantOverlayModel.STATE, AssistantOverlayState.FULL);

        View onboardingView = AssistantOnboardingCoordinator.show(
                mContent.mBottomBarView.getContext(), mContent.mBottomBarView, accepted -> {
                    mOnboardingScrollView = null;
                    if (!accepted) {
                        callback.onResult(false);
                        return;
                    }

                    mModel.getHeaderModel().set(AssistantHeaderModel.VISIBLE, true);

                    // Hide overlay.
                    mModel.getOverlayModel().set(
                            AssistantOverlayModel.STATE, AssistantOverlayState.HIDDEN);

                    callback.onResult(true);
                });
        mOnboardingScrollView = onboardingView.findViewById(R.id.onboarding_scroll_view);
    }

    /** Request showing the Assistant bottom bar view and expand the sheet. */
    public void showAndExpand() {
        if (mBottomSheetController.requestShowContent(mContent, /* animate= */ true)) {
            mBottomSheetController.expandSheet();
        }
    }

    /** Hide the Assistant bottom bar view. */
    public void hide() {
        mBottomSheetController.hideContent(mContent, /* animate= */ true);
    }

    private void setChildMarginTop(View child, int marginTop) {
        LinearLayout.LayoutParams params = (LinearLayout.LayoutParams) child.getLayoutParams();
        params.topMargin = marginTop;
        child.setLayoutParams(params);
    }

    /**
     * Observe {@code model} such that we set the topMargin of {@code carouselView} to {@code
     * marginTop} when {@code model} is not empty and set it to 0 otherwise.
     */
    private void setCarouselMarginTop(
            View carouselView, ListModel<AssistantChip> chipsModel, int marginTop) {
        chipsModel.addObserver(new AbstractListObserver<Void>() {
            @Override
            public void onDataSetChanged() {
                setChildMarginTop(carouselView, chipsModel.size() > 0 ? marginTop : 0);
            }
        });
    }

    @VisibleForTesting
    public AssistantCarouselCoordinator getSuggestionsCoordinator() {
        return mSuggestionsCoordinator;
    }

    @VisibleForTesting
    public AssistantCarouselCoordinator getActionsCoordinator() {
        return mActionsCoordinator;
    }

    private void setHorizontalMargins(View view) {
        LinearLayout.LayoutParams layoutParams = (LinearLayout.LayoutParams) view.getLayoutParams();
        int horizontalMargin = view.getContext().getResources().getDimensionPixelSize(
                R.dimen.autofill_assistant_bottombar_horizontal_spacing);
        layoutParams.setMarginStart(horizontalMargin);
        layoutParams.setMarginEnd(horizontalMargin);
        view.setLayoutParams(layoutParams);
    }

    // TODO(crbug.com/806868): Move this class at the top of the file once it is a static class.
    private class AssistantBottomSheetContent implements BottomSheet.BottomSheetContent {
        private final View mToolbarView;
        private final SizeListenableLinearLayout mBottomBarView;

        public AssistantBottomSheetContent(Context context) {
            mToolbarView = LayoutInflater.from(context).inflate(
                    R.layout.autofill_assistant_bottom_sheet_toolbar, /* root= */ null);
            mBottomBarView = (SizeListenableLinearLayout) LayoutInflater.from(context).inflate(
                    R.layout.autofill_assistant_bottom_sheet_content, /* root= */ null);
        }

        @Override
        public View getContentView() {
            return mBottomBarView;
        }

        @Nullable
        @Override
        public View getToolbarView() {
            return mToolbarView;
        }

        @Override
        public int getVerticalScrollOffset() {
            // TODO(crbug.com/806868): Have a single ScrollView container that contains all child
            // views (except carousels) instead.
            if (mOnboardingScrollView != null && mOnboardingScrollView.isShown()) {
                return mOnboardingScrollView.getScrollY();
            }

            if (mPaymentRequestCoordinator.getView().isShown()) {
                return mPaymentRequestCoordinator.getView().getScrollY();
            }

            return 0;
        }

        @Override
        public void destroy() {}

        @Override
        public int getPriority() {
            return BottomSheet.ContentPriority.HIGH;
        }

        @Override
        public boolean swipeToDismissEnabled() {
            return false;
        }

        @Override
        public boolean isPeekStateEnabled() {
            return true;
        }

        @Override
        public boolean wrapContentEnabled() {
            return true;
        }

        @Override
        public boolean hasCustomLifecycle() {
            return true;
        }

        @Override
        public boolean hasCustomScrimLifecycle() {
            return true;
        }

        @Override
        public boolean setContentSizeListener(@Nullable BottomSheet.ContentSizeListener listener) {
            mBottomBarView.setContentSizeListener(listener);
            return true;
        }

        @Override
        public int getSheetContentDescriptionStringId() {
            return R.string.autofill_assistant_sheet_content_description;
        }

        @Override
        public int getSheetHalfHeightAccessibilityStringId() {
            return R.string.autofill_assistant_sheet_half_height;
        }

        @Override
        public int getSheetFullHeightAccessibilityStringId() {
            return R.string.autofill_assistant_sheet_full_height;
        }

        @Override
        public int getSheetClosedAccessibilityStringId() {
            return R.string.autofill_assistant_sheet_closed;
        }
    }
}
