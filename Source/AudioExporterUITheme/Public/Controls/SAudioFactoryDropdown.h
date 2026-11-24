// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

namespace AudioExporter {
DECLARE_DELEGATE_OneParam(FOnAudioFactoryDropdownSelectionChanged, int32 /* NewIndex */);

/**
 * SAudioFactoryDropdown
 *
 * Fully themed dropdown built on SMenuAnchor + custom popup panel.
 * Rounded corners, dark background, hover highlights -- consistent with AudioFactory design.
 *
 * AccentColor defaults to FAudioFactoryTheme::GetAccentPrimary().
 * No PropertyAdapter dependency -- use SelectedIndex attribute + OnSelectionChanged delegate.
 */
class AF_UI_API SAudioFactoryDropdown : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryDropdown)
		: _AccentColor(FAudioFactoryTheme::GetAccentPrimary())
		, _SelectedIndex(0)
		, _MinWidth(0.f)
	{}
		SLATE_ARGUMENT(TArray<FText>, Options)
		/** Accent color for selected-row tinting. Defaults to palette AccentPrimary. */
		SLATE_ARGUMENT(FLinearColor, AccentColor)
		SLATE_ATTRIBUTE(int32, SelectedIndex)
		SLATE_EVENT(FOnAudioFactoryDropdownSelectionChanged, OnSelectionChanged)
		/** Minimum width of the trigger button (0 = auto). Prevents layout reshuffling. */
		SLATE_ARGUMENT(float, MinWidth)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Options            = InArgs._Options;
		AccentColor        = InArgs._AccentColor;
		SelectedIndex      = InArgs._SelectedIndex;
		OnSelectionChanged = InArgs._OnSelectionChanged;
		MinWidth           = InArgs._MinWidth;

		const FLinearColor TriggerBg(FColor(0x0E, 0x0E, 0x0E));
		const FLinearColor TriggerHoverBg(FColor(0x1A, 0x1A, 0x1A));
		const FLinearColor TriggerOutline(FColor(0x2A, 0x2A, 0x2A));
		const FLinearColor TriggerHoverOutline(FColor(0x3A, 0x3A, 0x3A));

		BodyBrush = MakeUnique<FSlateRoundedBoxBrush>(
			TriggerBg, FAudioFactoryConstants::ButtonRadius, TriggerOutline, FAudioFactoryConstants::OutlineWidth);

		BodyHoverBrush = MakeUnique<FSlateRoundedBoxBrush>(
			TriggerHoverBg, FAudioFactoryConstants::ButtonRadius, TriggerHoverOutline, FAudioFactoryConstants::OutlineWidth);

		PopupBrush = MakeUnique<FSlateRoundedBoxBrush>(
			FAudioFactoryTheme::GetPanelBG(),
			6.0f,
			FMath::Lerp(FAudioFactoryTheme::GetDivider(), AccentColor, 0.25f),
			1.0f);

		const FSlateBrush* NormalBrushPtr = BodyBrush.Get();
		const FSlateBrush* HoverBrushPtr  = BodyHoverBrush.Get();

		TSharedRef<SMenuAnchor> Anchor = SAssignNew(MenuAnchor, SMenuAnchor)
			.Placement(MenuPlacement_ComboBox)
			.OnGetMenuContent(this, &SAudioFactoryDropdown::BuildPopupContent)
			[
				SAssignNew(TriggerBorder, SBorder)
				.BorderImage(NormalBrushPtr)
				.Padding(FMargin(0.f))
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "NoBorder")
					.ContentPadding(FMargin(7.f, 4.f))
					.OnClicked_Lambda([this]() -> FReply { TogglePopup(); return FReply::Handled(); })
					.OnHovered_Lambda([this, HoverBrushPtr]()
					{
						if (TriggerBorder.IsValid()) TriggerBorder->SetBorderImage(HoverBrushPtr);
					})
					.OnUnhovered_Lambda([this, NormalBrushPtr]()
					{
						if (TriggerBorder.IsValid()) TriggerBorder->SetBorderImage(NormalBrushPtr);
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(this, &SAudioFactoryDropdown::GetSelectedText)
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeSection))
							.ColorAndOpacity(FAudioFactoryTheme::GetValue())
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(4.f, 0.f, 0.f, 0.f)
						[
							SNew(SBox)
							.WidthOverride(8.f)
							.HeightOverride(8.f)
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("Icons.ChevronDown"))
								.ColorAndOpacity(FAudioFactoryTheme::GetDisabled())
							]
						]
					]
				]
			];

		if (MinWidth > 0.f)
		{
			ChildSlot
			[
				SNew(SBox).MinDesiredWidth(MinWidth)[ Anchor ]
			];
		}
		else
		{
			ChildSlot[ Anchor ];
		}
	}

	void SetSelectedIndex(int32 NewIndex) { SelectedIndex = TAttribute<int32>(NewIndex); }
	int32 GetSelectedIndex() const { return SelectedIndex.Get(INDEX_NONE); }
	FText GetSelectedText() const
	{
		const int32 Idx = GetSelectedIndex();
		return Options.IsValidIndex(Idx) ? Options[Idx] : FText::GetEmpty();
	}

