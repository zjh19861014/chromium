// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/favicon/favicon_loader.h"

#include "components/favicon/core/large_icon_service_impl.h"
#include "components/favicon_base/fallback_icon_style.h"
#include "components/favicon_base/favicon_types.h"
#import "ios/chrome/common/favicon/favicon_attributes.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "url/gurl.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace {

// Dummy URL for the favicon case.
const char kTestFaviconURL[] = "http://test/favicon";
// Dummy URL for the fallback case.
const char kTestFallbackURL[] = "http://test/fallback";

// Size of dummy favicon image.
const CGFloat kTestFaviconSize = 57;

// FaviconLoaderTest is parameterized on this enum to test both
// FaviconLoader::FaviconForPageUrl and FaviconLoader::FaviconForIconUrl.
enum FaviconUrlType { TEST_PAGE_URL, TEST_ICON_URL };

// FakeLargeIconService mimics a LargeIconService that returns a LargeIconResult
// with a test favicon image for kTestFaviconURL and a LargeIconResult
// initialized with FallbackIconStyle.
class FakeLargeIconService : public favicon::LargeIconServiceImpl {
 public:
  FakeLargeIconService()
      : favicon::LargeIconServiceImpl(/*favicon_service=*/nullptr,
                                      /*image_fetcher=*/nullptr) {}

  // Returns LargeIconResult with valid bitmap if |page_url| is
  // |kTestFaviconURL|, or LargeIconResult with fallback style.
  base::CancelableTaskTracker::TaskId
  GetLargeIconRawBitmapOrFallbackStyleForPageUrl(
      const GURL& page_url,
      int min_source_size_in_pixel,
      int desired_size_in_pixel,
      const favicon_base::LargeIconCallback& callback,
      base::CancelableTaskTracker* tracker) override {
    if (page_url.spec() == kTestFaviconURL) {
      favicon_base::FaviconRawBitmapResult bitmapResult;
      bitmapResult.expired = false;

      // Create bitmap.
      scoped_refptr<base::RefCountedBytes> data(new base::RefCountedBytes());
      SkBitmap bitmap;
      bitmap.allocN32Pixels(30, 30);
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false, &data->data());
      bitmapResult.bitmap_data = data;

      favicon_base::LargeIconResult result(bitmapResult);
      callback.Run(result);
    } else {
      favicon_base::FallbackIconStyle* fallback =
          new favicon_base::FallbackIconStyle();
      favicon_base::LargeIconResult result(fallback);
      fallback = NULL;
      callback.Run(result);
    }

    return 1;
  }

  // Returns the same as |GetLargeIconRawBitmapOrFallbackStyleForPageUrl|.
  base::CancelableTaskTracker::TaskId
  GetLargeIconRawBitmapOrFallbackStyleForIconUrl(
      const GURL& icon_url,
      int min_source_size_in_pixel,
      int desired_size_in_pixel,
      const favicon_base::LargeIconCallback& callback,
      base::CancelableTaskTracker* tracker) override {
    return GetLargeIconRawBitmapOrFallbackStyleForPageUrl(
        icon_url, min_source_size_in_pixel, desired_size_in_pixel, callback,
        tracker);
  }
};

class FaviconLoaderTest : public PlatformTest,
                          public ::testing::WithParamInterface<FaviconUrlType> {
 protected:
  FaviconLoaderTest() : favicon_loader_(&large_icon_service_) {}

  FakeLargeIconService large_icon_service_;
  FaviconLoader favicon_loader_;

  // Returns FaviconLoader::FaviconForPageUrl or
  // FaviconLoader::FaviconForIconUrl depending on the TEST_P param.
  FaviconAttributes* FaviconForUrl(
      const GURL& url,
      FaviconLoader::FaviconAttributesCompletionBlock callback) {
    if (GetParam() == TEST_PAGE_URL) {
      return favicon_loader_.FaviconForPageUrl(
          url, kTestFaviconSize, kTestFaviconSize,
          /*fallback_to_google_server=*/false, callback);
    } else {
      return favicon_loader_.FaviconForIconUrl(url, kTestFaviconSize,
                                               kTestFaviconSize, callback);
    }
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FaviconLoaderTest);
};

// Tests that image is returned when a favicon is retrieved from
// LargeIconService.
TEST_P(FaviconLoaderTest, FaviconForPageUrl) {
  __block bool callback_executed = false;
  auto confirmation_block = ^(FaviconAttributes* favicon_attributes) {
    callback_executed = true;
    EXPECT_TRUE(favicon_attributes.faviconImage);
  };
  FaviconForUrl(GURL(kTestFaviconURL), confirmation_block);
  EXPECT_TRUE(callback_executed);
}

// Tests that fallback data is provided when no favicon is retrieved from
// LargeIconService.
TEST_P(FaviconLoaderTest, FallbackIcon) {
  __block bool callback_executed = false;
  auto confirmation_block = ^(FaviconAttributes* favicon_attributes) {
    callback_executed = true;
    EXPECT_FALSE(favicon_attributes.faviconImage);
    EXPECT_TRUE(favicon_attributes.monogramString);
    EXPECT_TRUE(favicon_attributes.textColor);
    EXPECT_TRUE(favicon_attributes.backgroundColor);
  };
  FaviconForUrl(GURL(kTestFallbackURL), confirmation_block);
  EXPECT_TRUE(callback_executed);
}

// Tests that when favicon is in cache, it is returned immediately. The callback
// should never be executed.
TEST_P(FaviconLoaderTest, Cache) {
  // Favicon retrieval that should put it in the cache.
  FaviconForUrl(GURL(kTestFaviconURL), nil);
  __block bool callback_executed = false;
  auto confirmation_block = ^(FaviconAttributes* faviconAttributes) {
    callback_executed = true;
  };
  FaviconAttributes* attributes_result =
      FaviconForUrl(GURL(kTestFaviconURL), confirmation_block);
  EXPECT_TRUE(attributes_result.faviconImage);
  // Favicon should be returned by cache, so callback should never exectue.
  EXPECT_FALSE(callback_executed);
}

INSTANTIATE_TEST_SUITE_P(ProgrammaticFaviconLoaderTest,
                         FaviconLoaderTest,
                         ::testing::Values(FaviconUrlType::TEST_PAGE_URL,
                                           FaviconUrlType::TEST_ICON_URL));

}  // namespace
