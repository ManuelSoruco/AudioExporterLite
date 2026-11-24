// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Audio/IAudioFactoryMeterSource.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

/**
 * SAudioFactoryMeterStrip -- LAYER 2 (Audio/DSP)
 *
 * Vertical meter strip with three visual layers:
 *   1. RMS fill        -- dim background bar (slow, stable, from IAudioFactoryMeterSource::GetRmsLevel*)
 *   2. Realtime Peak   -- bright ballistic bar (PPM attack/decay, from ExchangePeak*)
 *   3. Peak Hold tick  -- white 2px marker (holds PeakHoldTime, then decays)
 *
 * Bottom readout: amber numeric dBFS value (historic peak). Click to reset.
 *
 * Source: pass any IAudioFactoryMeterSource* (e.g. FAudioFXPreviewEngine).
 * Passing nullptr or a disconnected source silences the meter gracefully.
 */
namespace AudioExporter {
class AF_UI_API SAudioFactoryMeterStrip : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryMeterStrip)
		: _MeterSource(nullptr)
		, _bIsInput(true)
		, _bIsStereo(true)
	{}
		/** Meter data provider. May be null (meter shows silence). */
		SLATE_ARGUMENT(IAudioFactoryMeterSource*, MeterSource)
		/** true = show "IN" label, false = show "OUT" label */
		SLATE_ARGUMENT(bool, bIsInput)
		/** true = show two bars side-by-side (L/R), false = single mono bar */
		SLATE_ARGUMENT(bool, bIsStereo)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

	/** Click anywhere on strip resets the historic peak hold. */
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	IAudioFactoryMeterSource* MeterSource = nullptr;
	bool bIsInput  = true;
	bool bIsStereo = true;

	// Ballistic display (PPM-style)
	mutable float DisplayLevel  = 0.f;
	mutable float DisplayLevelR = 0.f;

	// Peak hold tick
	mutable float PeakLevel      = 0.f;
	mutable float PeakLevelR     = 0.f;
	mutable float PeakHoldTimer  = 0.f;
	mutable float PeakHoldTimerR = 0.f;

	// RMS (background fill)
	mutable float RmsLevel  = 0.f;
	mutable float RmsLevelR = 0.f;

	// Historic peak (never decays, click to reset)
	mutable float HistoricPeak  = 0.f;
	mutable float HistoricPeakR = 0.f;

	// Ballistic constants
	static constexpr float DecayRate     = 1.7f;  // linear/s
	static constexpr float PeakHoldTime  = 2.0f;  // seconds
	static constexpr float PeakDecayRate = 1.2f;  // linear/s after hold
	static constexpr float ReadoutHeight = 14.f;  // px for numeric readout

	static float LinearToDb(float Linear);
	static FString DbToString(float Db);
};
} // namespace AudioExporter

