// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"

/**
 * EAFParamControl
 *
 * Discriminates the control type rendered by FAudioFactoryParamSpec.
 * Six standard types + FreeText + Separator cover all settings-dialog use cases.
 * Exotic layouts (paired sliders, template preview) stay as custom section builders.
 */
namespace AudioExporter {
enum class EAFParamControl : uint8
{
	Toggle,       // SAudioFactoryToggle  -- bool
	RadioGroup,   // SAudioFactoryButton  -- int32 (index into Options)
	Dropdown,     // SAudioFactoryDropdown -- int32
	SliderFloat,  // SSlider + value text  -- float
	PathPicker,   // SAudioFactoryPathPicker -- FText path + browse button
	ReadOnlyText, // STextBlock            -- FText (summaries, counts)
	FreeText,     // SEditableTextBox      -- FString (suffix, template fields)
	Separator,    // Thin horizontal divider line (no label, no control)
};

/**
 * FAudioFactoryParamSpec
 *
 * Data-driven descriptor for a single parameter row inside SAudioFactoryDetailsPanel.
 *
 * Usage: populate the relevant fields for your control type and optionally call
 * .AsSub() to indent as a subordinate param, or .When(Guard) to grey it out.
 *
 * Static builders are provided for all standard types (fluent API).
 */
struct FAudioFactoryParamSpec
{
	// -- Identity -------------------------------------------------------------
	FText           Label;
	FText           ToolTip;
	EAFParamControl Control = EAFParamControl::Toggle;

	// -- Sub-param (indented, visually subordinate) ----------------------------
	/** When true, renders with SpacingL left indent to signal dependency on a parent toggle. */
	bool bIsSubParam = false;

	// -- Toggle ----------------------------------------------------------------
	TAttribute<bool>       BoolValue;
	TFunction<void(bool)>  OnBoolChanged;

	// -- RadioGroup / Dropdown -------------------------------------------------
	TArray<FText>          Options;
	TAttribute<int32>      IntValue;
	TFunction<void(int32)> OnIntChanged;

	// -- SliderFloat ----------------------------------------------------------
	TAttribute<float>      FloatValue;
	TFunction<void(float)> OnFloatChanged;
	float                  FloatMin     = 0.0f;
	float                  FloatMax     = 1.0f;
	float                  FloatStep    = 0.0f;          // 0 = continuous
	TAttribute<FText>      FloatDisplay;                 // formatted text e.g. "-16.0 dB"

	// -- PathPicker -----------------------------------------------------------
	TAttribute<FText>  PathText;
	TFunction<void()>  OnBrowsePath;

	// -- ReadOnlyText ---------------------------------------------------------
	TAttribute<FText>      ReadOnlyText;
	TAttribute<FSlateColor> ReadOnlyColor;

	// -- FreeText -------------------------------------------------------------
	TAttribute<FText>              FreeTextValue;
	TFunction<void(const FString&)> OnFreeTextChanged;

	// -- Enabled guard --------------------------------------------------------
	/** When false, the entire row is greyed out and non-interactive. */
	TAttribute<bool> bIsEnabled = true;

	// -- Fluent modifiers -----------------------------------------------------

	FAudioFactoryParamSpec& AsSub()
	{
		bIsSubParam = true;
		return *this;
	}

	FAudioFactoryParamSpec& When(TAttribute<bool> Guard)
	{
		bIsEnabled = Guard;
		return *this;
	}

	// -- Static builders -------------------------------------------------------

	static FAudioFactoryParamSpec Toggle(
		FText InLabel, FText InTip,
		TAttribute<bool> InVal, TFunction<void(bool)> InCb)
	{
		FAudioFactoryParamSpec S;
		S.Control        = EAFParamControl::Toggle;
		S.Label          = MoveTemp(InLabel);
		S.ToolTip        = MoveTemp(InTip);
		S.BoolValue      = InVal;
		S.OnBoolChanged  = MoveTemp(InCb);
		return S;
	}

