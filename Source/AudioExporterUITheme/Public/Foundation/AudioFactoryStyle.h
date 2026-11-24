// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryConstants.h"
#include "Styling/StyleColors.h"

#ifndef AF_UI_API
#define AF_UI_API
#endif

namespace AudioExporter {
struct FAudioFactorySpacingTokens
{
	float XS = FAudioFactoryConstants::SpacingXS;
	float S = FAudioFactoryConstants::SpacingS;
	float M = FAudioFactoryConstants::SpacingM;
	float L = FAudioFactoryConstants::SpacingL;
	float XL = FAudioFactoryConstants::SpacingXL;
};

struct FAudioFactoryRadiusTokens
{
	float S = FAudioFactoryConstants::RadiusS;
	float M = FAudioFactoryConstants::RadiusM;
	float L = FAudioFactoryConstants::RadiusL;
};

struct FAudioFactoryFontTokens
{
	int32 XS = FAudioFactoryConstants::FontSizeXS;
	int32 SM = FAudioFactoryConstants::FontSizeSM;
	int32 MD = FAudioFactoryConstants::FontSizeMD;
	int32 LG = FAudioFactoryConstants::FontSizeLG;
	int32 XL = FAudioFactoryConstants::FontSizeXL;
};

struct FAudioFactoryColumnTokens
{
	float Label = FAudioFactoryConstants::LabelColumnWidth;
	float Help = FAudioFactoryConstants::HelpColumnWidth;
};

struct FAudioFactoryWindowTokenRange
{
	FVector2D Min = FVector2D(FAudioFactoryConstants::SettingsWindowMinWidth, 0.0f);
	FVector2D Max = FVector2D(FAudioFactoryConstants::SettingsWindowMaxWidth, 0.0f);
};

struct FAudioFactoryWindowTokens
{
	FAudioFactoryWindowTokenRange Settings;
	FAudioFactoryWindowTokenRange Preview;

	FAudioFactoryWindowTokens()
	{
		Preview.Min = FVector2D(FAudioFactoryConstants::PreviewWindowMinWidth, 0.0f);
		Preview.Max = FVector2D(FAudioFactoryConstants::PreviewWindowMaxWidth, 0.0f);
	}
};

struct AF_UI_API FAudioFactoryPluginWindowTokens
{
	float HeaderHeight = 40.0f;            // was 36 — native toolbar rhythm
	float ContextStripHeight = 30.0f;      // MINIMUM height, not an override — see strip widgets
	float StatusStripHeight = 26.0f;       // minimum height
	float FooterHeight = 48.0f;            // was 44 — native dialog footers breathe
	float WindowRadius = 4.0f;
	float AccentUnderlineHeight = 2.0f;

	FLinearColor DividerColor;   // FStyleColors::Black
	FLinearColor Surface;        // FStyleColors::Panel
	FLinearColor SurfaceAlt;     // FStyleColors::Header (used ONLY by the header bar now)
	FLinearColor InputSurface;   // FStyleColors::Input
	FLinearColor Recessed;       // FStyleColors::Recessed (details body backdrop)
	FLinearColor TextPrimary;    // FStyleColors::Foreground
	FLinearColor TextHover;      // FStyleColors::ForegroundHover (link hover)
	FLinearColor TextMuted;      // theme-derived — no dedicated slot exists
	FLinearColor TextDisabled;   // theme-derived — no dedicated slot exists

	// 0.10 is the strongest severity wash that keeps TextPrimary (Foreground) at WCAG AA 4.5:1
	// over Lerp(Surface, Severity) for the engine's bright Warning/Success slots — enforced by
	// the status-strip contrast test.
	float StatusTintStrength = 0.10f;
	FLinearColor StatusError;    // FStyleColors::Error
	FLinearColor StatusWarning;  // FStyleColors::Warning
	FLinearColor StatusReady;    // FStyleColors::Success
	FLinearColor StatusNeutral;  // = TextMuted

	// Resolves every color from live FStyleColors slots. Safe to call at runtime only — this type
	// is instantiated the first time FAudioFactoryStyle::Get() runs (a lazily-constructed
	// function-local static), never at static-init time.
	FAudioFactoryPluginWindowTokens();
};

inline FAudioFactoryPluginWindowTokens::FAudioFactoryPluginWindowTokens()
{
	DividerColor = FStyleColors::Black.GetSpecifiedColor();
	Surface = FStyleColors::Panel.GetSpecifiedColor();
	SurfaceAlt = FStyleColors::Header.GetSpecifiedColor();
	InputSurface = FStyleColors::Input.GetSpecifiedColor();
	Recessed = FStyleColors::Recessed.GetSpecifiedColor();
	TextPrimary = FStyleColors::Foreground.GetSpecifiedColor();
	TextHover = FStyleColors::ForegroundHover.GetSpecifiedColor();
	TextMuted = FMath::Lerp(Surface, TextPrimary, 0.62f);
	TextDisabled = FMath::Lerp(Surface, TextPrimary, 0.35f);

	StatusError = FStyleColors::Error.GetSpecifiedColor();
	StatusWarning = FStyleColors::Warning.GetSpecifiedColor();
	StatusReady = FStyleColors::Success.GetSpecifiedColor();
	StatusNeutral = TextMuted;
}

struct FAudioFactoryTokens
{
	FAudioFactorySpacingTokens Spacing;
	FAudioFactoryRadiusTokens Radius;
	FAudioFactoryFontTokens FontSize;
	FAudioFactoryColumnTokens Columns;
	FAudioFactoryWindowTokens Window;
	FAudioFactoryPluginWindowTokens PluginWindow;
};

class FAudioFactoryStyle
{
public:
	static const FAudioFactoryStyle& Get()
	{
		static const FAudioFactoryStyle Instance;
		return Instance;
	}

	const FAudioFactoryTokens& Tokens() const
	{
		return TokenSet;
	}

	const FAudioFactoryPluginWindowTokens& PluginWindowTokens() const
	{
		return TokenSet.PluginWindow;
	}

private:
	FAudioFactoryTokens TokenSet;
};
} // namespace AudioExporter

