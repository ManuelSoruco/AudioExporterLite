// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"

/**
 * Available built-in theme presets.
 *
 * Plugins SELECT a theme via FAudioFactoryTheme::SetTheme() -- they do NOT construct
 * their own palette. New themes are authored inside this submodule by AudioFactory only.
 */
namespace AudioExporter {
enum class EAudioFactoryTheme : uint8
{
	/** Near-black professional studio dark -- DEFAULT for all AudioFactory plugins */
	Dark,
};

/**
 * FAudioFactoryPalette
 *
 * All theme-variant colors in a single plain struct.
 * NOT a USTRUCT -- no reflection needed. One shared mutable instance per process.
 *
 * Plugins NEVER construct a FAudioFactoryPalette directly.
 * Call FAudioFactoryTheme::SetTheme() once in StartupModule(),
 * then optionally FAudioFactoryTheme::SetPluginAccent() for brand identity.
 */
struct FAudioFactoryPalette
{
	// -- Surfaces -------------------------------------------------------------

	FLinearColor WindowBG;
	FLinearColor PanelBG;
	FLinearColor SectionBackground;
	FLinearColor SurfaceLight;
	FLinearColor SurfaceRaised;
	FLinearColor SurfaceMid;
	FLinearColor Divider;

	// -- Text ------------------------------------------------------------------

	FLinearColor Header;
	FLinearColor Label;
	FLinearColor Value;
	FLinearColor Disabled;
	FLinearColor TextAccent;
	FLinearColor Brand;
	FLinearColor FocusRingColor;

	// -- Accents ---------------------------------------------------------------

	/** Plugin brand accent -- THE ONLY field SetPluginAccent() can modify. */
	FLinearColor AccentPrimary;
	FLinearColor AccentSecondary;
	FLinearColor AccentWarn;
	FLinearColor AccentInfo;

	// -- Effect Category Accent Bases (Layer 2) -----------------------------

	FLinearColor AccentDelayBase;
	FLinearColor AccentFilterBase;
	FLinearColor AccentDynamicsBase;
	FLinearColor AccentModulationBase;
	FLinearColor AccentDistortionBase;
	FLinearColor AccentReverbBase;
	FLinearColor AccentUtilityBase;

	// -- Bypass ----------------------------------------------------------------

	FLinearColor BypassActiveColor;
	FLinearColor BypassInactiveColor;

	// -- Brush Surface Colors --------------------------------------------------

	FLinearColor SectionFill;
	FLinearColor SectionOutline;
	FLinearColor CardFill;
	FLinearColor CardOutline;
	FLinearColor InsetFill;
	FLinearColor InsetOutline;
	FLinearColor BypassActiveFill;
	FLinearColor BypassActiveOutline;

	// -- Gradient --------------------------------------------------------------

	FLinearColor GradientTop;
	FLinearColor GradientBottom;

	// -- Visualization (Layer 2) -----------------------------------------------

	FLinearColor VisualizationBackground;
	FLinearColor GridMinorColor;
	FLinearColor GridMajorColor;
	FLinearColor VisualizationCurveColor;

	// -- Parameter Type Colors (Layer 2) ---------------------------------------

	FLinearColor ParamColorTime;
	FLinearColor ParamColorGain;
	FLinearColor ParamColorMix;
	FLinearColor ParamColorFrequency;
	FLinearColor ParamColorResonance;
	FLinearColor ParamColorDynamics;
	FLinearColor ParamColorDefault;

	// -- Band Palette (Layer 2 -- 8 hues for EQ / Multiband / TapDelay) --------

	static constexpr int32 NumBandColors = 8;
	FLinearColor BandPalette[NumBandColors];
};
} // namespace AudioExporter

