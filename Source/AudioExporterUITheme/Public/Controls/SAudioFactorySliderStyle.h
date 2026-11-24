// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/SlateTypes.h"

/**
 * FAudioFactorySliderStyle
 *
 * Header-only FSliderStyle factory for AudioFactory plugin UIs.
 * Returns an FSliderStyle configured with the active theme palette.
 *
 * Usage:
 * @code
 *   static const FSliderStyle Style = FAudioFactorySliderStyle::Make();
 *   SNew(SSlider).Style(&Style).Value(MyValue).OnValueChanged(...);
 * @endcode
 */
namespace AudioExporter {
namespace FAudioFactorySliderStyle
{
	/**
	 * Create an AudioFactory-styled FSliderStyle.
	 * @param FillColor   Accent / fill color. Defaults to palette AccentPrimary.
	 * @param TrackColor  Unfilled track color. Defaults to SurfaceLight.
	 */
	inline FSliderStyle Make(
		const FLinearColor& FillColor  = FAudioFactoryTheme::GetAccentPrimary(),
		const FLinearColor& TrackColor = FAudioFactoryTheme::GetDivider())
	{
		FSliderStyle Style;

		// -- Track (bar image) --------------------------------------------------
		// FSlateRoundedBoxBrush gives the track a defined desired size (vs FSlateColorBrush
		// which resolves to 0x0 and produces an invisible bar at some viewport scales).
		FSlateRoundedBoxBrush TrackBrushNormal(TrackColor, 1.5f);
		FSlateRoundedBoxBrush TrackBrushDisabled(FAudioFactoryTheme::GetDisabled(), 1.5f);
		Style.SetNormalBarImage(TrackBrushNormal);
		Style.SetHoveredBarImage(TrackBrushNormal);
		Style.SetDisabledBarImage(TrackBrushDisabled);

		// -- Thumb (handle) -----------------------------------------------------
		// FSlateColorBrush has no explicit ImageSize -- SSlider uses the brush desired size
		// to size the thumb widget, yielding a 0px invisible handle. FSlateRoundedBoxBrush
		// with an explicit ImageSize renders the correct circle.
		FSlateRoundedBoxBrush ThumbNormal(FillColor, 6.0f);
		ThumbNormal.ImageSize = FVector2D(12.0f, 12.0f);

		FSlateRoundedBoxBrush ThumbHover(FAudioFactoryTheme::GetAccentSecondary(), 6.0f);
		ThumbHover.ImageSize = FVector2D(14.0f, 14.0f);

		FSlateRoundedBoxBrush ThumbDisabled(FAudioFactoryTheme::GetDisabled(), 6.0f);
		ThumbDisabled.ImageSize = FVector2D(12.0f, 12.0f);

		Style.SetNormalThumbImage(ThumbNormal);
		Style.SetHoveredThumbImage(ThumbHover);
		Style.SetDisabledThumbImage(ThumbDisabled);

		Style.BarThickness = 3.0f;

		return Style;
	}
}
} // namespace AudioExporter

