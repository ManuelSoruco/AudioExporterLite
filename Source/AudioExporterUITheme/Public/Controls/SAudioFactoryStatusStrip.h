// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryPluginWindowTypes.h"
#include "Foundation/AudioFactoryStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

#ifndef AF_UI_API
#define AF_UI_API
#endif

/**
 * Status / validation strip rendered by the window frame. Collapsed unless the
 * bound Status carries a non-empty message with a non-Neutral severity. Shared
 * by secondary windows so they can host the same strip without the full
 * frame; StatusTintStrength stays 0.10 (WCAG gate).
 */
namespace AudioExporter {
class AF_UI_API SAudioFactoryStatusStrip : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryStatusStrip)
	{
	}
		SLATE_ATTRIBUTE(FAudioFactoryStatus, Status)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// Status-prefixed names so none of these hide SWidget/SCompoundWidget virtuals.
	EVisibility GetStatusVisibility() const;
	FText GetStatusMessage() const;
	FText GetStatusToolTip() const;
	FSlateColor GetStatusForegroundColor() const;
	FSlateColor GetStatusBackgroundColor() const;
	const FSlateBrush* GetStatusIconBrush() const;
	FSlateColor GetStatusIconColor() const;

	TAttribute<FAudioFactoryStatus> StatusAttribute;
};
} // namespace AudioExporter
