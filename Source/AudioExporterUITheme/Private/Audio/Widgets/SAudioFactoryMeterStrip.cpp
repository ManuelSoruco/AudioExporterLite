// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

// SAudioFactoryMeterStrip -- triple-value level meter (RMS / Realtime Peak / Historic Peak).

#include "Audio/Widgets/SAudioFactoryMeterStrip.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Styling/CoreStyle.h"
namespace AudioExporter {

#define LOCTEXT_NAMESPACE "SAudioFactoryMeterStrip"

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

float SAudioFactoryMeterStrip::LinearToDb(float Linear)
{
	if (Linear <= 0.000001f) return -80.0f;
	return FMath::Clamp(20.0f * FMath::LogX(10.0f, Linear), -80.0f, 0.0f);
}

FString SAudioFactoryMeterStrip::DbToString(float Db)
{
	if (Db <= -80.0f) return TEXT("\u2212\u221E dB"); // -inf dB
	return FString::Printf(TEXT("%.1f dB"), Db);
}

// -----------------------------------------------------------------------------
//  Construct
// -----------------------------------------------------------------------------

void SAudioFactoryMeterStrip::Construct(const FArguments& InArgs)
{
	MeterSource = InArgs._MeterSource;
	bIsInput    = InArgs._bIsInput;
	bIsStereo   = InArgs._bIsStereo;

	const FText Label = bIsInput
		? LOCTEXT("InputLabel",  "IN")
		: LOCTEXT("OutputLabel", "OUT");

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(bIsStereo ? 34.0f : 20.0f)
		[
			SNew(SVerticalBox)

			// IN/OUT label
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.0f, 4.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FAudioFactoryTheme::GetHeader())
			]

			// Bar area (fills remaining height minus readout)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.HAlign(HAlign_Center)
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(bIsStereo ? 18.0f : 8.0f)
			]

			// Numeric readout placeholder (custom-painted below)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Fill)
			[
				SNew(SBox).HeightOverride(ReadoutHeight)
			]
		]
	];
}

// -----------------------------------------------------------------------------
//  Tick
// -----------------------------------------------------------------------------

void SAudioFactoryMeterStrip::Tick(
	const FGeometry& AllottedGeometry,
	const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!MeterSource || !MeterSource->IsActive())
	{
		DisplayLevel = DisplayLevelR = RmsLevel = RmsLevelR = 0.f;
		PeakLevel = PeakLevelR = 0.f;
		PeakHoldTimer = PeakHoldTimerR = 0.f;
		// Historic peak intentionally preserved across engine disconnect
		return;
	}

	// Peak (exchange -- max since last Tick, resets to 0). Exactly one exchange per channel per
	// Tick: the source exposes a single peak pair, so IN and OUT strips read the same accumulator
	// and a second exchange would hand the other strip a zeroed peak.
	float RawL = MeterSource->ExchangePeakL();
	float RawR = MeterSource->ExchangePeakR();

	// RMS (plain read, DSP already smooths it)
	float RmsL = MeterSource->GetRmsLevelL();
	float RmsR = MeterSource->GetRmsLevelR();

	if (!bIsStereo) { RawR = RawL; RmsR = RmsL; }

	// Ballistics (instant attack, PPM decay)
	auto ApplyBallistics = [&](float Raw, float& Display, float& Peak, float& HoldTimer)
	{
		if (Raw > Display) { Display = Raw; }
		else               { Display = FMath::Max(Display - DecayRate * InDeltaTime, 0.f); }

		if (Raw >= Peak) { Peak = Raw; HoldTimer = 0.f; }
		else
		{
			HoldTimer += InDeltaTime;
			if (HoldTimer > PeakHoldTime)
				Peak = FMath::Max(Peak - PeakDecayRate * InDeltaTime, 0.f);
		}
	};

	ApplyBallistics(RawL, DisplayLevel,  PeakLevel,  PeakHoldTimer);
	ApplyBallistics(RawR, DisplayLevelR, PeakLevelR, PeakHoldTimerR);

	// RMS smooth toward new value
	RmsLevel  = FMath::Lerp(RmsLevel,  RmsL, 0.3f);
	RmsLevelR = FMath::Lerp(RmsLevelR, RmsR, 0.3f);

	// Historic peak
	HistoricPeak  = FMath::Max(HistoricPeak,  DisplayLevel);
	HistoricPeakR = FMath::Max(HistoricPeakR, DisplayLevelR);
}

