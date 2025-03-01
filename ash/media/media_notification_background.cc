// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/media/media_notification_background.h"

#include <algorithm>
#include <vector>

#include "skia/ext/image_operations.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_analysis.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/views/view.h"

namespace ash {

namespace {

constexpr int kMediaImageGradientWidth = 40;

constexpr SkColor kMediaNotificationDefaultBackgroundColor = SK_ColorWHITE;

// The ratio for a background color option to be considered very popular.
constexpr double kMediaNotificationBackgroundColorVeryPopularRatio = 2.5;

// The ratio for the most popular foreground color to be used.
constexpr double kMediaNotificationForegroundColorMostPopularRatio = 0.01;

// The minimum saturation for the most popular foreground color to be used.
constexpr double kMediaNotificationForegroundColorMostPopularMinSaturation =
    0.19;

// The ratio for the more vibrant foreground color to use.
constexpr double kMediaNotificationForegroundColorMoreVibrantRatio = 1.0;

bool IsNearlyWhiteOrBlack(SkColor color) {
  color_utils::HSL hsl;
  color_utils::SkColorToHSL(color, &hsl);
  return hsl.l >= 0.9 || hsl.l <= 0.08;
}

int GetHueDegrees(const SkColor& color) {
  color_utils::HSL hsl;
  color_utils::SkColorToHSL(color, &hsl);
  return hsl.h * 360;
}

double GetSaturation(const color_utils::Swatch& swatch) {
  color_utils::HSL hsl;
  color_utils::SkColorToHSL(swatch.color, &hsl);
  return hsl.s;
}

bool IsForegroundColorSwatchAllowed(const SkColor& background,
                                    const SkColor& candidate) {
  if (IsNearlyWhiteOrBlack(candidate))
    return false;

  if (IsNearlyWhiteOrBlack(background))
    return true;

  int diff = abs(GetHueDegrees(candidate) - GetHueDegrees(background));
  return diff > 10 && diff < 350;
}

base::Optional<SkColor> GetNotificationBackgroundColor(const SkBitmap* source) {
  if (!source || source->empty() || source->isNull())
    return base::nullopt;

  std::vector<color_utils::Swatch> swatches =
      color_utils::CalculateColorSwatches(
          *source, 16, gfx::Rect(source->width() / 2, source->height()),
          base::nullopt);

  if (swatches.empty())
    return base::nullopt;

  base::Optional<color_utils::Swatch> most_popular;
  base::Optional<color_utils::Swatch> non_white_black;

  // Find the most popular color with the most weight and the color which
  // is the color with the most weight that is not white or black.
  for (auto& swatch : swatches) {
    if (!IsNearlyWhiteOrBlack(swatch.color) &&
        (!non_white_black || swatch.population > non_white_black->population)) {
      non_white_black = swatch;
    }

    if (most_popular && swatch.population < most_popular->population)
      continue;

    most_popular = swatch;
  }

  DCHECK(most_popular);

  // If the most popular color is not white or black then we should use that.
  if (!IsNearlyWhiteOrBlack(most_popular->color))
    return most_popular->color;

  // If we could not find a color that is not white or black then we should
  // use the most popular color.
  if (!non_white_black)
    return most_popular->color;

  // If the most popular color is very popular then we should use that color.
  if (static_cast<double>(most_popular->population) /
          non_white_black->population >
      kMediaNotificationBackgroundColorVeryPopularRatio) {
    return most_popular->color;
  }

  return non_white_black->color;
}

color_utils::Swatch SelectVibrantSwatch(const color_utils::Swatch& more_vibrant,
                                        const color_utils::Swatch& vibrant) {
  if ((static_cast<double>(more_vibrant.population) / vibrant.population) <
      kMediaNotificationForegroundColorMoreVibrantRatio) {
    return vibrant;
  }

  return more_vibrant;
}

color_utils::Swatch SelectMutedSwatch(const color_utils::Swatch& muted,
                                      const color_utils::Swatch& more_muted) {
  double population_ratio =
      static_cast<double>(muted.population) / more_muted.population;

  // Use the swatch with the higher saturation ratio.
  return (GetSaturation(muted) * population_ratio > GetSaturation(more_muted))
             ? muted
             : more_muted;
}

base::Optional<SkColor> GetNotificationForegroundColor(
    const base::Optional<SkColor>& background_color,
    const SkBitmap* source) {
  if (!background_color || !source || source->empty() || source->isNull())
    return base::nullopt;

  const bool is_light =
      color_utils::GetRelativeLuminance(*background_color) > 0.5;
  const SkColor fallback_color = is_light ? SK_ColorBLACK : SK_ColorWHITE;

  gfx::Rect bitmap_area(source->width(), source->height());
  bitmap_area.Inset(source->width() * 0.4, 0, 0, 0);

  // If the background color is dark we want to look for colors that are darker
  // and vice versa.
  const color_utils::LumaRange more_luma_range =
      is_light ? color_utils::LumaRange::DARK : color_utils::LumaRange::LIGHT;

  std::vector<color_utils::ColorProfile> color_profiles;
  color_profiles.push_back(color_utils::ColorProfile(
      more_luma_range, color_utils::SaturationRange::VIBRANT));
  color_profiles.push_back(color_utils::ColorProfile(
      color_utils::LumaRange::NORMAL, color_utils::SaturationRange::VIBRANT));
  color_profiles.push_back(color_utils::ColorProfile(
      color_utils::LumaRange::NORMAL, color_utils::SaturationRange::MUTED));
  color_profiles.push_back(color_utils::ColorProfile(
      more_luma_range, color_utils::SaturationRange::MUTED));
  color_profiles.push_back(color_utils::ColorProfile(
      color_utils::LumaRange::ANY, color_utils::SaturationRange::ANY));

  std::vector<color_utils::Swatch> best_swatches =
      color_utils::CalculateProminentColorsOfBitmap(
          *source, color_profiles, &bitmap_area,
          base::BindRepeating(&IsForegroundColorSwatchAllowed,
                              background_color.value()));

  if (best_swatches.empty())
    return fallback_color;

  DCHECK_EQ(5u, best_swatches.size());

  const color_utils::Swatch& more_vibrant = best_swatches[0];
  const color_utils::Swatch& vibrant = best_swatches[1];
  const color_utils::Swatch& muted = best_swatches[2];
  const color_utils::Swatch& more_muted = best_swatches[3];
  const color_utils::Swatch& most_popular = best_swatches[4];

  // We are looking for a fraction that is at least 0.2% of the image.
  const size_t population_min =
      std::min(bitmap_area.width() * bitmap_area.height(),
               color_utils::kMaxConsideredPixelsForSwatches) *
      0.002;

  // This selection algorithm is an implementation of MediaNotificationProcessor
  // from Android. It will select more vibrant colors first since they stand out
  // better against the background. If not, it will fallback to muted colors,
  // the most popular color and then either white/black. Any swatch has to be
  // above a minimum population threshold to be determined significant enough in
  // the artwork to be used.
  base::Optional<color_utils::Swatch> swatch;
  if (more_vibrant.population > population_min &&
      vibrant.population > population_min) {
    swatch = SelectVibrantSwatch(more_vibrant, vibrant);
  } else if (more_vibrant.population > population_min) {
    swatch = more_vibrant;
  } else if (vibrant.population > population_min) {
    swatch = vibrant;
  } else if (muted.population > population_min &&
             more_muted.population > population_min) {
    swatch = SelectMutedSwatch(muted, more_muted);
  } else if (muted.population > population_min) {
    swatch = muted;
  } else if (more_muted.population > population_min) {
    swatch = more_muted;
  } else if (most_popular.population > population_min) {
    return most_popular.color;
  } else {
    return fallback_color;
  }

  if (most_popular == *swatch)
    return swatch->color;

  if (static_cast<double>(swatch->population) / most_popular.population <
          kMediaNotificationForegroundColorMostPopularRatio &&
      GetSaturation(most_popular) >
          kMediaNotificationForegroundColorMostPopularMinSaturation) {
    return most_popular.color;
  }

  return swatch->color;
}

}  // namespace

MediaNotificationBackground::MediaNotificationBackground(
    views::View* owner,
    int top_radius,
    int bottom_radius,
    double artwork_max_width_pct)
    : owner_(owner),
      top_radius_(top_radius),
      bottom_radius_(bottom_radius),
      artwork_max_width_pct_(artwork_max_width_pct) {
  DCHECK(owner);
}

MediaNotificationBackground::~MediaNotificationBackground() = default;

void MediaNotificationBackground::Paint(gfx::Canvas* canvas,
                                        views::View* view) const {
  DCHECK(view);

  gfx::ScopedCanvas scoped_canvas(canvas);
  gfx::Rect bounds = view->GetContentsBounds();

  {
    // Draw a rounded rectangle which the background will be clipped to. The
    // radius is provided by the notification and can change based on where in
    // the list the notification is.
    const SkScalar top_radius = SkIntToScalar(top_radius_);
    const SkScalar bottom_radius = SkIntToScalar(bottom_radius_);

    const SkScalar radii[8] = {top_radius,    top_radius,    top_radius,
                               top_radius,    bottom_radius, bottom_radius,
                               bottom_radius, bottom_radius};

    SkPath path;
    path.addRoundRect(gfx::RectToSkRect(bounds), radii, SkPath::kCW_Direction);
    canvas->ClipPath(path, true);
  }

  {
    // Draw the artwork. The artwork is resized to the height of the view while
    // maintaining the aspect ratio.
    gfx::Rect source_bounds =
        gfx::Rect(0, 0, artwork_.width(), artwork_.height());
    gfx::Rect artwork_bounds = GetArtworkBounds(bounds);

    canvas->DrawImageInt(
        artwork_, source_bounds.x(), source_bounds.y(), source_bounds.width(),
        source_bounds.height(), artwork_bounds.x(), artwork_bounds.y(),
        artwork_bounds.width(), artwork_bounds.height(), false /* filter */);
  }

  // Draw a filled rectangle which will act as the main background of the
  // notification. This may cover up some of the artwork.
  const SkColor background_color =
      background_color_.value_or(kMediaNotificationDefaultBackgroundColor);
  canvas->FillRect(GetFilledBackgroundBounds(bounds), background_color);

  {
    // Draw a gradient to fade the color background and the image together.
    gfx::Rect draw_bounds = GetGradientBounds(bounds);

    const SkColor colors[2] = {
        background_color, SkColorSetA(background_color, SK_AlphaTRANSPARENT)};
    const SkPoint points[2] = {GetGradientStartPoint(draw_bounds),
                               GetGradientEndPoint(draw_bounds)};

    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kFill_Style);
    flags.setShader(cc::PaintShader::MakeLinearGradient(points, colors, nullptr,
                                                        2, SkTileMode::kClamp));

    canvas->DrawRect(draw_bounds, flags);
  }
}