private:
	TSharedRef<SWidget> BuildPopupContent()
	{
		PopupRowBrushes.Empty(Options.Num());
		TSharedRef<SVerticalBox> RowBox = SNew(SVerticalBox);
		const int32 CurrentSel = GetSelectedIndex();

		for (int32 Index = 0; Index < Options.Num(); ++Index)
		{
			const bool bIsSelected = (Index == CurrentSel);
			const FLinearColor RowFill = bIsSelected
				? FMath::Lerp(FAudioFactoryTheme::GetSectionBackground(), AccentColor, 0.18f)
				: FAudioFactoryTheme::GetSectionBackground();
			const FLinearColor TextCol = bIsSelected
				? FMath::Lerp(FAudioFactoryTheme::GetValue(), AccentColor, 0.4f)
				: FAudioFactoryTheme::GetValue();

			TSharedPtr<FSlateRoundedBoxBrush> RowBrush = MakeShared<FSlateRoundedBoxBrush>(RowFill, 4.f);
			PopupRowBrushes.Add(RowBrush);
			const FSlateBrush* RowBrushPtr = RowBrush.Get();
			const int32 CapturedIndex = Index;

			RowBox->AddSlot().AutoHeight().Padding(3.f, 2.f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "NoBorder")
				.ContentPadding(FMargin(0.f))
				.OnClicked_Lambda([this, CapturedIndex]() -> FReply
				{
					SelectOption(CapturedIndex);
					return FReply::Handled();
				})
				[
					SNew(SBorder)
					.BorderImage(RowBrushPtr)
					.Padding(FMargin(10.f, 5.f))
					[
						SNew(STextBlock)
						.Text(Options[Index])
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeSection))
						.ColorAndOpacity(TextCol)
					]
				]
			];
		}

		return SNew(SBorder)
			.BorderImage(PopupBrush.Get())
			.Padding(FMargin(0.f, 2.f))
			[
				SNew(SBox).MinDesiredWidth(80.f)[ RowBox ]
			];
	}

	void TogglePopup()
	{
		if (MenuAnchor.IsValid())
		{
			MenuAnchor->SetIsOpen(!MenuAnchor->IsOpen(), !MenuAnchor->IsOpen());
		}
	}

	void SelectOption(int32 Index)
	{
		if (MenuAnchor.IsValid()) MenuAnchor->SetIsOpen(false);
		if (Index != GetSelectedIndex())
		{
			SelectedIndex = TAttribute<int32>(Index);
			OnSelectionChanged.ExecuteIfBound(Index);
		}
	}

	TArray<FText>        Options;
	FLinearColor         AccentColor;
	TAttribute<int32>    SelectedIndex;
	FOnAudioFactoryDropdownSelectionChanged OnSelectionChanged;
	float                MinWidth = 0.f;

	TSharedPtr<SMenuAnchor>  MenuAnchor;
	TSharedPtr<SBorder>      TriggerBorder;
	TUniquePtr<FSlateRoundedBoxBrush> BodyBrush;
	TUniquePtr<FSlateRoundedBoxBrush> BodyHoverBrush;
	TUniquePtr<FSlateRoundedBoxBrush> PopupBrush;
	TArray<TSharedPtr<FSlateRoundedBoxBrush>> PopupRowBrushes;
};
} // namespace AudioExporter

