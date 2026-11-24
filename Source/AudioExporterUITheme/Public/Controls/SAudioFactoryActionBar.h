// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Foundation/AudioFactoryStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"

namespace AudioExporter {
class AF_UI_API SAudioFactoryActionBar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryActionBar)
	{}
		SLATE_NAMED_SLOT(FArguments, LeftContent)
		SLATE_NAMED_SLOT(FArguments, RightContent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		const FAudioFactoryTokens& Tokens = FAudioFactoryStyle::Get().Tokens();

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FAudioFactoryTheme::GetDivider())
			.Padding(FMargin(0.0f, 1.0f, 0.0f, 0.0f))
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("NoBrush"))
				.Padding(FMargin(Tokens.Spacing.M, Tokens.Spacing.S, Tokens.Spacing.M, Tokens.Spacing.M))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						InArgs._LeftContent.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._LeftContent.Widget
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						InArgs._RightContent.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._RightContent.Widget
					]
				]
			]
		];
	}
};
} // namespace AudioExporter