void MediaNotificationBackground::UpdateArtwork(const gfx::ImageSkia& image) {
  if (artwork_.BackedBySameObjectAs(image))
    return;

  artwork_ = image;
  background_color_ = GetNotificationBackgroundColor(artwork_.bitmap());
  foreground_color_ =
      GetNotificationForegroundColor(background_color_, artwork_.bitmap());
  owner_->SchedulePaint();
}

void MediaNotificationBackground::UpdateCornerRadius(int top_radius,
                                                     int bottom_radius) {
  if (top_radius_ == top_radius && bottom_radius_ == bottom_radius)
    return;

  top_radius_ = top_radius;
  bottom_radius_ = bottom_radius;

  owner_->SchedulePaint();
}

void MediaNotificationBackground::UpdateArtworkMaxWidthPct(
    double max_width_pct) {
  if (artwork_max_width_pct_ == max_width_pct)
    return;

  artwork_max_width_pct_ = max_width_pct;
  owner_->SchedulePaint();
}

int MediaNotificationBackground::GetArtworkWidth(
    const gfx::Size& view_size) const {
  if (artwork_.isNull())
    return 0;

  // Calculate the aspect ratio of the image and determine what the width of the
  // image should be based on that ratio and the height of the notification.
  float aspect_ratio = (float)artwork_.width() / artwork_.height();
  return ceil(view_size.height() * aspect_ratio);
}

