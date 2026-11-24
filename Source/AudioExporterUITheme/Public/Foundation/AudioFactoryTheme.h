// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryPalette.h"
#include "Foundation/AudioFactoryConstants.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "UObject/Package.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"

#ifndef AF_UI_API
#define AF_UI_API
#endif

namespace AudioExporter {
namespace AudioFactoryThemePrivate
{
	inline const FAudioFactoryPalette& GetDarkTemplate()
	{
		static const FAudioFactoryPalette Dark{
			// Core surfaces
			.WindowBG = FLinearColor(FColor(0x0C, 0x0C, 0x0C)),
			.PanelBG = FLinearColor(FColor(0x11, 0x11, 0x11)),
			.SectionBackground = FLinearColor(FColor(0x11, 0x11, 0x11)),
			.SurfaceLight = FLinearColor(FColor(0x1A, 0x1A, 0x1A)),
			.SurfaceRaised = FLinearColor(FColor(0x2C, 0x2C, 0x2C)),
			.SurfaceMid = FLinearColor(FColor(0x1A, 0x1A, 0x1A)),
			.Divider = FLinearColor(FColor(0x28, 0x28, 0x28)),

			// Text
			.Header = FLinearColor(FColor(0xE8, 0xE8, 0xE8)),
			.Label = FLinearColor(FColor(0xAA, 0xAA, 0xAA)),
			.Value = FLinearColor(FColor(0xCC, 0xCC, 0xCC)),
			.Disabled = FLinearColor(FColor(0x4A, 0x4A, 0x4A)),
			.TextAccent = FLinearColor(FColor(0xE0, 0xE0, 0xE0)),
			.Brand = FLinearColor(FColor(0x52, 0x52, 0x52)),
			.FocusRingColor = FLinearColor(FColor(0x44, 0x88, 0xFF)),

			// Accents
			.AccentPrimary = FLinearColor(FColor(0x19, 0x99, 0xFF)),
			.AccentSecondary = FLinearColor(FColor(0x00, 0xD4, 0xAA)),
			.AccentWarn = FLinearColor(FColor(0xFF, 0x33, 0x44)),
			.AccentInfo = FLinearColor(FColor(0x44, 0x88, 0xFF)),
			.AccentDelayBase = FLinearColor(FColor(0x2D, 0x9C, 0xDB)),
			.AccentFilterBase = FLinearColor(FColor(0xA8, 0x55, 0xF7)),
			.AccentDynamicsBase = FLinearColor(FColor(0xFF, 0x4D, 0x6A)),
			.AccentModulationBase = FLinearColor(FColor(0x22, 0xD3, 0xEE)),
			.AccentDistortionBase = FLinearColor(FColor(0xF5, 0x9E, 0x0B)),
			.AccentReverbBase = FLinearColor(FColor(0x34, 0xD3, 0x99)),
			.AccentUtilityBase = FLinearColor(FColor(0x94, 0xA3, 0xB8)),
			.BypassActiveColor = FLinearColor(FColor(0xB3, 0x26, 0x26)),
			.BypassInactiveColor = FLinearColor(FColor(0x66, 0x66, 0x66)),

			// Containers
			.SectionFill = FLinearColor(FColor(0x14, 0x14, 0x14)),
			.SectionOutline = FLinearColor(FColor(0x2E, 0x2E, 0x2E)),
			.CardFill = FLinearColor(FColor(0x1E, 0x1E, 0x1E)),
			.CardOutline = FLinearColor(FColor(0x35, 0x35, 0x35)),
			.InsetFill = FLinearColor(FColor(0x06, 0x06, 0x06)),
			.InsetOutline = FLinearColor(FColor(0x0E, 0x0E, 0x0E)),
			.BypassActiveFill = FLinearColor(FColor(0x2E, 0x0A, 0x0A)),
			.BypassActiveOutline = FLinearColor(FColor(0x66, 0x14, 0x14)),
			.GradientTop = FLinearColor(FColor(0x1E, 0x1E, 0x1E)),
			.GradientBottom = FLinearColor(FColor(0x0D, 0x0D, 0x0D)),

			// Visualization
			.VisualizationBackground = FLinearColor(FColor(0x08, 0x08, 0x08)),
			.GridMinorColor = FLinearColor(FColor(0x1A, 0x1A, 0x1A)),
			.GridMajorColor = FLinearColor(FColor(0x22, 0x22, 0x22)),
			.VisualizationCurveColor = FLinearColor(FColor(0xA8, 0xA8, 0xB2)),

			// Parameter colors
			.ParamColorTime = FLinearColor(FColor(0x9B, 0x59, 0xB6)),
			.ParamColorGain = FLinearColor(FColor(0xE8, 0xA8, 0x38)),
			.ParamColorMix = FLinearColor(FColor(0x44, 0xBB, 0x66)),
			.ParamColorFrequency = FLinearColor(FColor(0x29, 0x79, 0xFF)),
			.ParamColorResonance = FLinearColor(FColor(0x00, 0xCC, 0xDD)),
			.ParamColorDynamics = FLinearColor(FColor(0xDD, 0x44, 0x44)),
			.ParamColorDefault = FLinearColor(FColor(0xB3, 0xB3, 0xB3)),

			// Band colors
			.BandPalette = {
				FLinearColor(FColor(0x33, 0x85, 0xF5)),
				FLinearColor(FColor(0x1A, 0xD9, 0xD9)),
				FLinearColor(FColor(0x33, 0xC7, 0x59)),
				FLinearColor(FColor(0xF2, 0xCC, 0x1A)),
				FLinearColor(FColor(0xFF, 0x6B, 0x00)),
				FLinearColor(FColor(0xE6, 0x33, 0x4D)),
				FLinearColor(FColor(0xCC, 0x33, 0xCC)),
				FLinearColor(FColor(0x80, 0xE6, 0x33)),
			}
		};

		return Dark;
	}
}

/**
 * Central theme entry point for AudioFactory Slate modules.
 *
 * Each consuming UITheme module defines AF_UI_API in its Build.cs and owns its
 * module log category definition. The shared source remains identical across
 * plugin consumers.
 */
class AF_UI_API FAudioFactoryTheme
{
public:
	/** Select a built-in theme from the catalog. Call during module startup. */
	static void SetTheme(EAudioFactoryTheme Theme)
	{
		FAudioFactoryPalette& Pal = GetMutablePalette();
		switch (Theme)
		{
		case EAudioFactoryTheme::Dark:
		default:
			Pal = AudioFactoryThemePrivate::GetDarkTemplate();
			break;
		}
	}

