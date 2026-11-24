// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"

/**
 * AudioFactoryAudioTypes.h -- LAYER 2 (Audio/DSP)
 *
 * Shared enums for audio-domain UI elements (knob tiers, parameter types, value formats).
 * Used by AudioFactory effects plugins that build on top of AudioWidgets.
 *
 * Include "AudioFactoryAudioUI.h" (not "AudioFactoryUI.h") to pull in this layer.
 */

// -----------------------------------------------------------------------------

/**
 * Semantic parameter color categories -- each maps to a distinct hue.
 * Controls the arc fill on SAudioFactoryLabeledKnob / SAudioMaterialKnob.
 * Parameters of the same semantic type share a color across all effects.
 */
namespace AudioExporter {
enum class EAudioFactoryParamColor : uint8
{
	/** Attack, Release, Delay time, PreDelay, DecayTime, LookAhead */
	Time,

	/** InputGain, OutputGain, Wet/Dry Volume, GainDb */
	Gain,

	/** WetLevel, DryLevel, Feedback, Spread, Depth, Mix, Amount */
	Mix,

	/** CutoffFrequency, Frequency, SampleRate */
	Frequency,

	/** FilterQ, Resonance, Bandwidth */
	Resonance,

	/** Threshold, Ratio, Knee */
	Dynamics,
};

// -----------------------------------------------------------------------------

/**
 * Knob size tiers -- 4-step visual hierarchy.
 *
 * Hero    (140px) -- THE defining parameter. One per effect (rarely two).
 * Primary  (72px) -- Core parameters the user adjusts frequently.
 * Secondary(48px) -- Supporting parameters: Wet/Dry, Feedback, Q, etc.
 * Tertiary (36px) -- Rarely-touched parameters: LookAhead, Knee, etc.
 */
enum class EAudioFactoryKnobSize : uint8
{
	/** 36x36 -- rarely-touched / advanced parameters */
	Tertiary,

	/** 48x48 -- supporting parameters (default) */
	Secondary,

	/** 72x72 -- core parameters */
	Primary,

	/** 140x140 -- THE defining parameter of the effect */
	Hero,
};

// -----------------------------------------------------------------------------

/**
 * Value display formats for text labels below knobs.
 * Passed to AudioFactoryAudioTheme::FormatParameterValue().
 */
enum class EAudioFactoryValueFormat : uint8
{
	/** Fall back to EAudioUnitsValueType from the knob widget */
	Default,

	/** e.g. "-12.0 dB" */
	Decibels,

	/** e.g. "440 Hz" or "1.20 kHz" */
	Frequency,

	/** e.g. "10 ms" or "1.50 s" */
	Time,

	/** e.g. "4:1" */
	Ratio,

	/** e.g. "50%" */
	Percentage,

	/** e.g. "1.41" (2 decimal places) */
	QFactor,

	/** e.g. "16" with optional custom suffix */
	Integer,

	/** e.g. "8 Bit" */
	BitDepth,
};
} // namespace AudioExporter

