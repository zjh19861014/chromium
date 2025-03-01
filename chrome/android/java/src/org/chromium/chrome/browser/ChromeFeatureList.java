// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser;

import org.chromium.base.VisibleForTesting;
import org.chromium.base.annotations.JNINamespace;
import org.chromium.base.annotations.MainDex;
import org.chromium.base.library_loader.LibraryLoader;

import java.util.Map;

/**
 * Java accessor for base/feature_list.h state.
 */
@JNINamespace("chrome::android")
@MainDex
public abstract class ChromeFeatureList {
    /** Map that stores substitution feature flags for tests. */
    private static Map<String, Boolean> sTestFeatures;

    // Prevent instantiation.
    private ChromeFeatureList() {}

    /**
     * Sets the feature flags to use in JUnit tests, since native calls are not available there.
     * Do not use directly, prefer using the {@link Features} annotation.
     *
     * @see Features
     * @see Features.Processor
     */
    @VisibleForTesting
    public static void setTestFeatures(Map<String, Boolean> features) {
        sTestFeatures = features;
    }

    /**
     * @return Whether the native FeatureList has been initialized. If this method returns false,
     *         none of the methods in this class that require native access should be called (except
     *         in tests if test features have been set).
     */
    public static boolean isInitialized() {
        if (sTestFeatures != null) return true;
        if (!LibraryLoader.getInstance().isInitialized()) return false;

        // Even if the native library is loaded, the C++ FeatureList might not be initialized yet.
        // In that case, accessing it will not immediately fail, but instead cause a crash later
        // when it is initialized. Return whether the native FeatureList has been initialized,
        // so the return value can be tested, or asserted for a more actionable stack trace
        // on failure.
        //
        // The FeatureList is however guaranteed to be initialized by the time
        // AsyncInitializationActivity#finishNativeInitialization is called.
        return nativeIsInitialized();
    }

    /**
     * Returns whether the specified feature is enabled or not.
     *
     * Note: Features queried through this API must be added to the array
     * |kFeaturesExposedToJava| in chrome/browser/android/chrome_feature_list.cc
     *
     * @param featureName The name of the feature to query.
     * @return Whether the feature is enabled or not.
     */
    public static boolean isEnabled(String featureName) {
        if (sTestFeatures != null) {
            Boolean enabled = sTestFeatures.get(featureName);
            if (enabled == null) throw new IllegalArgumentException(featureName);
            return enabled;
        }

        assert isInitialized();
        return nativeIsEnabled(featureName);
    }

    /**
     * Returns a field trial param for the specified feature.
     *
     * Note: Features queried through this API must be added to the array
     * |kFeaturesExposedToJava| in chrome/browser/android/chrome_feature_list.cc
     *
     * @param featureName The name of the feature to retrieve a param for.
     * @param paramName The name of the param for which to get as an integer.
     * @return The parameter value as a String. The string is empty if the feature does not exist or
     *   the specified parameter does not exist.
     */
    public static String getFieldTrialParamByFeature(String featureName, String paramName) {
        if (sTestFeatures != null) return "";
        assert isInitialized();
        return nativeGetFieldTrialParamByFeature(featureName, paramName);
    }

    /**
     * Returns a field trial param as an int for the specified feature.
     *
     * Note: Features queried through this API must be added to the array
     * |kFeaturesExposedToJava| in chrome/browser/android/chrome_feature_list.cc
     *
     * @param featureName The name of the feature to retrieve a param for.
     * @param paramName The name of the param for which to get as an integer.
     * @param defaultValue The integer value to use if the param is not available.
     * @return The parameter value as an int. Default value if the feature does not exist or the
     *         specified parameter does not exist or its string value does not represent an int.
     */
    public static int getFieldTrialParamByFeatureAsInt(
            String featureName, String paramName, int defaultValue) {
        if (sTestFeatures != null) return defaultValue;
        assert isInitialized();
        return nativeGetFieldTrialParamByFeatureAsInt(featureName, paramName, defaultValue);
    }

    /**
     * Returns a field trial param as a double for the specified feature.
     *
     * Note: Features queried through this API must be added to the array
     * |kFeaturesExposedToJava| in chrome/browser/android/chrome_feature_list.cc
     *
     * @param featureName The name of the feature to retrieve a param for.
     * @param paramName The name of the param for which to get as an integer.
     * @param defaultValue The double value to use if the param is not available.
     * @return The parameter value as a double. Default value if the feature does not exist or the
     *         specified parameter does not exist or its string value does not represent a double.
     */
    public static double getFieldTrialParamByFeatureAsDouble(
            String featureName, String paramName, double defaultValue) {
        if (sTestFeatures != null) return defaultValue;
        assert isInitialized();
        return nativeGetFieldTrialParamByFeatureAsDouble(featureName, paramName, defaultValue);
    }

