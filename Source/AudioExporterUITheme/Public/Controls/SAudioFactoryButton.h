// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

namespace AudioExporter {
DECLARE_DELEGATE_OneParam(FOnAudioFactoryButtonSelectionChanged, int32 /* NewIndex */);

/**
 * SAudioFactoryButton
 *
 * Styled rounded toggle button -- primary interactive button primitive for all AudioFactory plugin UIs.
 *
 * Single-option mode (most common):
 *   Pass one entry in Options. Acts as a boolean toggle:
 *   clicking an active button turns it OFF (SelectedIndex -> -1),
 *   clicking an inactive button turns it ON (SelectedIndex -> 0).
 *
 * Multi-option mode (radio group):
 *   Pass 2-4 entries. Only one active at a time. Clicking active does nothing.
 *
 * AccentColor defaults to FAudioFactoryTheme::GetAccentPrimary().
 * No PropertyAdapter dependency -- use SelectedIndex attribute + OnSelectionChanged delegate.
 */
class AF_UI_API SAudioFactoryButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryButton)
		: _AccentColor(FAudioFactoryTheme::GetAccentPrimary())
		, _SelectedIndex(0)
	{}
		/** Labels for each button option. */
		SLATE_ARGUMENT(TArray<FText>, Options)
		/** Accent color for active-state button tinting. Defaults to palette AccentPrimary. */
		SLATE_ARGUMENT(FLinearColor, AccentColor)
		/** Currently selected index (-1 = none active). Supports TAttribute binding. */
		SLATE_ATTRIBUTE(int32, SelectedIndex)
		/** Fired when selection changes. */
		SLATE_EVENT(FOnAudioFactoryButtonSelectionChanged, OnSelectionChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Options           = InArgs._Options;
		AccentColor       = InArgs._AccentColor;
		SelectedIndex     = InArgs._SelectedIndex;
		OnSelectionChanged = InArgs._OnSelectionChanged;

		ChildSlot
		[
			SAssignNew(ButtonBox, SHorizontalBox)
		];

		RebuildButtons();
	}

	/** Set the selected index programmatically (rebuilds button visuals). */
	void SetSelectedIndex(int32 NewIndex)
	{
		SelectedIndex = TAttribute<int32>(NewIndex);
		RebuildButtons();
	}

