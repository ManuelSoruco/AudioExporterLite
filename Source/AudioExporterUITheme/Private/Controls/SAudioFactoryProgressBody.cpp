// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Controls/SAudioFactoryProgressBody.h"

#include "Foundation/AudioFactoryStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
namespace AudioExporter {

namespace AudioFactoryProgressBodyPrivate
{
	constexpr float ProgressBarHeight = 4.0f;
}

void SAudioFactoryProgressBody::Construct(const FArguments& InArgs)
{
	SublineAttribute = InArgs._Subline;
	ProgressFractionAttribute = InArgs._ProgressFraction;

	// Engine-styled bar, accent-free: the stock "ProgressBar" style with fill
	// animation disabled (no idle animation per the locked UI constraints). No
	// FillColorAndOpacity is set, so the fill stays the engine default and an
	// unset ProgressFraction shows the engine marquee.
	ProgressBarStyle = FAppStyle::Get().GetWidgetStyle<FProgressBarStyle>("ProgressBar");
	ProgressBarStyle.SetEnableFillAnimation(false);

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(STextBlock)
			.Text(InArgs._Headline)
			.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			.ColorAndOpacity(FSlateColor(FAudioFactoryStyle::Get().PluginWindowTokens().TextPrimary))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SAssignNew(SublineText, STextBlock)
			.Text(SublineAttribute)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor(FAudioFactoryStyle::Get().PluginWindowTokens().TextMuted))
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBox)
			.HeightOverride(AudioFactoryProgressBodyPrivate::ProgressBarHeight)
			[
				SAssignNew(ProgressBar, SProgressBar)
				.Style(&ProgressBarStyle)
				.Percent(ProgressFractionAttribute)
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(LogView, SAudioFactoryTaskLogView)
			.LogItems(InArgs._LogItems)
		]
	];
}

void SAudioFactoryProgressBody::SetSubline(TAttribute<FText> InSubline)
{
	SublineAttribute = MoveTemp(InSubline);
	if (SublineText.IsValid())
	{
		SublineText->SetText(SublineAttribute);
	}
}

void SAudioFactoryProgressBody::SetProgressFraction(TAttribute<TOptional<float>> InProgressFraction)
{
	ProgressFractionAttribute = MoveTemp(InProgressFraction);
	if (ProgressBar.IsValid())
	{
		ProgressBar->SetPercent(ProgressFractionAttribute);
	}
}

void SAudioFactoryProgressBody::SetLogItems(const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>>* InLogItems)
{
	if (LogView.IsValid())
	{
		LogView->SetLogItems(InLogItems);
	}
}

void SAudioFactoryProgressBody::RequestScrollToBottom()
{
	if (LogView.IsValid())
	{
		LogView->RequestScrollToBottom();
	}
}
} // namespace AudioExporter