	/** Override the active plugin accent after SetTheme() has selected a palette. */
	static void SetPluginAccent(const FLinearColor& AccentColor)
	{
		GetMutablePalette().AccentPrimary = AccentColor;
	}

	/** Returns the active color palette. Read-only after startup. */
	static const FAudioFactoryPalette& GetPalette() { return GetMutablePalette(); }

	static FLinearColor GetWindowBG()          { return GetPalette().WindowBG; }
	static FLinearColor GetPanelBG()           { return GetPalette().PanelBG; }
	static FLinearColor GetSectionBackground() { return GetPalette().SectionBackground; }
	static FLinearColor GetSurfaceLight()      { return GetPalette().SurfaceLight; }
	static FLinearColor GetSurfaceRaised()     { return GetPalette().SurfaceRaised; }
	static FLinearColor GetSurfaceMid()        { return GetPalette().SurfaceMid; }
	static FLinearColor GetDivider()           { return GetPalette().Divider; }

	static FLinearColor GetHeader()     { return GetPalette().Header; }
	static FLinearColor GetLabel()      { return GetPalette().Label; }
	static FLinearColor GetValue()      { return GetPalette().Value; }
	static FLinearColor GetDisabled()   { return GetPalette().Disabled; }
	static FLinearColor GetTextAccent() { return GetPalette().TextAccent; }
	static FLinearColor GetBrand()      { return GetPalette().Brand; }
	static FLinearColor GetFocusRing()  { return GetPalette().FocusRingColor; }

	/** Section group label color for compact all-caps labels. */
	static FLinearColor GetSectionLabel() { return FLinearColor(FColor(0x7A, 0x7A, 0x7A)); }

	static FLinearColor GetAccentPrimary()   { return GetPalette().AccentPrimary; }
	static FLinearColor GetAccentSecondary() { return GetPalette().AccentSecondary; }
	static FLinearColor GetAccentWarn()      { return GetPalette().AccentWarn; }
	static FLinearColor GetAccentInfo()      { return GetPalette().AccentInfo; }

	static FLinearColor GetAccentDelay()      { return DarkenAccent(GetPalette().AccentDelayBase); }
	static FLinearColor GetAccentFilter()     { return DarkenAccent(GetPalette().AccentFilterBase); }
	static FLinearColor GetAccentDynamics()   { return DarkenAccent(GetPalette().AccentDynamicsBase); }
	static FLinearColor GetAccentModulation() { return DarkenAccent(GetPalette().AccentModulationBase); }
	static FLinearColor GetAccentDistortion() { return DarkenAccent(GetPalette().AccentDistortionBase); }
	static FLinearColor GetAccentReverb()     { return DarkenAccent(GetPalette().AccentReverbBase); }
	static FLinearColor GetAccentUtility()    { return DarkenAccent(GetPalette().AccentUtilityBase); }

