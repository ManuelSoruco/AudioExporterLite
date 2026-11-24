// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Audio/IAudioFactoryMeterSource.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/**
 * SAudioFactoryGainReductionMeter -- LAYER 2 (Audio/DSP)
 *
 * Gain reduction meter strip (0 dB at top -> MaxDisplayDb at bottom).
 *
 * Reads gain reduction data from IAudioFactoryMeterSource each Tick, applies
 * analogue-style peak-hold and decay, then draws a filled bar in OnPaint.
 *
 * Meter behavior:
 *   - Instantaneous fill: tracks current GR, decays at DecayRateDbPerSec
 *   - Peak hold: held for PeakHoldTime seconds, then also decays
 *   - Color: AccentPrimary below 20 dB reduction, AccentWarn above
 *   - Dims when source has no active GR data
 *
 * Source: pass any IAudioFactoryMeterSource* implementing GetGainReductionDb()
 * and HasGainReductionData().
 */
namespace AudioExporter {
class AF_UI_API SAudioFactoryGainReductionMeter : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryGainReductionMeter)
		: _MeterSource(nullptr)
	{}
		/** Meter data provider. May be null (meter shows silence). */
		SLATE_ARGUMENT(IAudioFactoryMeterSource*, MeterSource)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void Tick(
		const FGeometry& AllottedGeometry,
		const double InCurrentTime,
		const float InDeltaTime) override;

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

private:
	IAudioFactoryMeterSource* MeterSource = nullptr;

	mutable float DisplayReductionDb = 0.f; // current smoothed GR [0..MaxDisplayDb]
	mutable float PeakReductionDb    = 0.f; // peak-hold GR [0..MaxDisplayDb]
	mutable float PeakHoldTimer      = 0.f; // time since peak was set
	mutable bool  bHasActiveData     = false;

	static constexpr float DecayRateDbPerSec = 3.0f;
	static constexpr float PeakHoldTime      = 1.5f;
	static constexpr float MaxDisplayDb      = 40.0f;
};
} // namespace AudioExporter

