// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"

/**
 * IAudioFactoryMeterSource -- LAYER 2 (Audio/DSP)
 *
 * Abstract meter data provider for SAudioFactoryMeterStrip and
 * SAudioFactoryGainReductionMeter. Decouples the meter widgets from any
 * specific audio backend; implement from any audio data source.
 *
 * All getters are const and safe to call from the Slate game thread.
 * Data is typically populated from an audio render thread via atomics inside
 * the implementing class.
 */
namespace AudioExporter {
class IAudioFactoryMeterSource
{
public:
	virtual ~IAudioFactoryMeterSource() = default;

	// -- Level Readout (0..1 linear, 0 = silent) ---------------------------

	/** Current peak level, left channel (or mono). */
	virtual float GetPeakLevelL() const = 0;

	/** Current peak level, right channel (same as L for mono sources). */
	virtual float GetPeakLevelR() const = 0;

	/** RMS level, left channel -- DSP-smoothed background fill value. */
	virtual float GetRmsLevelL() const = 0;

	/** RMS level, right channel. */
	virtual float GetRmsLevelR() const = 0;

	/**
	 * Exchange-based peak read: atomically returns and resets the peak-since-last-call.
	 * Avoids missing transients between Slate Tick frames.
	 * Default implementation returns GetPeakLevelL() without reset (safe but imprecise).
	 */
	virtual float ExchangePeakL() { return GetPeakLevelL(); }
	virtual float ExchangePeakR() { return GetPeakLevelR(); }

	// -- Gain Reduction (optional) -----------------------------------------

	/**
	 * Gain reduction magnitude in dB (positive value = reduction applied).
	 * Returns 0 when the source is not a dynamics processor or GR is unavailable.
	 */
	virtual float GetGainReductionDb() const { return 0.0f; }

	/** True when the implementing class has live gain reduction data to display. */
	virtual bool HasGainReductionData() const { return false; }

	// -- Activity ----------------------------------------------------------

	/** True when the source is actively providing valid meter data. */
	virtual bool IsActive() const = 0;
};
} // namespace AudioExporter