// -----------------------------------------------------------------------------
//  OnPaint
// -----------------------------------------------------------------------------

int32 SAudioFactoryMeterStrip::OnPaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	LayerId = SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);

	const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");
	const FLinearColor RmsColor(0.04f, 0.06f, 0.03f, 1.f);
	const FLinearColor PeakOkColor(0.06f, 0.55f, 0.06f, 1.f);
	const FLinearColor PeakClipColor(0.9f, 0.15f, 0.05f, 1.f);
	const FLinearColor PeakTickColor = FLinearColor::White;
	const FLinearColor ReadoutColor(0.9f, 0.7f, 0.1f, 1.f);

	const float TotalH   = AllottedGeometry.GetLocalSize().Y;
	const float ReadoutY = TotalH - ReadoutHeight;
	const float BarH     = ReadoutY - 24.f;  // minus label and padding
	const float BarTop   = 24.f;

	const float BarW    = bIsStereo ? 8.0f : 8.0f;
	const float BarGap  = 2.f;
	const float BarOffL = bIsStereo ? 1.f : 5.f;
	const float BarOffR = bIsStereo ? BarOffL + BarW + BarGap : BarOffL;

	auto PaintChannel = [&](float Rms, float Display, float Peak, float BarX)
	{
		// RMS fill (background)
		const float RmsFillH = FMath::Clamp(Rms, 0.f, 1.f) * BarH;
		if (RmsFillH > 0.f)
		{
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
				AllottedGeometry.ToPaintGeometry(
					FVector2f(BarW, RmsFillH),
					FSlateLayoutTransform(FVector2f(BarX, BarTop + BarH - RmsFillH))),
				WhiteBrush, ESlateDrawEffect::None, RmsColor);
		}

		// Realtime peak fill
		const float PeakFillH = FMath::Clamp(Display, 0.f, 1.f) * BarH;
		const FLinearColor PeakCol = (Display >= 0.99f) ? PeakClipColor : PeakOkColor;
		if (PeakFillH > 0.f)
		{
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
				AllottedGeometry.ToPaintGeometry(
					FVector2f(BarW, PeakFillH),
					FSlateLayoutTransform(FVector2f(BarX, BarTop + BarH - PeakFillH))),
				WhiteBrush, ESlateDrawEffect::None, PeakCol);
		}

		// Peak hold tick (2px)
		const float PeakTickY = BarTop + BarH - FMath::Clamp(Peak, 0.f, 1.f) * BarH;
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 2,
			AllottedGeometry.ToPaintGeometry(
				FVector2f(BarW, 2.f),
				FSlateLayoutTransform(FVector2f(BarX, PeakTickY))),
			WhiteBrush, ESlateDrawEffect::None, PeakTickColor.CopyWithNewOpacity(Peak * 0.9f));
	};

	PaintChannel(RmsLevel,  DisplayLevel,  PeakLevel,  BarOffL);
	if (bIsStereo)
	{
		PaintChannel(RmsLevelR, DisplayLevelR, PeakLevelR, BarOffR);
	}

	// Numeric readout (historic peak average)
	const float HistoricAvg = bIsStereo ? (HistoricPeak + HistoricPeakR) * 0.5f : HistoricPeak;
	const FString ReadoutStr = DbToString(LinearToDb(HistoricAvg));

	FSlateDrawElement::MakeText(OutDrawElements, LayerId + 3,
		AllottedGeometry.ToPaintGeometry(
			FVector2f(AllottedGeometry.GetLocalSize().X, ReadoutHeight),
			FSlateLayoutTransform(FVector2f(0.f, ReadoutY))),
		ReadoutStr,
		FCoreStyle::GetDefaultFontStyle("Regular", 7),
		ESlateDrawEffect::None, ReadoutColor);

	return LayerId + 4;
}

// -----------------------------------------------------------------------------
//  Mouse
// -----------------------------------------------------------------------------

FReply SAudioFactoryMeterStrip::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		HistoricPeak  = 0.f;
		HistoricPeakR = 0.f;
		return FReply::Handled();
	}
	return SCompoundWidget::OnMouseButtonDown(MyGeometry, MouseEvent);
}

#undef LOCTEXT_NAMESPACE

} // namespace AudioExporter
