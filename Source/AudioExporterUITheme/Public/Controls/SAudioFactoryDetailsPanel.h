// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Controls/AudioFactoryParamSpec.h"
#include "Controls/SAudioFactoryButton.h"
#include "Controls/SAudioFactoryCollapsibleSection.h"
#include "Controls/SAudioFactoryDropdown.h"
#include "Controls/SAudioFactoryParamRow.h"
#include "Controls/SAudioFactoryPathPicker.h"
#include "Controls/SAudioFactorySliderStyle.h"
#include "Controls/SAudioFactoryToggle.h"
#include "Foundation/AudioFactoryConstants.h"
#include "Foundation/AudioFactoryStyle.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

/**
 * SAudioFactoryDetailsPanel
 *
 * Data-driven settings panel -- the settings-dialog equivalent of IDetailsView
 * without UObject reflection overhead.
 *
 * Feed it an array of FAudioFactorySectionSpec (each containing FAudioFactoryParamSpec
 * rows) and it renders an identical, layout-consistent panel for every plugin.
 *
 * CUSTOM LAYOUT NOTE: Exotic param layouts (for example paired sliders or a
 * naming-template preview) must remain as custom section builders and
 * are placed outside this panel -- annotate them with:
 *   // CUSTOM: paired layout, not expressible in SAudioFactoryDetailsPanel
 *
 * Acceptance cap: <= 200 lines. Exotic cases stay outside.
 */
namespace AudioExporter {
class AF_UI_API SAudioFactoryDetailsPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryDetailsPanel) : _LabelWidth(0.f) {}
		/** Array of section descriptors -- the panel owns rendering; callers own data binding. */
		SLATE_ARGUMENT(TArray<FAudioFactorySectionSpec>, Sections)
		/** Override the label column width. 0 = use FAudioFactoryConstants::LabelColumnWidth. */
		SLATE_ARGUMENT(float, LabelWidth)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		const float LW = (InArgs._LabelWidth > 0.f)
			? InArgs._LabelWidth
			: FAudioFactoryStyle::Get().Tokens().Columns.Label;

		TSharedRef<SVerticalBox> Root = SNew(SVerticalBox);

		for (const FAudioFactorySectionSpec& Section : InArgs._Sections)
		{
			Root->AddSlot()
				.AutoHeight()
				[BuildSection(Section, LW)];
		}

		ChildSlot[Root];
	}

