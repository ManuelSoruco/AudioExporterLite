// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

// SAudioFactoryGainReductionMeter -- gain reduction bar with hold/decay ballistics.

#include "Audio/Widgets/SAudioFactoryGainReductionMeter.h"
#include "Foundation/AudioFactoryTheme.h"
namespace AudioExporter {

#define LOCTEXT_NAMESPACE "SAudioFactoryGainReductionMeter"

void SAudioFactoryGainReductionMeter::Construct(const FArguments& InArgs)
{
	MeterSource = InArgs._MeterSource;
	ChildSlot[ SNullWidget::NullWidget ];
}

void SAudioFactoryGainReductionMeter::Tick(
	const FGeometry& AllottedGeometry,
	const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!MeterSource || !MeterSource->HasGainReductionData())
	{
		// Decay to zero when no data
		bHasActiveData    = false;
		DisplayReductionDb = FMath::Max(DisplayReductionDb - DecayRateDbPerSec * InDeltaTime, 0.f);
		PeakReductionDb    = FMath::Max(PeakReductionDb    - DecayRateDbPerSec * InDeltaTime, 0.f);
		return;
	}

	bHasActiveData = true;
	const float RawDb = FMath::Clamp(MeterSource->GetGainReductionDb(), 0.f, MaxDisplayDb);

	// Fill: instant attack, slow decay
	if (RawDb > DisplayReductionDb)
	{
		DisplayReductionDb = RawDb;
	}
	else
	{
		DisplayReductionDb = FMath::Max(DisplayReductionDb - DecayRateDbPerSec * InDeltaTime, 0.f);
	}

	// Peak hold
	if (RawDb >= PeakReductionDb)
	{
		PeakReductionDb = RawDb;
		PeakHoldTimer   = 0.f;
	}
	else
	{
		PeakHoldTimer += InDeltaTime;
		if (PeakHoldTimer > PeakHoldTime)
		{
			PeakReductionDb = FMath::Max(PeakReductionDb - DecayRateDbPerSec * InDeltaTime, 0.f);
		}
	}
}

int32 SAudioFactoryGainReductionMeter::OnPaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	LayerId = SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect,
		OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);

	const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");

	const FVector2f Size   = AllottedGeometry.GetLocalSize();
	const float     BarH   = Size.Y;
	const float     BarW   = Size.X;

	const float FillFrac = FMath::Clamp(DisplayReductionDb / MaxDisplayDb, 0.f, 1.f);
	const float PeakFrac = FMath::Clamp(PeakReductionDb   / MaxDisplayDb, 0.f, 1.f);

	// Dim everything when no active data
	const float Alpha = bHasActiveData ? 1.0f : 0.25f;

	// Fill color: accent primary for light reduction, accent warn for heavy
	const FLinearColor FillCol = (DisplayReductionDb < 20.f)
		? FAudioFactoryTheme::GetAccentPrimary().CopyWithNewOpacity(Alpha * 0.85f)
		: FAudioFactoryTheme::GetAccentWarn().CopyWithNewOpacity(Alpha * 0.85f);

	const float FillH = FillFrac * BarH;
	if (FillH > 0.f)
	{
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(
				FVector2f(BarW, FillH),
				FSlateLayoutTransform(FVector2f(0.f, 0.f))),
			WhiteBrush, ESlateDrawEffect::None, FillCol);
	}

	// Peak hold tick (2px)
	const float PeakY = PeakFrac * BarH;
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
		AllottedGeometry.ToPaintGeometry(
			FVector2f(BarW, 2.f),
			FSlateLayoutTransform(FVector2f(0.f, PeakY))),
		WhiteBrush, ESlateDrawEffect::None,
		FLinearColor::White.CopyWithNewOpacity(Alpha * PeakFrac * 0.8f));

	return LayerId + 2;
}

#undef LOCTEXT_NAMESPACE

} // namespace AudioExporter
