// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Controls/SAudioFactoryParamRow.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace AudioExporter {
class AF_UI_API SAudioFactoryPathPicker : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryPathPicker)
		: _Label(FText::GetEmpty())
		, _BrowseText(NSLOCTEXT("AudioFactoryPathPicker", "Browse", "Browse..."))
	{}
		SLATE_ATTRIBUTE(FText, Label)
		SLATE_ATTRIBUTE(FText, PathText)
		SLATE_ATTRIBUTE(FText, BrowseText)
		SLATE_EVENT(FOnClicked, OnBrowseClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		PathText = InArgs._PathText;
		OnBrowseClicked = InArgs._OnBrowseClicked;

		ChildSlot
		[
			SNew(SAudioFactoryParamRow)
			.Label(InArgs._Label)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, FAudioFactoryConstants::SpacingS, 0.0f)
				[
					SNew(STextBlock)
					.Text(PathText)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeSmall))
					.ColorAndOpacity(FSlateColor(FAudioFactoryTheme::GetDisabled()))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ContentPadding(FMargin(10.0f, 4.0f))
					.OnClicked(OnBrowseClicked)
					[
						SNew(STextBlock)
						.Text(InArgs._BrowseText)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeSmall))
						.ColorAndOpacity(FSlateColor(FAudioFactoryTheme::GetLabel()))
					]
				]
			]
		];
	}

private:
	TAttribute<FText> PathText;
	FOnClicked OnBrowseClicked;
};
} // namespace AudioExporter