private:
	// -- Section --------------------------------------------------------------

	TSharedRef<SWidget> BuildSection(const FAudioFactorySectionSpec& Section, float LW)
	{
		TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
		for (const FAudioFactoryParamSpec& P : Section.Params)
		{
			Rows->AddSlot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, FAudioFactoryConstants::ParamRowGap)
				[BuildParamRow(P, LW)];
		}

		return SNew(SAudioFactoryCollapsibleSection)
			.SectionLabel(Section.Label)
			.bDefaultExpanded(Section.bDefaultExpanded)
			.bUpperCaseLabel(true)  // all detail-panel sections render labels in ALL CAPS
			[Rows];
	}

	// -- Param Row -------------------------------------------------------------

	TSharedRef<SWidget> BuildParamRow(const FAudioFactoryParamSpec& P, float LW)
	{
		if (P.Control == EAFParamControl::Separator)
		{
			return SNew(SBox)
				.Padding(FMargin(0.f, FAudioFactoryConstants::SpacingXS))
				.HeightOverride(1.f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FAudioFactoryTheme::GetDivider())
				];
		}

		const float SubIndent = P.bIsSubParam ? FAudioFactoryConstants::SpacingL : 0.f;

		return SNew(SBox)
			.Padding(FMargin(SubIndent, 0.f, 0.f, 0.f))
			.IsEnabled(P.bIsEnabled)
			[
				SNew(SAudioFactoryParamRow)
				.Label(P.Label)
				.HelpText(P.ToolTip)
				.LabelWidth(FMath::Max(0.f, LW - SubIndent))
				[BuildControl(P)]
			];
	}

	// -- Control factory -------------------------------------------------------

	TSharedRef<SWidget> BuildControl(const FAudioFactoryParamSpec& P)
	{
		switch (P.Control)
		{
		case EAFParamControl::Toggle:
			return BuildToggle(P);
		case EAFParamControl::RadioGroup:
			return BuildRadio(P);
		case EAFParamControl::Dropdown:
			return BuildDropdown(P);
		case EAFParamControl::SliderFloat:
			return BuildSlider(P);
		case EAFParamControl::PathPicker:
			return BuildPath(P);
		case EAFParamControl::ReadOnlyText:
			return BuildReadOnly(P);
		case EAFParamControl::FreeText:
			return BuildFreeText(P);
		default:
			ensureMsgf(false, TEXT("%s: unknown EAFParamControl value %d"),
				*FString(__FUNCTION__), (int32)P.Control);
			return SNullWidget::NullWidget;
		}
	}

	// -- Individual control builders -------------------------------------------

	TSharedRef<SWidget> BuildToggle(const FAudioFactoryParamSpec& P)
	{
		return SNew(SAudioFactoryToggle)
			.bChecked(P.BoolValue.Get(false))
			.OnToggleChanged(FOnAudioFactoryToggleChanged::CreateLambda(
				[Cb = P.OnBoolChanged](bool bNew) { if (Cb) { Cb(bNew); } }));
	}

	TSharedRef<SWidget> BuildRadio(const FAudioFactoryParamSpec& P)
	{
		return SNew(SAudioFactoryButton)
			.Options(P.Options)
			.SelectedIndex(P.IntValue)
			.OnSelectionChanged(FOnAudioFactoryButtonSelectionChanged::CreateLambda(
				[Cb = P.OnIntChanged](int32 Idx) { if (Cb) { Cb(Idx); } }));
	}

	TSharedRef<SWidget> BuildDropdown(const FAudioFactoryParamSpec& P)
	{
		return SNew(SAudioFactoryDropdown)
			.Options(P.Options)
			.SelectedIndex(P.IntValue)
			.OnSelectionChanged(FOnAudioFactoryDropdownSelectionChanged::CreateLambda(
				[Cb = P.OnIntChanged](int32 Idx) { if (Cb) { Cb(Idx); } }));
	}

	TSharedRef<SWidget> BuildSlider(const FAudioFactoryParamSpec& P)
	{
		static const FSliderStyle SliderStyle = FAudioFactorySliderStyle::Make();

		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SSlider)
				.Style(&SliderStyle)
				.Value(P.FloatValue)
				.MinValue(P.FloatMin)
				.MaxValue(P.FloatMax)
				.OnValueChanged(FOnFloatValueChanged::CreateLambda(
					[Cb = P.OnFloatChanged](float V) { if (Cb) { Cb(V); } }))
			];

		if (P.FloatDisplay.IsBound() || P.FloatDisplay.IsSet())
		{
			Row->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FAudioFactoryConstants::SpacingS, 0.f, 0.f, 0.f)
				[
					SNew(SBox).WidthOverride(48.f).HAlign(HAlign_Right)
					[
						SNew(STextBlock)
						.Text(P.FloatDisplay)
						.Font(FCoreStyle::GetDefaultFontStyle(
							"Regular", FAudioFactoryConstants::FontSizeSmall))
						.ColorAndOpacity(FSlateColor(FAudioFactoryTheme::GetValue()))
					]
				];
		}
		return Row;
	}

	TSharedRef<SWidget> BuildPath(const FAudioFactoryParamSpec& P)
	{
		return SNew(SAudioFactoryPathPicker)
			.PathText(P.PathText)
			.OnBrowseClicked(FOnClicked::CreateLambda(
				[Cb = P.OnBrowsePath]() -> FReply { if (Cb) { Cb(); } return FReply::Handled(); }));
	}

	TSharedRef<SWidget> BuildReadOnly(const FAudioFactoryParamSpec& P)
	{
		const TAttribute<FSlateColor> Col = P.ReadOnlyColor.IsSet()
			? P.ReadOnlyColor
			: TAttribute<FSlateColor>(FSlateColor(FAudioFactoryTheme::GetValue()));

		return SNew(STextBlock)
			.Text(P.ReadOnlyText)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeSmall))
			.ColorAndOpacity(Col);
	}

	TSharedRef<SWidget> BuildFreeText(const FAudioFactoryParamSpec& P)
	{
		return SNew(SEditableTextBox)
			.Text(P.FreeTextValue)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeSmall))
			.ForegroundColor(FSlateColor(FAudioFactoryTheme::GetLabel()))
			.BackgroundColor(FAudioFactoryTheme::GetSurfaceRaised())
			.OnTextCommitted(FOnTextCommitted::CreateLambda(
				[Cb = P.OnFreeTextChanged](const FText& T, ETextCommit::Type)
				{ if (Cb) { Cb(T.ToString()); } }));
	}
};
} // namespace AudioExporter

