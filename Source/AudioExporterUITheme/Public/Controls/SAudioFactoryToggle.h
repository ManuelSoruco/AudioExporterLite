// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Styling/AppStyle.h"

namespace AudioExporter {
DECLARE_DELEGATE_OneParam(FOnAudioFactoryToggleChanged, bool /* bNewChecked */);

/**
 * SAudioFactoryToggle
 *
 * Styled on/off toggle switch for AudioFactory plugin UIs.
 *
 * Visual: 44x22 pill track with a 16px sliding circle thumb.
 *         OFF  -- dark track (#242424), dim border, thumb left-inset 3px.
 *         ON   -- accent-tinted track, accent border, thumb right-inset 3px.
 *
 * AccentColor defaults to FAudioFactoryTheme::GetAccentPrimary().
 */
class AF_UI_API SAudioFactoryToggle : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryToggle)
		: _bChecked(false)
		, _AccentColor(FAudioFactoryTheme::GetAccentPrimary())
		, _ToggleWidth(FAudioFactoryConstants::ToggleWidth)
		, _ToggleHeight(FAudioFactoryConstants::ToggleHeight)
	{}
		SLATE_ARGUMENT(bool, bChecked)
		/** Fill color when checked. Defaults to palette AccentPrimary. */
		SLATE_ARGUMENT(FLinearColor, AccentColor)
		SLATE_ARGUMENT(float, ToggleWidth)
		SLATE_ARGUMENT(float, ToggleHeight)
		SLATE_EVENT(FOnAudioFactoryToggleChanged, OnToggleChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		bChecked        = InArgs._bChecked;
		AccentColor     = InArgs._AccentColor;
		ToggleWidth     = InArgs._ToggleWidth;
		ToggleHeight    = InArgs._ToggleHeight;
		OnToggleChanged = InArgs._OnToggleChanged;

		ChildSlot
		[
			SNew(SBox)
			.WidthOverride(ToggleWidth)
			.HeightOverride(ToggleHeight)
			[
				SAssignNew(ToggleButton, SButton)
				.ButtonStyle(FAppStyle::Get(), "NoBorder")
				.ContentPadding(0.0f)
				.OnClicked(FOnClicked::CreateSP(this, &SAudioFactoryToggle::HandleClicked))
			]
		];

		RebuildVisual();
	}

	void SetChecked(bool bNewChecked)
	{
		if (bChecked != bNewChecked)
		{
			bChecked = bNewChecked;
			RebuildVisual();
		}
	}

	bool IsChecked() const { return bChecked; }

	/**
	 * Invokes the same handler the nested button's click is bound to, so a caller exercises the real
	 * toggle path rather than a parallel one. This mirrors SButton::SimulateClick. It deliberately does
	 * NOT set the state directly: anything that skipped HandleClicked would also skip RebuildVisual and
	 * the OnToggleChanged broadcast, and would therefore pass while a real click was broken.
	 */
	void SimulateClick()
	{
		HandleClicked();
	}

private:
	FReply HandleClicked()
	{
		bChecked = !bChecked;
		RebuildVisual();
		OnToggleChanged.ExecuteIfBound(bChecked);
		return FReply::Handled();
	}

	// Renders a 44x22 pill track with a 16px sliding circle thumb.
	void RebuildVisual()
	{
		// -- Track fill --------------------------------------------------------
		const FLinearColor TrackFill = bChecked
			? FMath::Lerp(FAudioFactoryTheme::GetSurfaceRaised(), AccentColor, 0.7f)
			: FLinearColor(FColor(0x24, 0x24, 0x24));

		const FLinearColor TrackOutline = bChecked
			? AccentColor.CopyWithNewOpacity(0.8f)
			: FLinearColor(FColor(0x3C, 0x3C, 0x3C));

		// -- Thumb color -------------------------------------------------------
		const FLinearColor ThumbColor = bChecked
			? FLinearColor::White
			: FLinearColor(FColor(0xBB, 0xBB, 0xBB));

		// -- Brushes -----------------------------------------------------------
		TrackBrush = MakeUnique<FSlateRoundedBoxBrush>(
			TrackFill, FAudioFactoryConstants::ToggleRadius,
			TrackOutline, FAudioFactoryConstants::OutlineWidth);

		ThumbBrush = MakeUnique<FSlateRoundedBoxBrush>(
			ThumbColor, FAudioFactoryConstants::ToggleThumbRadius,
			ThumbColor.CopyWithNewOpacity(0.f), 0.f);

		if (TSharedPtr<SButton> Button = StaticCastSharedPtr<SButton>(ToggleButton))
		{
			Button->SetContent(
				SNew(SBox)
				.WidthOverride(ToggleWidth)
				.HeightOverride(ToggleHeight)
				[
					SNew(SOverlay)
					// -- Track (background pill) ------------------------------------
					+ SOverlay::Slot()
					[
						SNew(SBorder)
						.BorderImage(TrackBrush.Get())
						.Padding(0)
					]
					// -- Thumb (sliding circle) -------------------------------------
					+ SOverlay::Slot()
					.HAlign(bChecked ? HAlign_Right : HAlign_Left)
					.VAlign(VAlign_Center)
					.Padding(FMargin(
						bChecked ? 0.f : 3.f,  // left  -- 3px inset when OFF
						0.f,
						bChecked ? 3.f : 0.f,  // right -- 3px inset when ON
						0.f))
					[
						SNew(SBox)
						.WidthOverride(FAudioFactoryConstants::ToggleThumbSize)
						.HeightOverride(FAudioFactoryConstants::ToggleThumbSize)
						[
							SNew(SBorder)
							.BorderImage(ThumbBrush.Get())
							.Padding(0)
						]
					]
				]
			);
		}
	}

	bool         bChecked    = false;
	FLinearColor AccentColor;
	float        ToggleWidth  = FAudioFactoryConstants::ToggleWidth;
	float        ToggleHeight = FAudioFactoryConstants::ToggleHeight;
	FOnAudioFactoryToggleChanged OnToggleChanged;

	TSharedPtr<SWidget>               ToggleButton;
	TUniquePtr<FSlateRoundedBoxBrush> TrackBrush;
	TUniquePtr<FSlateRoundedBoxBrush> ThumbBrush;
};
} // namespace AudioExporter

