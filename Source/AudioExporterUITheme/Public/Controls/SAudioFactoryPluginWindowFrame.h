// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryPluginWindowTypes.h"
#include "Foundation/AudioFactoryStyle.h"
#include "HAL/PlatformProcess.h"
#include "Styling/ISlateStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#ifndef AF_UI_API
#define AF_UI_API
#endif


namespace AudioExporter {
class AF_UI_API SAudioFactoryInlineCounts : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryInlineCounts)
	{
	}
		SLATE_ATTRIBUTE(TArray<FAudioFactoryInlineCount>, Counts)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	void RebuildCountRow(const TArray<FAudioFactoryInlineCount>& Counts);
	virtual void Tick(const FGeometry& AllottedGeometry, const double CurrentTime, const float DeltaTime) override;

	TAttribute<TArray<FAudioFactoryInlineCount>> CountsAttribute;
	int32 RenderedCountNum = INDEX_NONE;
};

class AF_UI_API SAudioFactoryHeaderIconButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryHeaderIconButton)
		: _IconName(NAME_None)
		, _ToolTipText(FText::GetEmpty())
		, _IsEnabled(true)
	{
	}
		SLATE_ARGUMENT(FName, IconName)
		SLATE_ARGUMENT(FText, ToolTipText)
		SLATE_ATTRIBUTE(bool, IsEnabled)
		SLATE_ARGUMENT(TFunction<FReply()>, OnClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class AF_UI_API SAudioFactoryHeaderLabeledButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryHeaderLabeledButton)
		: _IconName(NAME_None)
		, _Label(FText::GetEmpty())
		, _ToolTipText(FText::GetEmpty())
		, _IsEnabled(true)
	{
	}
		SLATE_ARGUMENT(FName, IconName)
		SLATE_ARGUMENT(FText, Label)
		SLATE_ARGUMENT(FText, ToolTipText)
		SLATE_ATTRIBUTE(bool, IsEnabled)
		SLATE_ARGUMENT(TFunction<FReply()>, OnClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class AF_UI_API SAudioFactoryContextStrip : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryContextStrip)
	{
	}
		SLATE_NAMED_SLOT(FArguments, Content)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class AF_UI_API SAudioFactoryPathContextStrip : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryPathContextStrip)
	{
	}
		SLATE_ARGUMENT(FAudioFactoryPathContext, PathContext)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class AF_UI_API SAudioFactorySelectionSummaryStrip : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactorySelectionSummaryStrip)
	{
	}
		SLATE_ATTRIBUTE(TArray<FAudioFactoryInlineCount>, Counts)
		SLATE_NAMED_SLOT(FArguments, Tools)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class AF_UI_API SAudioFactoryUtilityIconAction : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryUtilityIconAction)
	{
	}
		SLATE_ARGUMENT(FAudioFactoryFooterAction, Action)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class AF_UI_API SAudioFactoryFooterTextAction : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryFooterTextAction)
	{
	}
		SLATE_ARGUMENT(FAudioFactoryFooterAction, Action)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class AF_UI_API SAudioFactoryPrimaryActionButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryPrimaryActionButton)
		: _ProductId(EAudioFactoryProductId::AudioExporter)
	{
	}
		SLATE_ARGUMENT(FAudioFactoryPrimaryAction, Action)
		SLATE_ARGUMENT(EAudioFactoryProductId, ProductId)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class AF_UI_API SAudioFactoryActionFooter : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryActionFooter)
	{
	}
		SLATE_NAMED_SLOT(FArguments, UtilityActions)
		SLATE_NAMED_SLOT(FArguments, CancelAction)
		SLATE_NAMED_SLOT(FArguments, PrimaryAction)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

class AF_UI_API SAudioFactoryPluginWindowFrame : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryPluginWindowFrame)
		: _ProductId(EAudioFactoryProductId::AudioExporter)
	{
	}
		SLATE_ARGUMENT(EAudioFactoryProductId, ProductId)
		SLATE_ATTRIBUTE(TArray<FAudioFactoryInlineCount>, Counts)
		SLATE_ARGUMENT(TOptional<FAudioFactoryPathContext>, PathContext)
		SLATE_ATTRIBUTE(FAudioFactoryStatus, Status)
		SLATE_ARGUMENT(TFunction<void()>, OnCancelRequested)
		SLATE_ARGUMENT(TFunction<void()>, OnPrimaryRequested)
		SLATE_NAMED_SLOT(FArguments, HeaderTools)
		SLATE_NAMED_SLOT(FArguments, ContextContent)
		SLATE_NAMED_SLOT(FArguments, Body)
		SLATE_NAMED_SLOT(FArguments, FooterUtilityActions)
		SLATE_NAMED_SLOT(FArguments, CancelAction)
		SLATE_NAMED_SLOT(FArguments, PrimaryAction)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Bubble-phase key handling (NOT OnPreviewKeyDown) so focused text fields keep their own
	// Enter/Escape handling; the frame only reacts once nothing else has consumed the key.
	virtual bool SupportsKeyboardFocus() const override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	TFunction<void()> OnCancelRequestedFunc;
	TFunction<void()> OnPrimaryRequestedFunc;
};
} // namespace AudioExporter
