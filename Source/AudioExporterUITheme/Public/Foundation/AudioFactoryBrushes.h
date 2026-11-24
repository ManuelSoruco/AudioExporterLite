// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

// Included at the bottom of AudioFactoryTheme.h -- FAudioFactoryTheme is fully declared.
// Also safe to include standalone for IDE analysis (#pragma once prevents recursion).
#include "Foundation/AudioFactoryTheme.h"
#include "Brushes/SlateRoundedBoxBrush.h"

/**
 * FAudioFactoryBrushes
 *
 * Full brush factory -- 25+ brushes, all palette-aware.
 * All brushes are lazily initialized static locals (safe in UE5: Slate is always
 * initialized before any editor plugin StartupModule() runs).
 *
 * IMPORTANT: Do NOT call these from OnPaint() (hot path, every frame).
 * Cache brush pointers in your widget's Construct().
 *
 * For palette-reactive brushes, note that static locals are initialized once.
 * If SetTheme() is called after a brush is first accessed, the brush will NOT update.
 * Call SetTheme() before any widget construction to avoid this.
 */
namespace AudioExporter {
namespace FAudioFactoryBrushes
{
	// -- Foundation ------------------------------------------------------------

	/** Main window background -- PanelRadius, no outline. */
	inline const FSlateRoundedBoxBrush& Panel()
	{
		static FSlateRoundedBoxBrush B(FAudioFactoryTheme::GetWindowBG(), FAudioFactoryConstants::PanelRadius);
		return B;
	}