int MediaNotificationBackground::GetArtworkVisibleWidth(
    const gfx::Size& view_size) const {
  // The artwork should only take up a maximum percentage of the notification.
  return std::min(GetArtworkWidth(view_size),
                  (int)ceil(view_size.width() * artwork_max_width_pct_));
}

gfx::Rect MediaNotificationBackground::GetArtworkBounds(
    const gfx::Rect& view_bounds) const {
  int width = GetArtworkWidth(view_bounds.size());

  // The artwork should be positioned on the far right hand side of the
  // notification and be the same height.
  return owner_->GetMirroredRect(
      gfx::Rect(view_bounds.right() - width, 0, width, view_bounds.height()));
}

gfx::Rect MediaNotificationBackground::GetFilledBackgroundBounds(
    const gfx::Rect& view_bounds) const {
  // The filled background should take up the full notification except the area
  // taken up by the artwork.
  gfx::Rect bounds = gfx::Rect(view_bounds);
  bounds.Inset(0, 0, GetArtworkVisibleWidth(view_bounds.size()), 0);
  return owner_->GetMirroredRect(bounds);
}

gfx::Rect MediaNotificationBackground::GetGradientBounds(
    const gfx::Rect& view_bounds) const {
  if (artwork_.isNull())
    return gfx::Rect(0, 0, 0, 0);

  // The gradient should appear above the artwork on the left.
  return owner_->GetMirroredRect(gfx::Rect(
      view_bounds.width() - GetArtworkVisibleWidth(view_bounds.size()),
      view_bounds.y(), kMediaImageGradientWidth, view_bounds.height()));
}

SkPoint MediaNotificationBackground::GetGradientStartPoint(
    const gfx::Rect& draw_bounds) const {
  return gfx::PointToSkPoint(base::i18n::IsRTL() ? draw_bounds.right_center()
                                                 : draw_bounds.left_center());
}

SkPoint MediaNotificationBackground::GetGradientEndPoint(
    const gfx::Rect& draw_bounds) const {
  return gfx::PointToSkPoint(base::i18n::IsRTL() ? draw_bounds.left_center()
                                                 : draw_bounds.right_center());
}

}  // namespace ash
