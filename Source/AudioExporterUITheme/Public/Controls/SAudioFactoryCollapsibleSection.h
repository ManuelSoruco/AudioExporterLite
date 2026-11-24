// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Controls/IAudioFactoryWindowHost.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

/**
 * SAudioFactoryCollapsibleSection
 *
 * A panel card whose content can be collapsed/expanded by clicking the header.
 *
 * If WindowHost and HeightOffset are set, toggling automatically resizes the
 * parent window by +/-HeightOffset logical pixels.
 *
 * Resizes the parent window through the generic IAudioFactoryWindowHost interface.
 */
namespace AudioExporter {
class AF_UI_API SAudioFactoryCollapsibleSection : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryCollapsibleSection)
		: _SectionLabel(FText::GetEmpty())
		, _bDefaultExpanded(false)
		, _bDefaultCollapsed(false)
		, _bUpperCaseLabel(false)   // optional ALL-CAPS rendering for section headers
		, _WindowHost(nullptr)
		, _HeightOffset(0.f)
	{}
		SLATE_ARGUMENT(FText, SectionLabel)
		SLATE_ARGUMENT(bool, bDefaultExpanded)
		SLATE_ARGUMENT(bool, bDefaultCollapsed)
		/** When true, renders the section label in ALL CAPS. Default: false. */
		SLATE_ARGUMENT(bool, bUpperCaseLabel)
		SLATE_DEFAULT_SLOT(FArguments, Content)
		/** Called when the section is toggled. Passes the new expanded state. */
		SLATE_EVENT(FSimpleDelegate, OnToggled)
		/**
		 * Optional: pointer to the hosting window (implements IAudioFactoryWindowHost).
		 * When set with HeightOffset > 0, toggling auto-resizes the window.
		 * Pass nullptr when no auto-resize is needed.
		 */
		SLATE_ARGUMENT(IAudioFactoryWindowHost*, WindowHost)
		/** Logical pixel delta for window resize on expand (+) / collapse (-). */
		SLATE_ARGUMENT(float, HeightOffset)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		bExpanded         = InArgs._bDefaultCollapsed ? false : InArgs._bDefaultExpanded;
		OnToggledDelegate = InArgs._OnToggled;
		WindowHost        = InArgs._WindowHost;
		HeightOffset      = InArgs._HeightOffset;

		// Uppercase label transform.
		const FText DisplayLabel = InArgs._bUpperCaseLabel
			? FText::FromString(InArgs._SectionLabel.ToString().ToUpper())
			: InArgs._SectionLabel;

		const FAudioFactoryPalette& P = FAudioFactoryTheme::GetPalette();
		CardBrush = MakeUnique<FSlateRoundedBoxBrush>(
			P.CardFill, FAudioFactoryConstants::CardRadius,
			P.CardOutline, FAudioFactoryConstants::OutlineWidth);

		ContentWidget = InArgs._Content.Widget;

		ChildSlot
		[
			SNew(SBox)
			// Full SectionGap, not halved -- gives cards room to breathe.
			.Padding(FMargin(0.0f, FAudioFactoryConstants::SectionGap, 0.0f, 0.0f))
			[
				SNew(SBorder)
				.BorderImage(CardBrush.Get())
				// Card padding: SectionPadding on left/right/bottom, SpacingM on top (below the header row).
				.Padding(FMargin(
					FAudioFactoryConstants::SectionPadding, FAudioFactoryConstants::SpacingM,
					FAudioFactoryConstants::SectionPadding, FAudioFactoryConstants::SectionPadding))
				[
					SNew(SVerticalBox)

					// Header row: label (left) + chevron (right)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, bExpanded ? FAudioFactoryConstants::SpacingS : 0.0f)
					[
						// SBox wrapper gives the header a 32px minimum hit target.
						SNew(SBox)
						.HeightOverride(FAudioFactoryConstants::SectionHeaderHeight)
						[
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "NoBorder")
							.ContentPadding(FMargin(0.f))
							.Cursor(EMouseCursor::Hand)
							.OnClicked(this, &SAudioFactoryCollapsibleSection::OnHeaderClicked)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.FillWidth(1.0f)
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(DisplayLabel)
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", FAudioFactoryConstants::FontSizeSection))
									.ColorAndOpacity(FSlateColor(FAudioFactoryTheme::GetSectionLabel()))
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								[
									SNew(SBox)
									.WidthOverride(10.f)
									.HeightOverride(10.f)
									[
										SNew(SImage)
										.Image_Lambda([this]() -> const FSlateBrush*
										{
											return bExpanded
												? FAppStyle::GetBrush("Icons.ChevronDown")
												: FAppStyle::GetBrush("Icons.ChevronRight");
										})
										.ColorAndOpacity(FSlateColor(FAudioFactoryTheme::GetSectionLabel()))
									]
								]
							]
						]
					]

					// Collapsible content
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SBox)
						.Visibility_Lambda([this]() -> EVisibility
						{
							return bExpanded ? EVisibility::Visible : EVisibility::Collapsed;
						})
						[
							ContentWidget.ToSharedRef()
						]
					]
				]
			]
		];
	}

	bool IsExpanded() const { return bExpanded; }

private:
	FReply OnHeaderClicked()
	{
		bExpanded = !bExpanded;
		Invalidate(EInvalidateWidgetReason::Layout);

		if (HeightOffset > 0.f && WindowHost != nullptr)
		{
			WindowHost->ResizeWindowByDelta(bExpanded ? HeightOffset : -HeightOffset);
		}

		OnToggledDelegate.ExecuteIfBound();
		return FReply::Handled();
	}

	bool                              bExpanded       = false;
	FSimpleDelegate                   OnToggledDelegate;
	IAudioFactoryWindowHost*          WindowHost      = nullptr;
	float                             HeightOffset    = 0.f;
	TSharedPtr<SWidget>               ContentWidget;
	TUniquePtr<FSlateRoundedBoxBrush> CardBrush;
};
} // namespace AudioExporter

