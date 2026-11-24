// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryStyle.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Styling/CoreStyle.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace AudioExporter {
class AF_UI_API SAudioFactoryParamRow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryParamRow)
		: _Label(FText::GetEmpty())
		, _HelpText(FText::GetEmpty())
		, _LabelWidth(FAudioFactoryStyle::Get().Tokens().Columns.Label)
	{}
		SLATE_ATTRIBUTE(FText, Label)
		SLATE_ATTRIBUTE(FText, HelpText)
		SLATE_ARGUMENT(float, LabelWidth)
		SLATE_DEFAULT_SLOT(FArguments, Control)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		const FAudioFactoryTokens& Tokens = FAudioFactoryStyle::Get().Tokens();

		ChildSlot
		[
			// MinDesiredHeight ensures every param row is at least 30px tall.
			// HelpText renders as a tooltip, not inline visible text.
			SNew(SBox)
			.MinDesiredHeight(FAudioFactoryConstants::ParamRowMinHeight)
			.ToolTipText(InArgs._HelpText)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, Tokens.Spacing.S, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(InArgs._LabelWidth)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(InArgs._Label)
						// FontSizeBody, for legibility.
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeBody))
						.ColorAndOpacity(FSlateColor(FAudioFactoryTheme::GetLabel()))
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					InArgs._Control.Widget
				]
			]
		];
	}
};
} // namespace AudioExporter
