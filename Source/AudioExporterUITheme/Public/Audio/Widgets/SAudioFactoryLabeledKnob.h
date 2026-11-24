// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Audio/AudioFactoryAudioTypes.h"
#include "Audio/AudioFactoryAudioTheme.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "AudioMaterialSlate/AudioMaterialSlateTypes.h"
#include "AudioMaterialSlate/SAudioMaterialKnob.h"

/**
 * SAudioFactoryLabeledKnob -- LAYER 2 (Audio/DSP)
 *
 * Composite knob widget for AudioFactory effects plugins. Wraps SAudioMaterialKnob
 * (bare dial) with a parameter name label above and a live value label below.
 *
 * No PropertyAdapter dependency -- bind via Value attribute + OnValueChanged delegate.
 * Style creation goes through AudioFactoryAudioTheme::MakeKnobStyle().
 *
 * Layout (top to bottom):
 *   ParamName label (dim, centered)
 *   SAudioMaterialKnob (dial)
 *   Formatted value readout (live)
 *
 * STYLE NOTE: Style pointer must remain valid for the lifetime of this widget.
 * Store FAudioMaterialKnobStyle in the owning editor (e.g. via StoreKnobStyle()).
 */
namespace AudioExporter {
class AF_UI_API SAudioFactoryLabeledKnob : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryLabeledKnob)
		: _Value(0.5f)
		, _Style(nullptr)
		, _DefaultValue(0.5f)
		, _MinValue(0.f)
		, _MaxValue(1.f)
		, _ValueFormat(EAudioFactoryValueFormat::Default)
		, _ShowParamName(true)
	{}
		/** Parameter name shown above the knob (e.g. "Threshold"). */
		SLATE_ARGUMENT(FText, ParamName)

		/** Normalized [0,1] value bound to the knob. */
		SLATE_ATTRIBUTE(float, Value)

		/** Knob style pointer -- must remain stable (owned by the calling editor). */
		SLATE_ARGUMENT(const FAudioMaterialKnobStyle*, Style)

		/** Normalized [0,1] default value for Ctrl+Click reset. */
		SLATE_ARGUMENT(float, DefaultValue)

		/** Real-unit minimum value (used for value display and text entry clamping). */
		SLATE_ARGUMENT(float, MinValue)

		/** Real-unit maximum value. */
		SLATE_ARGUMENT(float, MaxValue)

		/** Value display format for the readout below the knob. */
		SLATE_ARGUMENT(EAudioFactoryValueFormat, ValueFormat)

		/** Whether to show the param name label. Default: true. */
		SLATE_ARGUMENT(bool, ShowParamName)

		/** Tooltip for the knob area. */
		SLATE_ARGUMENT(FText, KnobToolTipText)

		/** Called whenever the normalized value changes (drag or Ctrl+Click reset). */
		SLATE_EVENT(FOnFloatValueChanged, OnValueChanged)

		/**
		 * Called on any value change -- use to mark the edited UObject dirty.
		 * Supplement to OnValueChanged for cases where dirtying is separate.
		 */
		SLATE_EVENT(FSimpleDelegate, OnMarkDirty)

		/**
		 * The UObject being edited (e.g. USoundEffectPreset). When set, the widget
		 * calls MarkObjectDirty() automatically on Ctrl+Click reset.
		 */
		SLATE_ARGUMENT(TWeakObjectPtr<UObject>, EditedObject)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		ValueAttr     = InArgs._Value;
		DefaultValue  = InArgs._DefaultValue;
		MinValue      = InArgs._MinValue;
		MaxValue      = InArgs._MaxValue;
		ValueFormat   = InArgs._ValueFormat;
		OnValueChanged = InArgs._OnValueChanged;
		OnMarkDirtyDelegate = InArgs._OnMarkDirty;
		EditedObject  = InArgs._EditedObject;

		TSharedPtr<SAudioMaterialKnob> KnobPtr;
		TSharedRef<SWidget> KnobWidget =
			SAssignNew(KnobPtr, SAudioMaterialKnob)
			.Value(ValueAttr)
			.AudioMaterialKnobStyle(InArgs._Style)
			.ToolTipText(InArgs._KnobToolTipText)
			.OnFloatValueChanged_Lambda([this](float NewNorm)
			{
				OnValueChanged.ExecuteIfBound(NewNorm);
				OnMarkDirtyDelegate.ExecuteIfBound();
			});
		InnerKnob = KnobPtr;

		TSharedRef<STextBlock> ValueLabel =
			SNew(STextBlock)
			.Text_Lambda([this]() -> FText
			{
				return FText::FromString(FormatCurrentValue());
			})
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeBody))
			.ColorAndOpacity(FAudioFactoryTheme::GetValue())
			.Justification(ETextJustify::Center);

		if (InArgs._ShowParamName && !InArgs._ParamName.IsEmpty())
		{
			ChildSlot
			[
				SNew(SVerticalBox)

				// Param name above
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.f, 0.f, 0.f, 2.f)
				[
					SNew(STextBlock)
					.Text(InArgs._ParamName)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeBody))
					.ColorAndOpacity(FAudioFactoryTheme::GetLabel())
					.Justification(ETextJustify::Center)
				]

				// Knob dial
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					KnobWidget
				]

				// Formatted value below
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.f, 2.f, 0.f, 0.f)
				[
					ValueLabel
				]
			];
		}
		else
		{
			ChildSlot
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					KnobWidget
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.f, 2.f, 0.f, 0.f)
				[
					ValueLabel
				]
			];
		}
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		// Ctrl+Click: reset to default
		if (MouseEvent.IsControlDown() && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			OnValueChanged.ExecuteIfBound(DefaultValue);
			OnMarkDirtyDelegate.ExecuteIfBound();
			if (UObject* Obj = EditedObject.Get())
			{
				FAudioFactoryTheme::MarkObjectDirty(Obj);
			}
			return FReply::Handled();
		}
		return SCompoundWidget::OnMouseButtonDown(MyGeometry, MouseEvent);
	}

private:
	FString FormatCurrentValue() const
	{
		const float Norm = ValueAttr.Get(0.5f);
		// Map normalized [0,1] to real-unit value for display
		const float Real = MinValue + Norm * (MaxValue - MinValue);
		return AudioFactoryAudioTheme::FormatParameterValue(Real, ValueFormat);
	}

	TAttribute<float>          ValueAttr;
	float                      DefaultValue = 0.5f;
	float                      MinValue     = 0.f;
	float                      MaxValue     = 1.f;
	EAudioFactoryValueFormat   ValueFormat  = EAudioFactoryValueFormat::Default;
	FOnFloatValueChanged       OnValueChanged;
	FSimpleDelegate            OnMarkDirtyDelegate;
	TWeakObjectPtr<UObject>    EditedObject;
	TSharedPtr<SAudioMaterialKnob> InnerKnob;
};
} // namespace AudioExporter

