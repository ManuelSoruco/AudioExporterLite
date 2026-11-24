// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Controls/SAudioFactoryStatusStrip.h"

#include "Foundation/AudioFactoryStatusIcon.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
namespace AudioExporter {

void SAudioFactoryStatusStrip::Construct(const FArguments& InArgs)
{
	StatusAttribute = InArgs._Status;
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(this, &SAudioFactoryStatusStrip::GetStatusBackgroundColor)
		.Visibility(this, &SAudioFactoryStatusStrip::GetStatusVisibility)
		.Padding(0.0f)
		[
			SNew(SBox)
			.MinDesiredHeight(Tokens.StatusStripHeight)
			.Padding(FMargin(12.0f, 5.0f))
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(14.0f)
					.HeightOverride(14.0f)
					[
						SNew(SImage)
						.Image(this, &SAudioFactoryStatusStrip::GetStatusIconBrush)
						.ColorAndOpacity(this, &SAudioFactoryStatusStrip::GetStatusIconColor)
					]
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SAudioFactoryStatusStrip::GetStatusMessage)
					.ToolTipText(this, &SAudioFactoryStatusStrip::GetStatusToolTip)
					.ColorAndOpacity(this, &SAudioFactoryStatusStrip::GetStatusForegroundColor)
					.AutoWrapText(true)
				]
			]
		]
	];
}

EVisibility SAudioFactoryStatusStrip::GetStatusVisibility() const
{
	const FAudioFactoryStatus Status = StatusAttribute.Get();
	const bool bHasMessage = !Status.Message.IsEmpty();
	const bool bMeaningfulSeverity = Status.Severity != EAudioFactoryStatusSeverity::Neutral;
	return (bHasMessage && bMeaningfulSeverity) ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SAudioFactoryStatusStrip::GetStatusMessage() const
{
	return StatusAttribute.Get().Message;
}

FText SAudioFactoryStatusStrip::GetStatusToolTip() const
{
	const FAudioFactoryStatus Status = StatusAttribute.Get();
	return Status.ToolTip.IsEmpty() ? Status.Message : Status.ToolTip;
}

FSlateColor SAudioFactoryStatusStrip::GetStatusForegroundColor() const
{
	return FSlateColor(FAudioFactoryStyle::Get().PluginWindowTokens().TextPrimary);
}

FSlateColor SAudioFactoryStatusStrip::GetStatusBackgroundColor() const
{
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	const FLinearColor Severity = FAudioFactoryStatusIcon::SeverityColor(Tokens, StatusAttribute.Get().Severity);
	// Subtle severity-tinted band over the window surface so the strip reads as part of the frame.
	return FSlateColor(FMath::Lerp(Tokens.Surface, Severity, Tokens.StatusTintStrength));
}

const FSlateBrush* SAudioFactoryStatusStrip::GetStatusIconBrush() const
{
	return FAudioFactoryStatusIcon::Resolve(StatusAttribute.Get().Severity).Brush;
}

FSlateColor SAudioFactoryStatusStrip::GetStatusIconColor() const
{
	const FAudioFactoryStatusIcon::FStatusIconResolution Resolution =
		FAudioFactoryStatusIcon::Resolve(StatusAttribute.Get().Severity);
	if (!Resolution.bNeedsTint)
	{
		// "WithColor" icon variants are already baked-in tinted; re-tinting them would double up.
		return FSlateColor(FLinearColor::White);
	}

	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	return FSlateColor(FAudioFactoryStatusIcon::SeverityColor(Tokens, StatusAttribute.Get().Severity));
}
} // namespace AudioExporter
