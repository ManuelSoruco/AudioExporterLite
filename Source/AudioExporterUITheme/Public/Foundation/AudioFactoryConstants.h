// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

/**
 * FAudioFactoryConstants
 *
 * Shared layout and spacing constants used by all AudioFactory plugin UIs.
 * Use these everywhere instead of magic numbers for dimensional consistency.
 */
namespace AudioExporter {
namespace FAudioFactoryConstants
{
	// -- Shared design-system token grid ----------------------------------------
	static constexpr float SpacingXS = 4.0f;
	static constexpr float SpacingS  = 8.0f;
	static constexpr float SpacingM  = 12.0f;
	static constexpr float SpacingL  = 16.0f;
	static constexpr float SpacingXL = 24.0f;

	static constexpr float RadiusS = 3.0f;
	static constexpr float RadiusM = 6.0f;
	static constexpr float RadiusL = 10.0f;

	static constexpr int32 FontSizeXS = 10;
	static constexpr int32 FontSizeSM = 11;
	static constexpr int32 FontSizeMD = 13;
	static constexpr int32 FontSizeLG = 16;
	static constexpr int32 FontSizeXL = 20;

	static constexpr float LabelColumnWidth = 160.0f;
	static constexpr float HelpColumnWidth  = 24.0f;
	static constexpr float SettingsWindowMinWidth = 600.0f;
	static constexpr float SettingsWindowMaxWidth = 760.0f;
	static constexpr float PreviewWindowMinWidth  = 720.0f;
	static constexpr float PreviewWindowMaxWidth  = 900.0f;

	// -- Padding / Spacing -----------------------------------------------------

	/** Outer window content padding (all sides). */
	static constexpr float WindowPadding        = SpacingM;
	/** Inner section padding (between section card edge and its content). */
	static constexpr float SectionPadding       = SpacingL;
	/** Inner padding for inset visualization areas. */
	static constexpr float SectionInnerPadding  = SpacingS;
	/** Vertical gap between sibling section cards. */
	static constexpr float SectionGap           = SpacingM;
	/** Padding inside section header label strip. */
	static constexpr float SectionHeaderPadding = SpacingS;
	/** Gap between a label and the control it describes. */
	static constexpr float LabelToControlGap    = SpacingS;
	/** Horizontal spacing between adjacent control groups. */
	static constexpr float ControlGroupSpacing  = SpacingM;
	/** Gap between adjacent buttons in a row. */
	static constexpr float ButtonGroupSpacing   = 2.0f;
	/** Width of the left-accent stripe on section cards (when enabled); sized for visibility. */
	static constexpr float AccentStripeWidth    = 3.0f;
	/** Horizontal spacing between knobs in a group. */
	static constexpr float KnobHorizontalSpacing = SpacingM;

	// -- Corner Radii ----------------------------------------------------------

	/** Outer window / panel corner radius. */
	static constexpr float PanelRadius   = RadiusL;
	/** Section card corner radius. */
	static constexpr float SectionRadius = 8.0f;
	/** Elevated card corner radius. */
	static constexpr float CardRadius    = 8.0f;
	/** Button corner radius. */
	static constexpr float ButtonRadius  = 6.0f;    // capsule feel
	/** Standard 1px outline / border width. */
	static constexpr float OutlineWidth  = 1.0f;

	// -- Heights ---------------------------------------------------------------

	/** Standard header bar height. */
	static constexpr float HeaderBarHeight = 36.0f;
	/** Preset bar height (Layer 2). */
	static constexpr float PresetBarHeight = 28.0f;
	/** Standard tab / toolbar row height. */
	static constexpr float TabBarHeight    = 28.0f;
	/** Accent dot diameter (header bar, dirty indicators). */
	static constexpr float AccentDotSize   = 8.0f;
	/** Action bar / footer total height. */
	static constexpr float FooterHeight          = 48.0f;
	/** Section header row minimum height (hit target). */
	static constexpr float SectionHeaderHeight   = 32.0f;
	/** Minimum height for every param row in detail panels. */
	static constexpr float ParamRowMinHeight     = 30.0f;
	/** Vertical gap between param rows inside a section. */
	static constexpr float ParamRowGap           = 6.0f;

	// -- Toggle control sizes -------------------------------------------------

	/** Width of the toggle pill track. */
	static constexpr float ToggleWidth       = 44.0f;
	/** Height of the toggle pill track. */
	static constexpr float ToggleHeight      = 22.0f;
	/** Diameter of the toggle sliding thumb circle. */
	static constexpr float ToggleThumbSize   = 16.0f;
	/** Corner radius for the toggle pill track (full pill = height / 2). */
	static constexpr float ToggleRadius      = 11.0f;
	/** Corner radius for the toggle thumb circle (full circle = size / 2). */
	static constexpr float ToggleThumbRadius = 7.5f;

	// -- Font Sizes ------------------------------------------------------------

	/** Window / plugin title font size. */
	static constexpr int32 FontSizeTitle   = FontSizeMD;
	/** Section header label font size. */
	static constexpr int32 FontSizeSection = 9;             // all-caps trick
	/** Standard body / label font size. */
	static constexpr int32 FontSizeBody    = FontSizeSM;
	/** Small hint / brand text font size. */
	static constexpr int32 FontSizeSmall   = FontSizeXS;
}
} // namespace AudioExporter

