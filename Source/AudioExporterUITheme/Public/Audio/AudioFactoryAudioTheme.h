// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Audio/AudioFactoryAudioTypes.h"
#include "AudioMaterialSlate/AudioMaterialSlateTypes.h"

/**
 * AudioFactoryAudioTheme -- LAYER 2 (Audio/DSP)
 *
 * Knob and button style factories, value formatters, logarithmic frequency mapping,
 * and audio-domain color helpers for AudioFactory effects plugins.
 *
 * Requires "AudioWidgets" in PrivateDependencyModuleNames in consuming Build.cs.
 * Include "AudioFactoryAudioUI.h" (not "AudioFactoryUI.h") to pull in this layer.
 *
 * ALL functions are inline -- no separate .cpp. Source-only embedding.
 */
namespace AudioExporter {
namespace AudioFactoryAudioTheme
{

// -----------------------------------------------------------------------------
//  Parameter Colors
// -----------------------------------------------------------------------------

/** Returns the arc-fill color for the given parameter semantic type. */
inline FLinearColor GetParamColor(EAudioFactoryParamColor Type)
{
	const FAudioFactoryPalette& P = FAudioFactoryTheme::GetPalette();
	switch (Type)
	{
	case EAudioFactoryParamColor::Time:      return P.ParamColorTime;
	case EAudioFactoryParamColor::Gain:      return P.ParamColorGain;
	case EAudioFactoryParamColor::Mix:       return P.ParamColorMix;
	case EAudioFactoryParamColor::Frequency: return P.ParamColorFrequency;
	case EAudioFactoryParamColor::Resonance: return P.ParamColorResonance;
	case EAudioFactoryParamColor::Dynamics:  return P.ParamColorDynamics;
	default:                                 return P.ParamColorDefault;
	}
}

// -----------------------------------------------------------------------------
//  Knob Sizes
// -----------------------------------------------------------------------------

/** Returns the pixel size (width = height) for the given knob size tier. */
inline float GetKnobSize(EAudioFactoryKnobSize Size)
{
	switch (Size)
	{
	case EAudioFactoryKnobSize::Tertiary:  return 36.0f;
	case EAudioFactoryKnobSize::Secondary: return 48.0f;
	case EAudioFactoryKnobSize::Primary:   return 72.0f;
	case EAudioFactoryKnobSize::Hero:      return 140.0f;
	default:                               return 48.0f;
	}
}

// -----------------------------------------------------------------------------
//  Knob Style Factory
// -----------------------------------------------------------------------------

/**
 * Build an FAudioMaterialKnobStyle tinted to the given parameter type and accent.
 *
 * @param ParamType              Semantic parameter type (determines arc fill color)
 * @param AccentColor            Plugin/effect accent color -- subtly tints the knob body
 * @param KnobSize               Desired size tier (default: Secondary = 48px)
 * @param SaturationMultiplier   Color saturation multiplier (1.0 = identity, from settings)
 */
inline FAudioMaterialKnobStyle MakeKnobStyle(
	EAudioFactoryParamColor ParamType,
	const FLinearColor& AccentColor,
	EAudioFactoryKnobSize KnobSize = EAudioFactoryKnobSize::Secondary,
	float SaturationMultiplier = 1.0f)
{
	FAudioMaterialKnobStyle Style = FAudioMaterialKnobStyle::GetDefault();

	const FLinearColor RawParamCol = GetParamColor(ParamType);
	FLinearColor ParamCol;
	{
		FLinearColor HSV = RawParamCol.LinearRGBToHSV();
		HSV.G = FMath::Clamp(HSV.G * SaturationMultiplier, 0.f, 1.f);
		const float BrightnessBoost = FMath::Max(0.f, SaturationMultiplier - 1.f) * 0.35f;
		HSV.B = FMath::Clamp(HSV.B + BrightnessBoost, 0.f, 1.f);
		ParamCol = HSV.HSVToLinearRGB();
		ParamCol.A = 1.f;
	}

	const float Size = GetKnobSize(KnobSize);
	const FLinearColor BaseDark  = FAudioFactoryTheme::GetSurfaceLight();
	const FLinearColor KnobBody  = FMath::Lerp(BaseDark, AccentColor, 0.02f);

	Style.SetKnobMainColor(KnobBody);
	Style.SetKnobAccentColor(FMath::Lerp(FAudioFactoryTheme::GetSurfaceRaised(), AccentColor, 0.02f));
	Style.SetKnobShadowColor(FLinearColor(FColor(0x05, 0x05, 0x05)));
	Style.SetSmoothBevelColor(FMath::Lerp(FAudioFactoryTheme::GetSurfaceRaised(), AccentColor, 0.03f));
	Style.SetKnobIndicatorColor(ParamCol);
	Style.SetKnobEdgeFillColor(FAudioFactoryTheme::GetPanelBG());
	Style.SetKnobBarColor(FLinearColor(FColor(0x16, 0x16, 0x16)));
	Style.SetKnobBarShadowColor(FLinearColor(FColor(0x08, 0x08, 0x08)));

	const FLinearColor DesatParamCol = FMath::Lerp(FLinearColor(FColor(0x4D, 0x4D, 0x4D)), ParamCol, 0.5f);
	const FLinearColor BrightParamCol(
		FMath::Min(ParamCol.R * 1.4f + 0.1f, 1.0f),
		FMath::Min(ParamCol.G * 1.4f + 0.1f, 1.0f),
		FMath::Min(ParamCol.B * 1.4f + 0.1f, 1.0f),
		1.0f);

	Style.SetKnobBarFillMinColor(DesatParamCol);
	Style.SetKnobFillMidColor(ParamCol);
	Style.SetKnobBarFillMaxColor(BrightParamCol);
	Style.SetKnobBarFillTintColor(FMath::Lerp(FLinearColor::White, AccentColor, 0.15f));
	Style.DesiredSize = FVector2f(Size, Size);

	return Style;
}

// -----------------------------------------------------------------------------
//  Button Style Factory
// -----------------------------------------------------------------------------

/**
 * Build an FAudioMaterialButtonStyle for a toggle or selection button.
 *
 * @param AccentColor  Plugin/effect accent color -- tints the button body
 * @param bIsActive    Whether the button is in its active/selected state
 */
inline FAudioMaterialButtonStyle MakeButtonStyle(const FLinearColor& AccentColor, bool bIsActive = false)
{
	FAudioMaterialButtonStyle Style = FAudioMaterialButtonStyle::GetDefault();

	const FLinearColor BaseDark = FAudioFactoryTheme::GetSurfaceLight();
	Style.SetButtonMainColor(FMath::Lerp(BaseDark, AccentColor, bIsActive ? 0.45f : 0.03f));
	Style.SetButtonMainColorTint_1(FMath::Lerp(BaseDark, AccentColor, bIsActive ? 0.50f : 0.04f));
	Style.SetButtonMainColorTint_2(FMath::Lerp(BaseDark, AccentColor, bIsActive ? 0.40f : 0.02f));
	Style.SetButtonAccentColor(bIsActive ? AccentColor : FAudioFactoryTheme::GetDisabled());
	Style.SetButtonShadowColor(FLinearColor(FColor(0x08, 0x08, 0x0A)));
	Style.SetButtonUnpressedOutlineColor(bIsActive ? AccentColor : FLinearColor(FColor(0x1A, 0x1A, 0x1A)));
	Style.SetButtonPressedOutlineColor(bIsActive ? AccentColor : FAudioFactoryTheme::GetSurfaceRaised());
	Style.DesiredSize = FVector2f(60.0f, 24.0f);
	return Style;
}

// -----------------------------------------------------------------------------
//  Value Formatting
// -----------------------------------------------------------------------------

/**
 * Format a real-unit parameter value for display below a knob.
 * Uses period decimals (industry standard), regardless of system locale.
 *
 * @param RealValue  Value in real units (not normalized 0-1)
 * @param Format     Display format to use
 * @param Suffix     Optional suffix for Integer format (e.g. "bit")
 */
inline FString FormatParameterValue(float RealValue, EAudioFactoryValueFormat Format, const FString& Suffix = TEXT(""))
{
	switch (Format)
	{
	case EAudioFactoryValueFormat::Decibels:
		return FString::Printf(TEXT("%.1f dB"), RealValue);

	case EAudioFactoryValueFormat::Frequency:
		return (RealValue >= 1000.f)
			? FString::Printf(TEXT("%.2f kHz"), RealValue / 1000.f)
			: FString::Printf(TEXT("%.0f Hz"), RealValue);

	case EAudioFactoryValueFormat::Time:
		return (RealValue >= 1000.f)
			? FString::Printf(TEXT("%.2f s"), RealValue / 1000.f)
			: FString::Printf(TEXT("%.0f ms"), RealValue);

	case EAudioFactoryValueFormat::Ratio:
	{
		const int32 IntVal = FMath::RoundToInt32(RealValue);
		return FMath::IsNearlyEqual(RealValue, static_cast<float>(IntVal), 0.05f)
			? FString::Printf(TEXT("%d:1"), IntVal)
			: FString::Printf(TEXT("%.1f:1"), RealValue);
	}

	case EAudioFactoryValueFormat::Percentage:
		return FString::Printf(TEXT("%.0f%%"), RealValue * 100.f);

	case EAudioFactoryValueFormat::QFactor:
		return FString::Printf(TEXT("%.2f"), RealValue);

	case EAudioFactoryValueFormat::Integer:
		return Suffix.IsEmpty()
			? FString::Printf(TEXT("%d"), FMath::RoundToInt32(RealValue))
			: FString::Printf(TEXT("%d %s"), FMath::RoundToInt32(RealValue), *Suffix);

	case EAudioFactoryValueFormat::BitDepth:
		return FString::Printf(TEXT("%d Bit"), FMath::RoundToInt32(RealValue));

	case EAudioFactoryValueFormat::Default:
	default:
		return FString::Printf(TEXT("%.2f"), RealValue);
	}
}

// -----------------------------------------------------------------------------
//  Logarithmic Frequency Mapping
// -----------------------------------------------------------------------------

/**
 * Convert a frequency in Hz to a normalized 0-1 value using logarithmic (perceptual) scaling.
 * Used by EQ, Filter, Multiband, and reverb editors.
 */
inline float LogFreqToNorm(float Hz, float MinHz = 20.f, float MaxHz = 20000.f)
{
	const float Denom = FMath::Log2(MaxHz) - FMath::Log2(FMath::Max(MinHz, SMALL_NUMBER));
	if (FMath::IsNearlyZero(Denom)) return 0.f;
	return (FMath::Log2(FMath::Max(Hz, MinHz)) - FMath::Log2(FMath::Max(MinHz, SMALL_NUMBER))) / Denom;
}

/** Inverse of LogFreqToNorm -- normalized 0-1 back to Hz. */
inline float NormToLogFreq(float N, float MinHz = 20.f, float MaxHz = 20000.f)
{
	const float LogMin = FMath::Log2(FMath::Max(MinHz, SMALL_NUMBER));
	const float LogMax = FMath::Log2(FMath::Max(MaxHz, SMALL_NUMBER));
	return FMath::Pow(2.f, LogMin + N * (LogMax - LogMin));
}

// -----------------------------------------------------------------------------
//  Knob Tooltip Builder
// -----------------------------------------------------------------------------

/**
 * Build a standardized knob tooltip.
 * Returns the description -- parameters are self-explanatory via the param name label.
 */
inline FText MakeKnobTooltip(float /*Min*/, float /*Max*/, const FString& /*Units*/, const FText& Description)
{
	return Description;
}

// -----------------------------------------------------------------------------
//  Visualization Colors
// -----------------------------------------------------------------------------

inline FLinearColor GetVisualizationBG()   { return FAudioFactoryTheme::GetVisualizationBG(); }
inline FLinearColor GetGridMinor()         { return FAudioFactoryTheme::GetGridMinor(); }
inline FLinearColor GetGridMajor()         { return FAudioFactoryTheme::GetGridMajor(); }
inline FLinearColor GetCurveColor()        { return FAudioFactoryTheme::GetVisualizationCurve(); }
inline FLinearColor GetCurveGlow()         { return FAudioFactoryTheme::GetVisualizationGlow(); }

// -----------------------------------------------------------------------------
//  Effect Category Accents
// -----------------------------------------------------------------------------

/** Semantic effect category types for per-category accent stripes. */
enum class EAudioFactoryCategoryAccent : uint8
{
	Delay,
	Filter,
	Dynamics,
	Modulation,
	Distortion,
	Reverb,
	Utility,
};

/** Returns the darkened accent color for the given effect category. */
inline FLinearColor GetCategoryAccent(EAudioFactoryCategoryAccent Category)
{
	switch (Category)
	{
	case EAudioFactoryCategoryAccent::Delay:      return FAudioFactoryTheme::GetAccentDelay();
	case EAudioFactoryCategoryAccent::Filter:     return FAudioFactoryTheme::GetAccentFilter();
	case EAudioFactoryCategoryAccent::Dynamics:   return FAudioFactoryTheme::GetAccentDynamics();
	case EAudioFactoryCategoryAccent::Modulation: return FAudioFactoryTheme::GetAccentModulation();
	case EAudioFactoryCategoryAccent::Distortion: return FAudioFactoryTheme::GetAccentDistortion();
	case EAudioFactoryCategoryAccent::Reverb:     return FAudioFactoryTheme::GetAccentReverb();
	case EAudioFactoryCategoryAccent::Utility:    return FAudioFactoryTheme::GetAccentUtility();
	default:                                       return FAudioFactoryTheme::GetAccentPrimary();
	}
}

// -----------------------------------------------------------------------------
//  Band Palette
// -----------------------------------------------------------------------------

/** Shared 8-color palette for EQ / Multiband / TapDelay band coloring. Never allocates. */
inline const FLinearColor* GetBandPalette() { return FAudioFactoryTheme::GetBandPalette(); }

} // namespace AudioFactoryAudioTheme
} // namespace AudioExporter