	/** Section inset card -- SectionFill, 1px SectionOutline, 6px radius. */
	inline const FSlateRoundedBoxBrush& Section()
	{
		static FSlateRoundedBoxBrush B(
			FAudioFactoryTheme::GetPalette().SectionFill, FAudioFactoryConstants::SectionRadius,
			FAudioFactoryTheme::GetPalette().SectionOutline, FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Card (elevated panel) -- CardFill, 1px CardOutline, 6px radius. */
	inline const FSlateRoundedBoxBrush& Card()
	{
		static FSlateRoundedBoxBrush B(
			FAudioFactoryTheme::GetPalette().CardFill, FAudioFactoryConstants::CardRadius,
			FAudioFactoryTheme::GetPalette().CardOutline, FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Preset bar background -- rounded top corners only, panel color. */
	inline const FSlateRoundedBoxBrush& PresetBar()
	{
		static FSlateRoundedBoxBrush B(
			FAudioFactoryTheme::GetPanelBG(),
			FVector4(4.0f, 4.0f, 0.0f, 0.0f),
			FLinearColor(FColor(0x24, 0x24, 0x24, 0x80)),
			FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Button (inactive) -- SurfaceLight, 1px Divider, 4px radius. */
	inline const FSlateRoundedBoxBrush& Button()
	{
		static FSlateRoundedBoxBrush B(
			FAudioFactoryTheme::GetSurfaceLight(), FAudioFactoryConstants::ButtonRadius,
			FAudioFactoryTheme::GetDivider(), FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Button (active) -- SurfaceRaised, slightly brighter outline. */
	inline const FSlateRoundedBoxBrush& ButtonActive()
	{
		static FSlateRoundedBoxBrush B(
			FAudioFactoryTheme::GetSurfaceRaised(), FAudioFactoryConstants::ButtonRadius,
			FLinearColor(FColor(0x40, 0x40, 0x40)), FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Bypass button (inactive/processing) -- neutral surface. */
	inline const FSlateRoundedBoxBrush& Bypass()
	{
		static FSlateRoundedBoxBrush B(
			FAudioFactoryTheme::GetSurfaceLight(), FAudioFactoryConstants::ButtonRadius,
			FAudioFactoryTheme::GetDivider(), FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Bypass button (bypassed/active) -- dark red tint. */
	inline const FSlateRoundedBoxBrush& BypassActive()
	{
		const FAudioFactoryPalette& P = FAudioFactoryTheme::GetPalette();
		static FSlateRoundedBoxBrush B(
			P.BypassActiveFill, FAudioFactoryConstants::ButtonRadius,
			P.BypassActiveOutline, FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Inset panel for visualization areas -- very dark fill, subtle border. */
	inline const FSlateRoundedBoxBrush& Inset()
	{
		const FAudioFactoryPalette& P = FAudioFactoryTheme::GetPalette();
		static FSlateRoundedBoxBrush B(
			P.InsetFill, FAudioFactoryConstants::SectionRadius,
			P.InsetOutline, FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Accent dot / dirty indicator -- fully circular, AccentWarn. */
	inline const FSlateRoundedBoxBrush& DirtyIndicator()
	{
		static FSlateRoundedBoxBrush B(FAudioFactoryTheme::GetAccentWarn(), 2.5f);
		return B;
	}

	/** Circular accent dot -- uses AccentPrimary, 999px radius (always a circle). */
	inline const FSlateRoundedBoxBrush& AccentDot()
	{
		static FSlateRoundedBoxBrush B(FAudioFactoryTheme::GetAccentPrimary(), 999.0f);
		return B;
	}

	/** Transparent -- "no background" placeholder. */
	inline const FSlateRoundedBoxBrush& Transparent()
	{
		static FSlateRoundedBoxBrush B(FLinearColor::Transparent, FAudioFactoryConstants::ButtonRadius);
		return B;
	}

	/** Flat 0-radius divider line brush. */
	inline const FSlateRoundedBoxBrush& Divider()
	{
		static FSlateRoundedBoxBrush B(FAudioFactoryTheme::GetDivider(), 0.0f);
		return B;
	}

	/** Rounded 2px divider used in bars and inline separators. */
	inline const FSlateRoundedBoxBrush& RoundedDivider()
	{
		static FSlateRoundedBoxBrush B(FAudioFactoryTheme::GetDivider(), 2.0f);
		return B;
	}

	/** Header bar background -- rounded top corners, dark panel. */
	inline const FSlateRoundedBoxBrush& HeaderBar()
	{
		static FSlateRoundedBoxBrush B(
			FLinearColor(FColor(0x0E, 0x0E, 0x0E)),
			FVector4(6.0f, 6.0f, 0.0f, 0.0f));
		return B;
	}

	/** Toggle checked state -- AccentPrimary fill, fully rounded (pill shape). */
	inline const FSlateRoundedBoxBrush& ToggleOn()
	{
		static FSlateRoundedBoxBrush B(FAudioFactoryTheme::GetAccentPrimary(), 999.0f);
		return B;
	}

	/** Toggle unchecked state -- SurfaceLight, 1px Divider outline. */
	inline const FSlateRoundedBoxBrush& ToggleOff()
	{
		static FSlateRoundedBoxBrush B(
			FAudioFactoryTheme::GetSurfaceLight(), 999.0f,
			FAudioFactoryTheme::GetDivider(), FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Add (+) button -- neutral surface, muted outline. */
	inline const FSlateRoundedBoxBrush& AddButton()
	{
		static FSlateRoundedBoxBrush B(
			FAudioFactoryTheme::GetSurfaceLight(), FAudioFactoryConstants::ButtonRadius,
			FLinearColor(FColor(0x33, 0x33, 0x33)), FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Add (+) button hover -- slightly brighter, accent-tinted outline. */
	inline const FSlateRoundedBoxBrush& AddButtonHover()
	{
		static FSlateRoundedBoxBrush B(
			FLinearColor(FColor(0x38, 0x38, 0x38)), FAudioFactoryConstants::ButtonRadius,
			FAudioFactoryTheme::GetAccentSecondary(), FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Remove (x) button -- subtle dark red tint. */
	inline const FSlateRoundedBoxBrush& RemoveButton()
	{
		static FSlateRoundedBoxBrush B(
			FLinearColor(FColor(0x2E, 0x0D, 0x0D)), FAudioFactoryConstants::ButtonRadius,
			FLinearColor(FColor(0x4D, 0x14, 0x14)), FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/** Remove (x) button hover -- brighter red. */
	inline const FSlateRoundedBoxBrush& RemoveButtonHover()
	{
		static FSlateRoundedBoxBrush B(
			FLinearColor(FColor(0x59, 0x0F, 0x0F)), FAudioFactoryConstants::ButtonRadius,
			FAudioFactoryTheme::GetAccentWarn(), FAudioFactoryConstants::OutlineWidth);
		return B;
	}

	/**
	 * Circular glow brush for Hero knob halo effect.
	 * White fill, extreme radius. Tint with param color at low opacity (0.03-0.10).
	 */
	inline const FSlateRoundedBoxBrush& HeroGlow()
	{
		static FSlateRoundedBoxBrush B(FLinearColor::White, 999.0f);
		return B;
	}

	/** Subtle icon-button hover background. */
	inline const FSlateRoundedBoxBrush& IconButtonHover()
	{
		static FSlateRoundedBoxBrush B(FLinearColor(1.0f, 1.0f, 1.0f, 0.06f), FAudioFactoryConstants::ButtonRadius);
		return B;
	}

	/** Focus ring outline for keyboard-focusable controls. */
	inline const FSlateRoundedBoxBrush& FocusRing()
	{
		static FSlateRoundedBoxBrush B(
			FLinearColor::Transparent, FAudioFactoryConstants::ButtonRadius,
			FAudioFactoryTheme::GetFocusRing(), 1.5f);
		return B;
	}

	/** White tint-mask brush -- used where color is supplied at draw time via tint. */
	inline const FSlateRoundedBoxBrush& TintMask()
	{
		static FSlateRoundedBoxBrush B(FLinearColor::White, 0.0f);
		return B;
	}

	/** Notice card background -- for internal plugin notifications. */
	inline const FSlateRoundedBoxBrush& NoticeCard()
	{
		static FSlateRoundedBoxBrush B(FAudioFactoryTheme::GetSectionBackground(), 8.0f);
		return B;
	}

	/** Notice card accent stripe -- AccentWarn color, 4px radius. */
	inline const FSlateRoundedBoxBrush& NoticeAccent()
	{
		static FSlateRoundedBoxBrush B(FAudioFactoryTheme::GetAccentWarn(), 4.0f);
		return B;
	}

	/** Card bottom shadow strip -- 1px dark line to simulate drop shadow. */
	inline const FSlateRoundedBoxBrush& CardShadow()
	{
		static FSlateRoundedBoxBrush B(
			FLinearColor(0.0f, 0.0f, 0.0f, 0.5f),
			FVector4(0.0f, 0.0f, FAudioFactoryConstants::CardRadius, FAudioFactoryConstants::CardRadius));
		return B;
	}

	/** Inner top-shadow strip -- 1px dark line for inset feel. */
	inline const FSlateRoundedBoxBrush& InnerShadow()
	{
		static FSlateRoundedBoxBrush B(FLinearColor(0.0f, 0.0f, 0.0f, 0.35f), 0.0f);
		return B;
	}

	/** Highlight line brush -- 1px top highlight for glass effect on bars. */
	inline const FSlateRoundedBoxBrush& HighlightLine()
	{
		static FSlateRoundedBoxBrush B(
			FLinearColor(1.0f, 1.0f, 1.0f, 0.06f),
			FVector4(4.0f, 4.0f, 0.0f, 0.0f));
		return B;
	}
}  // namespace FAudioFactoryBrushes
} // namespace AudioExporter