	static FAudioFactoryParamSpec Radio(
		FText InLabel, FText InTip,
		TArray<FText> InOpts,
		TAttribute<int32> InVal, TFunction<void(int32)> InCb)
	{
		FAudioFactoryParamSpec S;
		S.Control        = EAFParamControl::RadioGroup;
		S.Label          = MoveTemp(InLabel);
		S.ToolTip        = MoveTemp(InTip);
		S.Options        = MoveTemp(InOpts);
		S.IntValue       = InVal;
		S.OnIntChanged   = MoveTemp(InCb);
		return S;
	}

	static FAudioFactoryParamSpec Combo(
		FText InLabel, FText InTip,
		TArray<FText> InOpts,
		TAttribute<int32> InVal, TFunction<void(int32)> InCb)
	{
		FAudioFactoryParamSpec S;
		S.Control        = EAFParamControl::Dropdown;
		S.Label          = MoveTemp(InLabel);
		S.ToolTip        = MoveTemp(InTip);
		S.Options        = MoveTemp(InOpts);
		S.IntValue       = InVal;
		S.OnIntChanged   = MoveTemp(InCb);
		return S;
	}

	static FAudioFactoryParamSpec Slider(
		FText InLabel, FText InTip,
		float InMin, float InMax,
		TAttribute<float> InVal, TFunction<void(float)> InCb,
		TAttribute<FText> InDisplay = TAttribute<FText>())
	{
		FAudioFactoryParamSpec S;
		S.Control        = EAFParamControl::SliderFloat;
		S.Label          = MoveTemp(InLabel);
		S.ToolTip        = MoveTemp(InTip);
		S.FloatMin       = InMin;
		S.FloatMax       = InMax;
		S.FloatValue     = InVal;
		S.OnFloatChanged = MoveTemp(InCb);
		S.FloatDisplay   = InDisplay;
		return S;
	}

	static FAudioFactoryParamSpec Path(
		FText InLabel, FText InTip,
		TAttribute<FText> InPath, TFunction<void()> InBrowse)
	{
		FAudioFactoryParamSpec S;
		S.Control       = EAFParamControl::PathPicker;
		S.Label         = MoveTemp(InLabel);
		S.ToolTip       = MoveTemp(InTip);
		S.PathText      = InPath;
		S.OnBrowsePath  = MoveTemp(InBrowse);
		return S;
	}

	static FAudioFactoryParamSpec Text(
		FText InLabel,
		TAttribute<FText> InText,
		TAttribute<FSlateColor> InColor = TAttribute<FSlateColor>())
	{
		FAudioFactoryParamSpec S;
		S.Control       = EAFParamControl::ReadOnlyText;
		S.Label         = MoveTemp(InLabel);
		S.ReadOnlyText  = InText;
		S.ReadOnlyColor = InColor;
		return S;
	}

	static FAudioFactoryParamSpec EditText(
		FText InLabel, FText InTip,
		TAttribute<FText> InVal, TFunction<void(const FString&)> InCb)
	{
		FAudioFactoryParamSpec S;
		S.Control           = EAFParamControl::FreeText;
		S.Label             = MoveTemp(InLabel);
		S.ToolTip           = MoveTemp(InTip);
		S.FreeTextValue     = InVal;
		S.OnFreeTextChanged = MoveTemp(InCb);
		return S;
	}

	static FAudioFactoryParamSpec Line()
	{
		FAudioFactoryParamSpec S;
		S.Control = EAFParamControl::Separator;
		return S;
	}
};

/**
 * FAudioFactorySectionSpec
 *
 * Groups a set of FAudioFactoryParamSpec rows under a collapsible section header.
 * Feed an array of these to SAudioFactoryDetailsPanel.
 */
struct FAudioFactorySectionSpec
{
	FText                          Label;
	bool                           bDefaultExpanded = true;
	TArray<FAudioFactoryParamSpec> Params;
};
} // namespace AudioExporter