	static FLinearColor GetBypassActive()   { return GetPalette().BypassActiveColor; }
	static FLinearColor GetBypassInactive() { return GetPalette().BypassInactiveColor; }

	static FLinearColor GetVisualizationBG()    { return GetPalette().VisualizationBackground; }
	static FLinearColor GetGridMinor()          { return GetPalette().GridMinorColor; }
	static FLinearColor GetGridMajor()          { return GetPalette().GridMajorColor; }
	static FLinearColor GetVisualizationCurve() { return GetPalette().VisualizationCurveColor; }
	static FLinearColor GetVisualizationGlow()
	{
		return GetPalette().VisualizationCurveColor.CopyWithNewOpacity(0.12f);
	}

	/** Darkens a color by lerping toward black. */
	static FLinearColor DarkenAccent(const FLinearColor& Color, float Amount = 0.3f)
	{
		return FMath::Lerp(Color, FLinearColor::Black, Amount);
	}

	/** Mark an object dirty before a widget callback mutates UObject-owned state. */
	static void MarkObjectDirty(UObject* Obj)
	{
		if (!ensureMsgf(Obj, TEXT("FAudioFactoryTheme::MarkObjectDirty: null object")))
		{
			return;
		}
		Obj->Modify();
		if (UPackage* Package = Obj->GetOutermost())
		{
			(void)Package->MarkPackageDirty();
		}
	}

	/** Clean up a UE asset name for display in compact UI labels. */
	static FString CleanAssetName(const FString& RawName)
	{
		FString Name = RawName;
		Name.RemoveFromStart(TEXT("SourceEffect"));
		Name.RemoveFromStart(TEXT("SubmixEffect"));

		int32 UnderscoreIndex;
		if (Name.FindLastChar(TEXT('_'), UnderscoreIndex) && UnderscoreIndex > 0)
		{
			const FString Tail = Name.Mid(UnderscoreIndex + 1);
			bool bAllDigits = !Tail.IsEmpty();
			for (TCHAR Ch : Tail)
			{
				if (!FChar::IsDigit(Ch))
				{
					bAllDigits = false;
					break;
				}
			}
			if (bAllDigits)
			{
				Name = Name.Left(UnderscoreIndex);
			}
		}
		Name.RemoveFromEnd(TEXT("Preset"));
		Name = Name.TrimStartAndEnd();
		return Name.IsEmpty() ? TEXT("Default") : Name;
	}

	/** Returns a dim label for use below compact knob groups. */
	static TSharedRef<SWidget> MakeGroupLabel(const FText& InText)
	{
		return SNew(STextBlock)
			.Text(InText)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
			.ColorAndOpacity(FSlateColor(GetSectionLabel()))
			.Justification(ETextJustify::Center);
	}

	/** Returns a one-pixel vertical divider for parameter groups. */
	static TSharedRef<SWidget> MakeVerticalDivider(float TopBottomPadding = 8.0f)
	{
		static const FSlateRoundedBoxBrush DivBrush(FLinearColor(FColor(0x2A, 0x2A, 0x2A)), 0.0f);
		return SNew(SBox)
			.WidthOverride(1.0f)
			.Padding(FMargin(0.0f, TopBottomPadding))
			[
				SNew(SBorder)
				.BorderImage(&DivBrush)
			];
	}

	/** Returns a one-pixel horizontal divider for vertical groups. */
	static TSharedRef<SWidget> MakeHorizontalDivider()
	{
		static const FSlateRoundedBoxBrush DivBrush(FLinearColor(FColor(0x2A, 0x2A, 0x2A)), 0.0f);
		return SNew(SBox)
			.HeightOverride(1.0f)
			[
				SNew(SBorder)
				.BorderImage(&DivBrush)
			];
	}

	static constexpr int32 NumBandPaletteColors = FAudioFactoryPalette::NumBandColors;

	/** Shared color palette for EQ, multiband, and tap-delay band coloring. */
	static const FLinearColor* GetBandPalette() { return GetPalette().BandPalette; }

private:
	static FAudioFactoryPalette& GetMutablePalette()
	{
		static FAudioFactoryPalette Pal = AudioFactoryThemePrivate::GetDarkTemplate();
		return Pal;
	}
};
} // namespace AudioExporter

#include "Foundation/AudioFactoryBrushes.h"
