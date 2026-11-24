// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Controls/SAudioFactoryTaskLogView.h"
#include "Foundation/AudioFactoryPluginWindowTypes.h"
#include "Styling/SlateTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SProgressBar;
class STextBlock;

#ifndef AF_UI_API
#define AF_UI_API
#endif

/**
 * Progress body for secondary windows: left-aligned headline in
 * DetailsView.CategoryFontStyle, muted sub-line, 4px engine-styled progress bar
 * (no accent tint, fill animation disabled), then the native task log view.
 * An unset ProgressFraction leaves the engine marquee visible (indeterminate).
 */
namespace AudioExporter {
class AF_UI_API SAudioFactoryProgressBody : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryProgressBody)
		: _Headline(FText::GetEmpty())
	{
	}
		SLATE_ARGUMENT(FText, Headline)
		SLATE_ATTRIBUTE(FText, Subline)
		SLATE_ATTRIBUTE(TOptional<float>, ProgressFraction)
		SLATE_ARGUMENT(const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>>*, LogItems)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetSubline(TAttribute<FText> InSubline);
	void SetProgressFraction(TAttribute<TOptional<float>> InProgressFraction);
	void SetLogItems(const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>>* InLogItems);
	void RequestScrollToBottom();

private:
	TAttribute<FText> SublineAttribute;
	TAttribute<TOptional<float>> ProgressFractionAttribute;
	FProgressBarStyle ProgressBarStyle;
	TSharedPtr<STextBlock> SublineText;
	TSharedPtr<SProgressBar> ProgressBar;
	TSharedPtr<SAudioFactoryTaskLogView> LogView;
};
} // namespace AudioExporter