    /**
     * Returns a field trial param as a boolean for the specified feature.
     *
     * Note: Features queried through this API must be added to the array
     * |kFeaturesExposedToJava| in chrome/browser/android/chrome_feature_list.cc
     *
     * @param featureName The name of the feature to retrieve a param for.
     * @param paramName The name of the param for which to get as an integer.
     * @param defaultValue The boolean value to use if the param is not available.
     * @return The parameter value as a boolean. Default value if the feature does not exist or the
     *         specified parameter does not exist or its string value is neither "true" nor "false".
     */
    public static boolean getFieldTrialParamByFeatureAsBoolean(
            String featureName, String paramName, boolean defaultValue) {
        if (sTestFeatures != null) return defaultValue;
        assert isInitialized();
        return nativeGetFieldTrialParamByFeatureAsBoolean(featureName, paramName, defaultValue);
    }

    // Alphabetical:
    public static final String ALLOW_NEW_INCOGNITO_TAB_INTENTS = "AllowNewIncognitoTabIntents";
    public static final String ALLOW_REMOTE_CONTEXT_FOR_NOTIFICATIONS =
            "AllowRemoteContextForNotifications";
    public static final String ALLOW_STARTING_SERVICE_MANAGER_ONLY =
            "AllowStartingServiceManagerOnly";
    public static final String AUTOFILL_ALLOW_NON_HTTP_ACTIVATION =
            "AutofillAllowNonHttpActivation";
    public static final String AUTOFILL_ENABLE_COMPANY_NAME = "AutofillEnableCompanyName";
    public static final String ADJUST_WEBAPK_INSTALLATION_SPACE = "AdjustWebApkInstallationSpace";
    public static final String ANDROID_NIGHT_MODE = "AndroidNightMode";
    public static final String ANDROID_NIGHT_MODE_CCT = "AndroidNightModeCCT";
    public static final String ANDROID_PAY_INTEGRATION_V1 = "AndroidPayIntegrationV1";
    public static final String ANDROID_PAY_INTEGRATION_V2 = "AndroidPayIntegrationV2";
    public static final String ANDROID_PAYMENT_APPS = "AndroidPaymentApps";
    public static final String ANDROID_SEARCH_ENGINE_CHOICE_NOTIFICATION =
            "AndroidSearchEngineChoiceNotification";
    public static final String ANDROID_SITE_SETTINGS_UI_REFRESH = "AndroidSiteSettingsUIRefresh";
    public static final String APP_NOTIFICATION_STATUS_MESSAGING = "AppNotificationStatusMessaging";
    public static final String AUTOFILL_ASSISTANT = "AutofillAssistant";
    public static final String AUTOFILL_MANUAL_FALLBACK_ANDROID = "AutofillManualFallbackAndroid";
    public static final String AUTOFILL_REFRESH_STYLE_ANDROID = "AutofillRefreshStyleAndroid";
    public static final String AUTOFILL_KEYBOARD_ACCESSORY = "AutofillKeyboardAccessory";
    public static final String BACKGROUND_TASK_SCHEDULER_FOR_BACKGROUND_SYNC =
            "BackgroundTaskSchedulerForBackgroundSync";
    public static final String CAPTIVE_PORTAL_CERTIFICATE_LIST = "CaptivePortalCertificateList";
    public static final String CCT_BACKGROUND_TAB = "CCTBackgroundTab";
    public static final String CCT_MODULE = "CCTModule";
    public static final String CCT_MODULE_CACHE = "CCTModuleCache";
    public static final String CCT_MODULE_CUSTOM_HEADER = "CCTModuleCustomHeader";
    public static final String CCT_MODULE_CUSTOM_REQUEST_HEADER = "CCTModuleCustomRequestHeader";
    public static final String CCT_MODULE_DEX_LOADING = "CCTModuleDexLoading";
    public static final String CCT_MODULE_POST_MESSAGE = "CCTModulePostMessage";
    public static final String CCT_MODULE_USE_INTENT_EXTRAS = "CCTModuleUseIntentExtras";
    public static final String CCT_EXTERNAL_LINK_HANDLING = "CCTExternalLinkHandling";
    public static final String CCT_POST_MESSAGE_API = "CCTPostMessageAPI";
    public static final String CCT_REDIRECT_PRECONNECT = "CCTRedirectPreconnect";
    public static final String CCT_RESOURCE_PREFETCH = "CCTResourcePrefetch";
    public static final String CCT_REPORT_PARALLEL_REQUEST_STATUS =
            "CCTReportParallelRequestStatus";
    public static final String CCT_TARGET_TRANSLATE_LANGUAGE = "CCTTargetTranslateLanguage";
    public static final String CHROME_DUET = "ChromeDuet";
    public static final String CHROME_DUET_ADAPTIVE = "ChromeDuetAdaptive";
    public static final String DONT_AUTO_HIDE_BROWSER_CONTROLS = "DontAutoHideBrowserControls";
    public static final String CHROME_SMART_SELECTION = "ChromeSmartSelection";
    public static final String CLEAR_OLD_BROWSING_DATA = "ClearOldBrowsingData";
    public static final String CLIPBOARD_CONTENT_SETTING = "ClipboardContentSetting";
    public static final String COMMAND_LINE_ON_NON_ROOTED = "CommandLineOnNonRooted";
    public static final String CONTENT_SUGGESTIONS_NOTIFICATIONS =
            "ContentSuggestionsNotifications";
    public static final String CONTENT_SUGGESTIONS_SCROLL_TO_LOAD =
            "ContentSuggestionsScrollToLoad";
    public static final String CONTEXTUAL_SEARCH_DEFINITIONS = "ContextualSearchDefinitions";
    public static final String CONTEXTUAL_SEARCH_ML_TAP_SUPPRESSION =
            "ContextualSearchMlTapSuppression";
    public static final String CONTEXTUAL_SEARCH_SIMPLIFIED_SERVER =
            "ContextualSearchSimplifiedServer";
    public static final String CONTEXTUAL_SEARCH_SECOND_TAP = "ContextualSearchSecondTap";
    public static final String CONTEXTUAL_SEARCH_TAP_DISABLE_OVERRIDE =
            "ContextualSearchTapDisableOverride";
    public static final String CONTEXTUAL_SEARCH_TRANSLATION_MODEL =
            "ContextualSearchTranslationModel";
    public static final String CONTEXTUAL_SEARCH_UNITY_INTEGRATION =
            "ContextualSearchUnityIntegration";
    public static final String CONTEXTUAL_SUGGESTIONS_BUTTON = "ContextualSuggestionsButton";
    public static final String CONTEXTUAL_SUGGESTIONS_OPT_OUT = "ContextualSuggestionsOptOut";
    public static final String CONTEXTUAL_SUGGESTIONS_IPH_REVERSE_SCROLL =
            "ContextualSuggestionsIPHReverseScroll";
    public static final String CUSTOM_CONTEXT_MENU = "CustomContextMenu";
    public static final String DATA_SAVER_LITE_MODE_REBRANDING = "DataSaverLiteModeRebranding";
    public static final String DELEGATE_OVERSCROLL_SWIPES = "DelegateOverscrollSwipes";
    public static final String DONT_PREFETCH_LIBRARIES = "DontPrefetchLibraries";
    public static final String DOWNLOAD_LOCATION_SHOW_IMAGE_IN_GALLERY =
            "DownloadLocationShowImageInGallery";
    public static final String DOWNLOAD_HOME_SHOW_STORAGE_INFO = "DownloadHomeShowStorageInfo";
    public static final String DOWNLOAD_PROGRESS_INFOBAR = "DownloadProgressInfoBar";
    public static final String DOWNLOAD_HOME_V2 = "DownloadHomeV2";
    public static final String DOWNLOAD_RENAME = "DownloadRename";
    public static final String DOWNLOADS_FOREGROUND = "DownloadsForeground";
    public static final String DOWNLOADS_AUTO_RESUMPTION_NATIVE = "DownloadsAutoResumptionNative";
    public static final String DOWNLOAD_OFFLINE_CONTENT_PROVIDER =
            "UseDownloadOfflineContentProvider";
    public static final String DOWNLOADS_LOCATION_CHANGE = "DownloadsLocationChange";
    public static final String DOWNLOAD_TAB_MANAGEMENT_MODULE = "DownloadTabManagementModule";
    public static final String DRAW_VERTICALLY_EDGE_TO_EDGE = "DrawVerticallyEdgeToEdge";
    public static final String EPHEMERAL_TAB = "EphemeralTab";
    public static final String EXPERIMENTAL_APP_BANNERS = "ExperimentalAppBanners";
    public static final String EXPLICIT_LANGUAGE_ASK = "ExplicitLanguageAsk";
    public static final String EXPLORE_SITES = "ExploreSites";
    public static final String FCM_INVALIDATIONS = "FCMInvalidations";
    public static final String GRANT_NOTIFICATIONS_TO_DSE = "GrantNotificationsToDSE";
    public static final String HANDLE_MEDIA_INTENTS = "HandleMediaIntents";
    public static final String HIDE_USER_DATA_FROM_INCOGNITO_NOTIFICATIONS =
            "HideUserDataFromIncognitoNotifications";
    public static final String HOME_PAGE_BUTTON_FORCE_ENABLED = "HomePageButtonForceEnabled";
    public static final String HOMEPAGE_TILE = "HomepageTile";
    public static final String HORIZONTAL_TAB_SWITCHER_ANDROID = "HorizontalTabSwitcherAndroid";
    public static final String IMMERSIVE_UI_MODE = "ImmersiveUiMode";
    public static final String INCOGNITO_STRINGS = "IncognitoStrings";
    public static final String INLINE_UPDATE_FLOW = "InlineUpdateFlow";
    public static final String INSTALLABLE_AMBIENT_BADGE_INFOBAR = "InstallableAmbientBadgeInfoBar";
    public static final String INTENT_BLOCK_EXTERNAL_FORM_REDIRECT_NO_GESTURE =
            "IntentBlockExternalFormRedirectsNoGesture";
    public static final String INTEREST_FEED_CONTENT_SUGGESTIONS = "InterestFeedContentSuggestions";
    public static final String JELLY_BEAN_SUPPORTED = "JellyBeanSupported";
    public static final String LANGUAGES_PREFERENCE = "LanguagesPreference";
    public static final String LOOKALIKE_NAVIGATION_URL_SUGGESTIONS_UI =
            "LookalikeUrlNavigationSuggestionsUI";
    public static final String SEARCH_ENGINE_PROMO_EXISTING_DEVICE =
            "SearchEnginePromo.ExistingDevice";
    public static final String SEARCH_ENGINE_PROMO_NEW_DEVICE = "SearchEnginePromo.NewDevice";
    public static final String MOBILE_IDENTITY_CONSISTENCY = "MobileIdentityConsistency";
    public static final String MODAL_PERMISSION_PROMPTS = "ModalPermissionPrompts";
    public static final String MODAL_PERMISSION_DIALOG_VIEW = "ModalPermissionDialogView";
    public static final String NEW_PHOTO_PICKER = "NewPhotoPicker";
    public static final String NETWORK_SERVICE = "NetworkService";
    public static final String NO_CREDIT_CARD_ABORT = "NoCreditCardAbort";
    public static final String NTP_ARTICLE_SUGGESTIONS = "NTPArticleSuggestions";
    public static final String NTP_BUTTON = "NTPButton";
    public static final String NTP_LAUNCH_AFTER_INACTIVITY = "NTPLaunchAfterInactivity";
    public static final String OFFLINE_INDICATOR = "OfflineIndicator";
    public static final String OFFLINE_INDICATOR_ALWAYS_HTTP_PROBE =
            "OfflineIndicatorAlwaysHttpProbe";
    public static final String OFFLINE_PAGES_DESCRIPTIVE_FAIL_STATUS =
            "OfflinePagesDescriptiveFailStatus";
    public static final String OFFLINE_PAGES_DESCRIPTIVE_PENDING_STATUS =
            "OfflinePagesDescriptivePendingStatus";
    public static final String OFFLINE_PAGES_LIVE_PAGE_SHARING = "OfflinePagesLivePageSharing";
    public static final String OFFLINE_PAGES_PREFETCHING = "OfflinePagesPrefetching";
    public static final String OMNIBOX_HIDE_SCHEME_IN_STEADY_STATE =
            "OmniboxUIExperimentHideSteadyStateUrlScheme";
    public static final String OMNIBOX_HIDE_TRIVIAL_SUBDOMAINS_IN_STEADY_STATE =
            "OmniboxUIExperimentHideSteadyStateUrlTrivialSubdomains";
    public static final String OMNIBOX_NEW_ANSWER_LAYOUT = "OmniboxNewAnswerLayout";
    public static final String OMNIBOX_RICH_ENTITY_SUGGESTIONS = "OmniboxRichEntitySuggestions";
    public static final String OMNIBOX_SHOW_SUGGESTION_FAVICONS =
            "OmniboxUIExperimentShowSuggestionFavicons";
    public static final String OMNIBOX_SPARE_RENDERER = "OmniboxSpareRenderer";
    public static final String OVERSCROLL_HISTORY_NAVIGATION = "OverscrollHistoryNavigation";
    public static final String PAY_WITH_GOOGLE_V1 = "PayWithGoogleV1";
    public static final String PASSWORDS_KEYBOARD_ACCESSORY = "PasswordsKeyboardAccessory";
    public static final String PERMISSION_DELEGATION = "PermissionDelegation";
    public static final String PER_METHOD_CAN_MAKE_PAYMENT_QUOTA =
            "WebPaymentsPerMethodCanMakePaymentQuota";
    public static final String PREDICTIVE_PREFETCHING_ALLOWED_ON_ALL_CONNECTION_TYPES =
            "PredictivePrefetchingAllowedOnAllConnectionTypes";
    public static final String PRIORITIZE_BOOTSTRAP_TASKS = "PrioritizeBootstrapTasks";
    public static final String PROGRESS_BAR_THROTTLE = "ProgressBarThrottle";
    public static final String PWA_PERSISTENT_NOTIFICATION = "PwaPersistentNotification";
    public static final String REACHED_CODE_PROFILER = "ReachedCodeProfiler";
    public static final String READER_MODE_IN_CCT = "ReaderModeInCCT";
    public static final String REMOVE_NAVIGATION_HISTORY = "RemoveNavigationHistory";
    public static final String REVAMPED_CONTEXT_MENU = "RevampedContextMenu";
    public static final String SEARCH_READY_OMNIBOX = "SearchReadyOmnibox";
    public static final String SEND_TAB_TO_SELF = "SyncSendTabToSelf";
    public static final String SENSOR_CONTENT_SETTING = "SensorContentSetting";
    public static final String SERVICE_MANAGER_FOR_DOWNLOAD = "ServiceManagerForDownload";
    public static final String SERVICE_WORKER_PAYMENT_APPS = "ServiceWorkerPaymentApps";
    public static final String SHOW_TRUSTED_PUBLISHER_URL = "ShowTrustedPublisherURL";
    public static final String SOLE_INTEGRATION = "SoleIntegration";
    public static final String SSL_COMMITTED_INTERSTITIALS = "SSLCommittedInterstitials";
    public static final String SPANNABLE_INLINE_AUTOCOMPLETE = "SpannableInlineAutocomplete";
    public static final String SUBRESOURCE_FILTER = "SubresourceFilter";
    public static final String QUERY_IN_OMNIBOX = "QueryInOmnibox";
    public static final String TAB_GROUPS_ANDROID = "TabGroupsAndroid";
    public static final String TAB_GRID_LAYOUT_ANDROID = "TabGridLayoutAndroid";
    public static final String TAB_REPARENTING = "TabReparenting";
    public static final String TAB_SWITCHER_ON_RETURN = "TabSwitcherOnReturn";
    public static final String TRANSLATE_ANDROID_MANUAL_TRIGGER = "TranslateAndroidManualTrigger";
    public static final String TRUSTED_WEB_ACTIVITY = "TrustedWebActivity";
    public static final String TRUSTED_WEB_ACTIVITY_POST_MESSAGE = "TrustedWebActivityPostMessage";
    public static final String VIDEO_PERSISTENCE = "VideoPersistence";
    public static final String UNIFIED_CONSENT = "UnifiedConsent";
    public static final String USAGE_STATS = "UsageStats";
    public static final String VR_BROWSING_FEEDBACK = "VrBrowsingFeedback";
    public static final String USER_ACTIVATION_V2 = "UserActivationV2";
    public static final String WEB_AUTH = "WebAuthentication";
    public static final String WEB_PAYMENTS = "WebPayments";
    public static final String WEB_PAYMENTS_METHOD_SECTION_ORDER_V2 =
            "WebPaymentsMethodSectionOrderV2";
    public static final String WEB_PAYMENTS_MODIFIERS = "WebPaymentsModifiers";
    public static final String WEB_PAYMENTS_RETURN_GOOGLE_PAY_IN_BASIC_CARD =
            "ReturnGooglePayInBasicCard";
    public static final String WEB_PAYMENTS_SINGLE_APP_UI_SKIP = "WebPaymentsSingleAppUiSkip";
    public static final String SERVICE_MANAGER_FOR_BACKGROUND_PREFETCH =
            "ServiceManagerForBackgroundPrefetch";

    private static native boolean nativeIsInitialized();
    private static native boolean nativeIsEnabled(String featureName);
    private static native String nativeGetFieldTrialParamByFeature(
            String featureName, String paramName);
    private static native int nativeGetFieldTrialParamByFeatureAsInt(
            String featureName, String paramName, int defaultValue);
    private static native double nativeGetFieldTrialParamByFeatureAsDouble(
            String featureName, String paramName, double defaultValue);
    private static native boolean nativeGetFieldTrialParamByFeatureAsBoolean(
            String featureName, String paramName, boolean defaultValue);
}
