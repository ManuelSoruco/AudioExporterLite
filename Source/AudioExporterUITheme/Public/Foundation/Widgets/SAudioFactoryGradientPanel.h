// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Brushes/SlateColorBrush.h"
#include "Rendering/DrawElementTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/**
 * SAudioFactoryGradientPanel
 *
 * VST-style background panel that draws a subtle vertical gradient
 * (slightly lighter at top, darker at bottom) behind child content.
 * The gradient is drawn via FSlateDrawElement::MakeGradient in OnPaint.
 *
 * Default colors sourced from FAudioFactoryTheme::GetPalette() GradientTop/Bottom.
 * An optional accent line can be drawn at the very top of the panel.
 */
namespace AudioExporter {
class AF_UI_API SAudioFactoryGradientPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryGradientPanel)
		: _TopColor(FAudioFactoryTheme::GetPalette().GradientTop)
		, _BottomColor(FAudioFactoryTheme::GetPalette().GradientBottom)
		, _CornerRadius(FAudioFactoryConstants::PanelRadius)
		, _AccentLineColor(FLinearColor::Transparent)
		, _AccentLineHeight(2.0f)
	{}
		/** Color at the top of the gradient (lighter -- lit from above). */
		SLATE_ARGUMENT(FLinearColor, TopColor)
		/** Color at the bottom of the gradient (darker). */
		SLATE_ARGUMENT(FLinearColor, BottomColor)
		/** Corner radius for the gradient box. */
		SLATE_ARGUMENT(float, CornerRadius)
		/** Optional accent line drawn at the very top of the panel. */
		SLATE_ARGUMENT(FLinearColor, AccentLineColor)
		/** Height of the optional accent line in pixels (default 2px). */
		SLATE_ARGUMENT(float, AccentLineHeight)
		/** Child content rendered on top of the gradient. */
		SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		TopColor         = InArgs._TopColor;
		BottomColor      = InArgs._BottomColor;
		CornerRadius     = InArgs._CornerRadius;
		AccentLineColor  = InArgs._AccentLineColor;
		AccentLineHeight = InArgs._AccentLineHeight;
		AccentBrush      = FSlateColorBrush(FLinearColor::White);

		ChildSlot
		[
			InArgs._Content.Widget
		];
	}

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		const FVector2f LocalSize = (FVector2f)AllottedGeometry.GetLocalSize();

		// Layer 0: Vertical gradient (top lighter -> bottom darker)
		TArray<FSlateGradientStop> GradientStops;
		GradientStops.Emplace(FVector2f(0.0f, 0.0f), TopColor);
		GradientStops.Emplace(FVector2f(LocalSize.X, LocalSize.Y), BottomColor);

		FSlateDrawElement::MakeGradient(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			MoveTemp(GradientStops),
			Orient_Vertical,
			ESlateDrawEffect::None,
			FVector4f(CornerRadius, CornerRadius, CornerRadius, CornerRadius));

		// Layer 1: Optional accent line at the top
		if (AccentLineColor.A > 0.0f)
		{
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId + 1,
				AllottedGeometry.ToPaintGeometry(
					FVector2f(LocalSize.X, AccentLineHeight),
					FSlateLayoutTransform(FVector2f(0.0f, 0.0f))),
				&AccentBrush,
				ESlateDrawEffect::None,
				AccentLineColor);
		}

		return SCompoundWidget::OnPaint(
			Args, AllottedGeometry, MyCullingRect, OutDrawElements,
			LayerId + 2, InWidgetStyle, bParentEnabled);
	}

private:
	FLinearColor TopColor;
	FLinearColor BottomColor;
	float        CornerRadius     = FAudioFactoryConstants::PanelRadius;
	FLinearColor AccentLineColor  = FLinearColor::Transparent;
	float        AccentLineHeight = 2.0f;

	// Pre-built in Construct to avoid hot-path allocation in OnPaint.
	FSlateColorBrush AccentBrush{ FLinearColor::White };
};
} // namespace AudioExporter