private:
	/** Weak handles the per-option hover delegates use to reach their own border and label. */
	struct FHoverTargets
	{
		TWeakPtr<SBorder>    Border;
		TWeakPtr<STextBlock> Text;
	};

	void RebuildButtons()
	{
		ButtonBox->ClearChildren();
		ButtonBrushes.Empty();
		HoverBrushes.Empty();

		const int32 CurrentSelection = SelectedIndex.Get(0);

		for (int32 Index = 0; Index < Options.Num(); ++Index)
		{
			const bool bIsActive = (Index == CurrentSelection);

			const FLinearColor BgColor = bIsActive
				? FMath::Lerp(FAudioFactoryTheme::GetSurfaceRaised(), AccentColor, 0.65f)
				: FLinearColor(FColor(0x14, 0x14, 0x14));  // slightly warmer dark

			const FLinearColor OutlineColor = bIsActive
				? AccentColor
				: FLinearColor(FColor(0x1A, 0x1A, 0x1A));

			const FLinearColor HoverBgColor(
				FMath::Min(BgColor.R * 1.3f + 0.03f, 1.0f),
				FMath::Min(BgColor.G * 1.3f + 0.03f, 1.0f),
				FMath::Min(BgColor.B * 1.3f + 0.03f, 1.0f),
				1.0f);

			const FLinearColor HoverOutlineColor = bIsActive
				? AccentColor
				: FLinearColor(
					FMath::Min(OutlineColor.R * 1.5f + 0.05f, 1.0f),
					FMath::Min(OutlineColor.G * 1.5f + 0.05f, 1.0f),
					FMath::Min(OutlineColor.B * 1.5f + 0.05f, 1.0f),
					1.0f);

			const FLinearColor TextColor      = bIsActive ? FAudioFactoryTheme::GetValue() : FLinearColor(FColor(0x88, 0x88, 0x88));
			const FLinearColor HoverTextColor = bIsActive ? FAudioFactoryTheme::GetValue() : FLinearColor(FColor(0x99, 0x99, 0x99));

			TUniquePtr<FSlateRoundedBoxBrush>& NormalBrush = ButtonBrushes.Emplace_GetRef(
				MakeUnique<FSlateRoundedBoxBrush>(BgColor, FAudioFactoryConstants::ButtonRadius, OutlineColor, FAudioFactoryConstants::OutlineWidth));

			TUniquePtr<FSlateRoundedBoxBrush>& HoverBrush = HoverBrushes.Emplace_GetRef(
				MakeUnique<FSlateRoundedBoxBrush>(HoverBgColor, FAudioFactoryConstants::ButtonRadius, HoverOutlineColor, FAudioFactoryConstants::OutlineWidth));

			const FSlateBrush* NormalBrushPtr = NormalBrush.Get();
			const FSlateBrush* HoverBrushPtr  = HoverBrush.Get();

			TSharedPtr<SBorder>    ItemBorder;
			TSharedPtr<STextBlock> ItemText;

			// The hover handlers live on the child button but repaint the parent border and its
			// text, so they reach both through this shared record of WEAK pointers, filled in once
			// the declaration below has finished building. Two reasons, both load-bearing:
			//  * A strong capture would be a parent<->child reference cycle -- the delegate is
			//    stored on the border's own child -- and RebuildButtons() runs on every click, so
			//    each rebuild would leak a border plus the brushes it points at. Same weak-capture
			//    rule the window frame documents (SAudioFactoryPluginWindowFrame.cpp).
			//  * SAssignNew only assigns its exposed pointer after the whole declaration, child
			//    content included, has been built, so capturing ItemBorder/ItemText here by value
			//    would capture null and the hover state would never apply at all.
			const TSharedRef<FHoverTargets> HoverTargets = MakeShared<FHoverTargets>();

			ButtonBox->AddSlot()
				.AutoWidth()
				.Padding(Index > 0 ? FAudioFactoryConstants::ButtonGroupSpacing : 0.0f, 0.0f, 0.0f, 0.0f)
				[
					SAssignNew(ItemBorder, SBorder)
					.BorderImage(NormalBrushPtr)
					.Padding(FMargin(0.0f))
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "NoBorder")
						.ContentPadding(FMargin(12.0f, 6.0f))  // more generous hit area
						.OnClicked(FOnClicked::CreateSP(this, &SAudioFactoryButton::OnButtonClicked, Index))
						.OnHovered_Lambda([HoverTargets, HoverBrushPtr, HoverTextColor]()
						{
							if (const TSharedPtr<SBorder> Border = HoverTargets->Border.Pin())  Border->SetBorderImage(HoverBrushPtr);
							if (const TSharedPtr<STextBlock> Text = HoverTargets->Text.Pin())   Text->SetColorAndOpacity(HoverTextColor);
						})
						.OnUnhovered_Lambda([HoverTargets, NormalBrushPtr, TextColor]()
						{
							if (const TSharedPtr<SBorder> Border = HoverTargets->Border.Pin())  Border->SetBorderImage(NormalBrushPtr);
							if (const TSharedPtr<STextBlock> Text = HoverTargets->Text.Pin())   Text->SetColorAndOpacity(TextColor);
						})
						[
							SAssignNew(ItemText, STextBlock)
							.Text(Options[Index])
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeBody))
							.ColorAndOpacity(TextColor)
						]
					]
				];

			HoverTargets->Border = ItemBorder;
			HoverTargets->Text   = ItemText;
		}
	}

	FReply OnButtonClicked(int32 Index)
	{
		const int32 Current = SelectedIndex.Get(0);
		int32 NewSelection = Current;

		if (Options.Num() == 1)
		{
			NewSelection = (Current == Index) ? -1 : Index;
		}
		else if (Index != Current)
		{
			NewSelection = Index;
		}

		if (NewSelection != Current)
		{
			SelectedIndex = TAttribute<int32>(NewSelection);
			RebuildButtons();
			OnSelectionChanged.ExecuteIfBound(NewSelection);
		}

		return FReply::Handled();
	}

	TArray<FText>        Options;
	FLinearColor         AccentColor = FLinearColor::White;
	TAttribute<int32>    SelectedIndex;
	FOnAudioFactoryButtonSelectionChanged OnSelectionChanged;

	TSharedPtr<SHorizontalBox> ButtonBox;
	TArray<TUniquePtr<FSlateRoundedBoxBrush>> ButtonBrushes;
	TArray<TUniquePtr<FSlateRoundedBoxBrush>> HoverBrushes;
};
} // namespace AudioExporter

