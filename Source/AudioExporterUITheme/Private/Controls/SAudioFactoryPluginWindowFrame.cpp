// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Controls/SAudioFactoryPluginWindowFrame.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Foundation/AudioFactoryStyle.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
namespace AudioExporter {

namespace AudioFactoryPluginWindowPrivate
{
	inline FText ResolveText(const TAttribute<FText>& Attribute, const FText& Fallback = FText::GetEmpty())
	{
		return Attribute.IsSet() ? Attribute.Get() : Fallback;
	}

	inline const FSlateBrush* ResolveFirstAppBrush(
		const FName Primary,
		const FName Secondary = NAME_None,
		const FName Tertiary = NAME_None)
	{
		if (!Primary.IsNone())
		{
			if (const FSlateBrush* Brush = FAppStyle::GetOptionalBrush(Primary, nullptr, nullptr))
			{
				return Brush;
			}
		}
		if (!Secondary.IsNone())
		{
			if (const FSlateBrush* Brush = FAppStyle::GetOptionalBrush(Secondary, nullptr, nullptr))
			{
				return Brush;
			}
		}
		if (!Tertiary.IsNone())
		{
			if (const FSlateBrush* Brush = FAppStyle::GetOptionalBrush(Tertiary, nullptr, nullptr))
			{
				return Brush;
			}
		}

		return FStyleDefaults::GetNoBrush();
	}

	inline const FSlateBrush* FindBrushInStyle(const FName StyleName, const FName BrushName)
	{
		if (const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(StyleName))
		{
			return Style->GetOptionalBrush(BrushName, nullptr, nullptr);
		}

		return nullptr;
	}

	inline const FSlateBrush* ResolveGlyphIconBrush(const FName GlyphName)
	{
		if (GlyphName == TEXT("AudioFactory.Glyph.Reset"))
		{
			return ResolveFirstAppBrush(TEXT("Icons.Refresh"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.SelectAll"))
		{
			return ResolveFirstAppBrush(TEXT("Icons.SelectInViewport"), TEXT("EditorViewport.SelectMode"), TEXT("Icons.Plus"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.Apply"))
		{
			return ResolveFirstAppBrush(TEXT("Icons.Check"), TEXT("Icons.Success"), TEXT("Icons.Plus"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.Ok"))
		{
			return ResolveFirstAppBrush(TEXT("Icons.Check"), TEXT("Icons.Success"), TEXT("Icons.Plus"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.Report"))
		{
			return ResolveFirstAppBrush(TEXT("Icons.Documentation"), TEXT("Icons.File"), TEXT("LevelEditor.Tabs.OutputLog"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.Logs"))
		{
			if (const FSlateBrush* Brush = FAppStyle::GetOptionalBrush(TEXT("MessageLog.TabIcon"), nullptr, nullptr))
			{
				return Brush;
			}

			// Log.TabIcon is registered in the core style set, not "OutputLogStyle".
			if (const FSlateBrush* Brush = FCoreStyle::Get().GetOptionalBrush(TEXT("Log.TabIcon"), nullptr, nullptr))
			{
				return Brush;
			}

			return ResolveFirstAppBrush(TEXT("Icons.Documentation"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.Copy"))
		{
			return ResolveFirstAppBrush(TEXT("GenericCommands.Copy"), TEXT("Icons.Copy"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.History"))
		{
			return ResolveFirstAppBrush(TEXT("Icons.Refresh"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.Export") || GlyphName == TEXT("AudioFactory.Glyph.ExportPro"))
		{
			return ResolveFirstAppBrush(TEXT("Icons.Save"), TEXT("Icons.Import"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.Convert"))
		{
			return ResolveFirstAppBrush(TEXT("ClassIcon.ModularSynthPresetBank"), TEXT("Icons.Refresh"));
		}
		if (GlyphName == TEXT("AudioFactory.Glyph.Normalize"))
		{
			return ResolveFirstAppBrush(TEXT("Icons.Settings"), TEXT("Icons.Stats"));
		}
		if (!GlyphName.IsNone())
		{
			if (const FSlateBrush* Brush = FAppStyle::GetOptionalBrush(GlyphName, nullptr, nullptr))
			{
				return Brush;
			}
		}

		return ResolveFirstAppBrush(TEXT("Icons.Info"));
	}

	inline FText MiddleTruncatePath(const FText& PathText, const int32 MaxChars = 54)
	{
		if (MaxChars <= 0)
		{
			return FText::FromString(TEXT("..."));
		}

		const FString Path = PathText.ToString().TrimStartAndEnd();
		if (Path.Len() <= MaxChars)
		{
			return PathText;
		}

		FString NormalizedPath = Path.Replace(TEXT("\\"), TEXT("/"));
		while (NormalizedPath.Contains(TEXT("//")))
		{
			NormalizedPath.ReplaceInline(TEXT("//"), TEXT("/"));
		}

		TArray<FString> Segments;
		NormalizedPath.ParseIntoArray(Segments, TEXT("/"), true);
		if (Segments.IsEmpty())
		{
			return PathText;
		}

		const bool bHasDrivePrefix = Segments[0].EndsWith(TEXT(":"));
		const int32 FirstTailSegment = bHasDrivePrefix ? 1 : 0;
		const FString Prefix = bHasDrivePrefix ? Segments[0] + TEXT("/") : FString();

		auto JoinSegments = [&Segments](const int32 StartIndex)
		{
			FString Result;
			for (int32 Index = StartIndex; Index < Segments.Num(); ++Index)
			{
				if (!Result.IsEmpty())
				{
					Result += TEXT("/");
				}
				Result += Segments[Index];
			}
			return Result;
		};

		FString BestCandidate;
		for (int32 StartIndex = Segments.Num() - 1; StartIndex >= FirstTailSegment; --StartIndex)
		{
			const FString Candidate = Prefix + TEXT(".../") + JoinSegments(StartIndex);
			if (Candidate.Len() <= MaxChars)
			{
				BestCandidate = Candidate;
				continue;
			}

			if (BestCandidate.IsEmpty())
			{
				BestCandidate = Candidate;
			}
			break;
		}

		return FText::FromString(BestCandidate.IsEmpty() ? NormalizedPath : BestCandidate);
	}

	inline FReply InvokeAction(const TFunction<FReply()>& Action)
	{
		return Action ? Action() : FReply::Handled();
	}

	/**
	 * Outline style for the footer utility actions (Select All, Show Selected, Logs).
	 * "SimpleButton" renders them with no fill and no border, so they read as plain text
	 * rather than as controls. This keeps the fill transparent -- so they stay visually below
	 * Cancel (filled secondary) and the accented primary -- and adds a quiet hairline outline
	 * that gives them a button silhouette. Function-local static: the tokens resolve live style
	 * colors, so this must not run at static-init time.
	 */
	inline const FButtonStyle& GetFooterOutlineButtonStyle()
	{
		static const FButtonStyle Style = []()
		{
			const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
			const FLinearColor Outline = FMath::Lerp(Tokens.Surface, Tokens.TextPrimary, 0.16f);
			const FLinearColor OutlineHover = FMath::Lerp(Tokens.Surface, Tokens.TextPrimary, 0.30f);
			const FLinearColor OutlineDisabled = FMath::Lerp(Tokens.Surface, Tokens.TextPrimary, 0.08f);
			const FLinearColor HoverFill = FMath::Lerp(Tokens.Surface, Tokens.TextPrimary, 0.10f);
			const FLinearColor PressedFill = FMath::Lerp(Tokens.Surface, FLinearColor::Black, 0.35f);

			FButtonStyle Result = FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton");
			Result.Normal   = FSlateRoundedBoxBrush(FLinearColor::Transparent, 4.0f, Outline, 1.0f);
			Result.Hovered  = FSlateRoundedBoxBrush(HoverFill, 4.0f, OutlineHover, 1.0f);
			Result.Pressed  = FSlateRoundedBoxBrush(PressedFill, 4.0f, OutlineHover, 1.0f);
			Result.Disabled = FSlateRoundedBoxBrush(FLinearColor::Transparent, 4.0f, OutlineDisabled, 1.0f);
			return Result;
		}();

		return Style;
	}

	inline const FSlateBrush* ResolveInlineCountIconBrush(const FName IconName)
	{
		if (IconName.IsNone())
		{
			return nullptr;
		}

		if (IconName == FAudioFactoryClassIconNames::MetaSoundSource())
		{
			if (const FSlateBrush* Brush = FindBrushInStyle(TEXT("MetaSoundStyle"), IconName))
			{
				return Brush;
			}

			if (const FSlateBrush* Brush = FindBrushInStyle(TEXT("AudioInsights"), TEXT("AudioInsights.Icon.SoundDashboard.MetaSound")))
			{
				return Brush;
			}
		}

		if (IconName == FAudioFactoryClassIconNames::SoundWave())
		{
			// The AudioInsights dashboard brush is the only genuine SoundWave icon the engine
			// ships; the app-style names below are lookalike fallbacks, so they must come last.
			if (const FSlateBrush* Brush = FindBrushInStyle(TEXT("AudioInsights"), TEXT("AudioInsights.Icon.SoundDashboard.SoundWave")))
			{
				return Brush;
			}

			if (const FSlateBrush* Brush = FAppStyle::GetOptionalBrush(TEXT("ClassIcon.ModularSynthPresetBank"), nullptr, nullptr))
			{
				return Brush;
			}

			if (const FSlateBrush* Brush = FAppStyle::GetOptionalBrush(TEXT("ClassIcon.SoundCue"), nullptr, nullptr))
			{
				return Brush;
			}
		}

		return FAppStyle::GetOptionalBrush(IconName, nullptr, FStyleDefaults::GetNoBrush());
	}
}

void SAudioFactoryInlineCounts::Construct(const FArguments& InArgs)
{
	CountsAttribute = InArgs._Counts;
	RebuildCountRow(CountsAttribute.Get());
}

void SAudioFactoryInlineCounts::Tick(const FGeometry& AllottedGeometry, const double CurrentTime, const float DeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);

	const TArray<FAudioFactoryInlineCount> CurrentCounts = CountsAttribute.Get();
	if (CurrentCounts.Num() != RenderedCountNum)
	{
		// If the bound count list changes size, refresh only this local row before the next paint.
		RebuildCountRow(CurrentCounts);
	}
}

void SAudioFactoryInlineCounts::RebuildCountRow(const TArray<FAudioFactoryInlineCount>& Counts)
{
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	TSharedRef<SHorizontalBox> CountRow = SNew(SHorizontalBox);
	const TAttribute<TArray<FAudioFactoryInlineCount>> BoundCounts = CountsAttribute;

	for (int32 Index = 0; Index < Counts.Num(); ++Index)
	{
		const FAudioFactoryInlineCount Count = Counts[Index];
		const FSlateBrush* IconBrush = AudioFactoryPluginWindowPrivate::ResolveInlineCountIconBrush(Count.IconName);

		CountRow->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(Index > 0 ? FMargin(16.0f, 0.0f, 0.0f, 0.0f) : FMargin(0.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, IconBrush == nullptr ? 0.0f : 6.0f, 0.0f)
			[
					IconBrush == nullptr
						? StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
						: StaticCastSharedRef<SWidget>(
							SNew(SImage)
							.Image(IconBrush)
							.ColorAndOpacity_Lambda([BoundCounts, Index, Tokens]()
							{
								const TArray<FAudioFactoryInlineCount> CurrentCounts = BoundCounts.Get();
								if (!CurrentCounts.IsValidIndex(Index))
								{
									// If the bound list shrinks before this row is rebuilt, render the icon disabled.
									return FSlateColor(Tokens.TextDisabled);
								}
								return FSlateColor(CurrentCounts[Index].IsEnabled.Get(true) ? Tokens.TextMuted : Tokens.TextDisabled);
							}))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
					SNew(STextBlock)
					.Text_Lambda([BoundCounts, Index]()
					{
						const TArray<FAudioFactoryInlineCount> CurrentCounts = BoundCounts.Get();
						if (!CurrentCounts.IsValidIndex(Index))
						{
							// If the bound list shrinks before this row is rebuilt, leave the label unavailable.
							return FText::GetEmpty();
						}
						return CurrentCounts[Index].Label;
					})
					.ToolTipText_Lambda([BoundCounts, Index]()
					{
						const TArray<FAudioFactoryInlineCount> CurrentCounts = BoundCounts.Get();
						if (!CurrentCounts.IsValidIndex(Index))
						{
							// If the bound list shrinks before this row is rebuilt, leave the tooltip unavailable.
							return FText::GetEmpty();
						}
						return CurrentCounts[Index].ToolTip;
					})
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity_Lambda([BoundCounts, Index, Tokens]()
					{
						const TArray<FAudioFactoryInlineCount> CurrentCounts = BoundCounts.Get();
						if (!CurrentCounts.IsValidIndex(Index))
						{
							// If the bound list shrinks before this row is rebuilt, render the label disabled.
							return FSlateColor(Tokens.TextDisabled);
						}
						return FSlateColor(CurrentCounts[Index].IsEnabled.Get(true) ? Tokens.TextMuted : Tokens.TextDisabled);
					})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(5.0f, 0.0f, 0.0f, 0.0f)
			[
					SNew(STextBlock)
					.Text_Lambda([BoundCounts, Index]()
					{
						const TArray<FAudioFactoryInlineCount> CurrentCounts = BoundCounts.Get();
						if (!CurrentCounts.IsValidIndex(Index))
						{
							// If the bound list shrinks before this row is rebuilt, leave the value unavailable.
							return FText::GetEmpty();
						}
						return AudioFactoryPluginWindowPrivate::ResolveText(CurrentCounts[Index].Value);
					})
					.ToolTipText_Lambda([BoundCounts, Index]()
					{
						const TArray<FAudioFactoryInlineCount> CurrentCounts = BoundCounts.Get();
						if (!CurrentCounts.IsValidIndex(Index))
						{
							// If the bound list shrinks before this row is rebuilt, leave the tooltip unavailable.
							return FText::GetEmpty();
						}
						return CurrentCounts[Index].ToolTip;
					})
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity_Lambda([BoundCounts, Index, Tokens]()
					{
						const TArray<FAudioFactoryInlineCount> CurrentCounts = BoundCounts.Get();
						if (!CurrentCounts.IsValidIndex(Index))
						{
							// If the bound list shrinks before this row is rebuilt, render the value disabled.
							return FSlateColor(Tokens.TextDisabled);
						}
						return FSlateColor(CurrentCounts[Index].IsEnabled.Get(true) ? Tokens.TextPrimary : Tokens.TextDisabled);
					})
			]
		];
	}

	ChildSlot
	[
		CountRow
	];
	RenderedCountNum = Counts.Num();
}

void SAudioFactoryHeaderIconButton::Construct(const FArguments& InArgs)
{
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	const FName IconName = InArgs._IconName;
	const TFunction<FReply()> OnClicked = InArgs._OnClicked;

	ChildSlot
	[
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.ToolTipText(InArgs._ToolTipText)
		.IsEnabled(InArgs._IsEnabled)
		.ContentPadding(FMargin(8.0f, 4.0f))
		.OnClicked_Lambda([OnClicked]()
		{
			return AudioFactoryPluginWindowPrivate::InvokeAction(OnClicked);
		})
		[
			SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.WidthOverride(14.0f)
			.HeightOverride(14.0f)
			[
				SNew(SImage)
				.Image(AudioFactoryPluginWindowPrivate::ResolveGlyphIconBrush(IconName))
				.ColorAndOpacity(FSlateColor(Tokens.TextMuted))
			]
		]
	];
}

void SAudioFactoryHeaderLabeledButton::Construct(const FArguments& InArgs)
{
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	const FName IconName = InArgs._IconName;
	const TFunction<FReply()> OnClicked = InArgs._OnClicked;

	ChildSlot
	[
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.ToolTipText(InArgs._ToolTipText)
		.IsEnabled(InArgs._IsEnabled)
		.ContentPadding(FMargin(8.0f, 4.0f))
		.OnClicked_Lambda([OnClicked]()
		{
			return AudioFactoryPluginWindowPrivate::InvokeAction(OnClicked);
		})
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 5.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(14.0f)
				.HeightOverride(14.0f)
				[
					SNew(SImage)
					.Image(AudioFactoryPluginWindowPrivate::ResolveGlyphIconBrush(IconName))
					.ColorAndOpacity(FSlateColor(Tokens.TextMuted))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(InArgs._Label)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor(Tokens.TextMuted))
			]
		]
	];
}

void SAudioFactoryContextStrip::Construct(const FArguments& InArgs)
{
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(Tokens.Surface)
		.Padding(FMargin(10.0f, 4.0f))
		[
			SNew(SBox)
			.MinDesiredHeight(Tokens.ContextStripHeight)
			.VAlign(VAlign_Center)
			[
				InArgs._Content.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._Content.Widget
			]
		]
	];
}

void SAudioFactoryPathContextStrip::Construct(const FArguments& InArgs)
{
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	const FAudioFactoryPathContext PathContext = InArgs._PathContext;

	ChildSlot
	[
		SNew(SAudioFactoryContextStrip)
		.Content()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(PathContext.Label)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor(Tokens.TextMuted))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SEditableTextBox)
				.IsReadOnly(true)
				// Display is middle-truncated for long paths; the tooltip below always carries
				// the full, untruncated path and is the canonical source for the real value.
				.Text_Lambda([PathContext]()
				{
					return AudioFactoryPluginWindowPrivate::MiddleTruncatePath(
						AudioFactoryPluginWindowPrivate::ResolveText(PathContext.Path));
				})
				.ToolTipText_Lambda([PathContext]()
				{
					return PathContext.ToolTip.IsEmpty()
						? AudioFactoryPluginWindowPrivate::ResolveText(PathContext.Path)
						: PathContext.ToolTip;
				})
				.Font(FAppStyle::GetFontStyle("SmallFont"))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(NSLOCTEXT("AudioFactoryPluginWindow", "PathContextChangeFolderTooltip", "Change the output folder."))
				.ContentPadding(FMargin(8.0f, 4.0f))
				.OnClicked_Lambda([PathContext]()
				{
					return AudioFactoryPluginWindowPrivate::InvokeAction(PathContext.OnAction);
				})
				[
					SNew(SBox)
					.WidthOverride(14.0f)
					.HeightOverride(14.0f)
					[
						SNew(SImage)
						.Image(AudioFactoryPluginWindowPrivate::ResolveFirstAppBrush(
							TEXT("Icons.FolderClosed"), TEXT("Icons.FolderOpen"), TEXT("Icons.Folder")))
						.ColorAndOpacity(FSlateColor(Tokens.TextMuted))
					]
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				PathContext.OnResetToDefault
					? StaticCastSharedRef<SWidget>(
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ToolTipText(NSLOCTEXT("AudioFactoryPluginWindow", "PathContextResetTooltip", "Reset to Default"))
						.Visibility_Lambda([PathContext]()
						{
							return PathContext.IsResetToDefaultVisible.Get(false)
								? EVisibility::Visible
								: EVisibility::Collapsed;
						})
						.ContentPadding(FMargin(4.0f))
						.OnClicked_Lambda([PathContext]()
						{
							return AudioFactoryPluginWindowPrivate::InvokeAction(PathContext.OnResetToDefault);
						})
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
						])
					: StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
			]
		]
	];
}

void SAudioFactorySelectionSummaryStrip::Construct(const FArguments& InArgs)
{
	const TAttribute<TArray<FAudioFactoryInlineCount>> Counts = InArgs._Counts;
	const bool bHasCounts = !Counts.Get().IsEmpty();

	ChildSlot
	[
		SNew(SAudioFactoryContextStrip)
		.Content()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				!bHasCounts
					? StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
					: StaticCastSharedRef<SWidget>(
						SNew(SAudioFactoryInlineCounts)
						.Counts(Counts))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				InArgs._Tools.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._Tools.Widget
			]
		]
	];
}

void SAudioFactoryUtilityIconAction::Construct(const FArguments& InArgs)
{
	const FAudioFactoryFooterAction Action = InArgs._Action;
	ChildSlot
	[
		SNew(SAudioFactoryHeaderIconButton)
		.IconName(Action.IconName)
		.ToolTipText(Action.ToolTip)
		.IsEnabled(Action.IsEnabled.IsSet() ? Action.IsEnabled : TAttribute<bool>(Action.bIsEnabled))
		.OnClicked(Action.OnClicked)
	];
}

void SAudioFactoryFooterTextAction::Construct(const FArguments& InArgs)
{
	const FAudioFactoryFooterAction Action = InArgs._Action;
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	const FText Label = Action.bUseLabel && !Action.Label.IsEmpty() ? Action.Label : Action.ToolTip;

	// Footer actions carry an IconName; the v1 footer text action ignored it, so the field was
	// dead plumbing. Render the resolved 14x14 glyph next to the label when one is set.
	const FSlateBrush* IconBrush = Action.IconName.IsNone()
		? nullptr
		: AudioFactoryPluginWindowPrivate::ResolveGlyphIconBrush(Action.IconName);

	TSharedRef<SHorizontalBox> ButtonContent = SNew(SHorizontalBox);
	if (IconBrush != nullptr)
	{
		ButtonContent->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 5.0f, 0.0f)
		[
			SNew(SBox)
			.WidthOverride(14.0f)
			.HeightOverride(14.0f)
			[
				SNew(SImage)
				.Image(IconBrush)
				.ColorAndOpacity(FSlateColor(Tokens.TextMuted))
			]
		];
	}

	ButtonContent->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Text(Label)
		.Font(FAppStyle::GetFontStyle("SmallFont"))
		.ColorAndOpacity(FSlateColor(Tokens.TextMuted))
	];

	ChildSlot
	[
		SNew(SButton)
		.ButtonStyle(&AudioFactoryPluginWindowPrivate::GetFooterOutlineButtonStyle())
		.IsEnabled(Action.IsEnabled.IsSet() ? Action.IsEnabled : TAttribute<bool>(Action.bIsEnabled))
		.ToolTipText(Action.ToolTip)
		.ContentPadding(FMargin(10.0f, 4.0f))
		.OnClicked_Lambda([Action]()
		{
			return AudioFactoryPluginWindowPrivate::InvokeAction(Action.OnClicked);
		})
		[
			ButtonContent
		]
	];
}

void SAudioFactoryPrimaryActionButton::Construct(const FArguments& InArgs)
{
	const FAudioFactoryPrimaryAction Action = InArgs._Action;
	const TAttribute<bool> IsEnabled = Action.IsEnabled.IsSet() ? Action.IsEnabled : TAttribute<bool>(Action.bIsEnabled);
	const FButtonStyle& ProductButtonStyle = FAudioFactoryProductStyles::GetPrimaryButtonStyle(InArgs._ProductId);

	ChildSlot
	[
		SNew(SBox)
		.MinDesiredWidth(88.0f)
		[
			SNew(SButton)
			.ButtonStyle(&ProductButtonStyle)
			.HAlign(HAlign_Center)
			.IsEnabled(IsEnabled)
			.ToolTipText(Action.ToolTip)
			.ContentPadding(FMargin(20.0f, 5.0f))
			.Text(Action.Label)
			.TextStyle(&FAppStyle::Get(), "PrimaryButtonText")
			.OnClicked_Lambda([Action]()
			{
				return AudioFactoryPluginWindowPrivate::InvokeAction(Action.OnClicked);
			})
		]
	];
}

void SAudioFactoryActionFooter::Construct(const FArguments& InArgs)
{
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(Tokens.Surface)
		.Padding(FMargin(10.0f, 0.0f))
		[
			SNew(SBox)
			.HeightOverride(Tokens.FooterHeight)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					InArgs._UtilityActions.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._UtilityActions.Widget
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					InArgs._CancelAction.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._CancelAction.Widget
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					InArgs._PrimaryAction.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._PrimaryAction.Widget
				]
			]
		]
	];
}

void SAudioFactoryPluginWindowFrame::Construct(const FArguments& InArgs)
{
	const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
	const bool bHasCustomContext = InArgs._ContextContent.Widget != SNullWidget::NullWidget;
	const bool bHasPathContext = InArgs._PathContext.IsSet();
	const bool bHasSelectionSummary = !InArgs._Counts.Get().IsEmpty() || InArgs._HeaderTools.Widget != SNullWidget::NullWidget;

	OnCancelRequestedFunc = InArgs._OnCancelRequested;
	OnPrimaryRequestedFunc = InArgs._OnPrimaryRequested;

	// 1px divider between the details body and the strip stack below it, and again directly
	// above the footer.
	auto MakeDivider = [&Tokens]() -> TSharedRef<SWidget>
	{
		return SNew(SBox)
			.HeightOverride(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(Tokens.DividerColor)
				.Padding(0.0f)
			];
	};

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(Tokens.DividerColor)
		.Padding(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(Tokens.Surface)
			.Padding(0.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					InArgs._Body.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._Body.Widget
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					MakeDivider()
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					bHasCustomContext
						? StaticCastSharedRef<SWidget>(
							SNew(SAudioFactoryContextStrip)
							.Content()
							[
								InArgs._ContextContent.Widget
							])
						: StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					bHasPathContext
						? StaticCastSharedRef<SWidget>(
							SNew(SAudioFactoryPathContextStrip)
							.PathContext(InArgs._PathContext.GetValue()))
						: StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					bHasSelectionSummary
						? StaticCastSharedRef<SWidget>(
							SNew(SAudioFactorySelectionSummaryStrip)
							.Counts(InArgs._Counts)
							.Tools()
							[
								InArgs._HeaderTools.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._HeaderTools.Widget
							])
						: StaticCastSharedRef<SWidget>(SNullWidget::NullWidget)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					MakeDivider()
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SAudioFactoryActionFooter)
					.UtilityActions()
					[
						InArgs._FooterUtilityActions.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._FooterUtilityActions.Widget
					]
					.CancelAction()
					[
						InArgs._CancelAction.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._CancelAction.Widget
					]
					.PrimaryAction()
					[
						InArgs._PrimaryAction.Widget == SNullWidget::NullWidget ? SNullWidget::NullWidget : InArgs._PrimaryAction.Widget
					]
				]
			]
		]
	];
}

bool SAudioFactoryPluginWindowFrame::SupportsKeyboardFocus() const
{
	return true;
}

FReply SAudioFactoryPluginWindowFrame::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape && OnCancelRequestedFunc)
	{
		OnCancelRequestedFunc();
		return FReply::Handled();
	}

	if (InKeyEvent.GetKey() == EKeys::Enter && OnPrimaryRequestedFunc)
	{
		OnPrimaryRequestedFunc();
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

} // namespace AudioExporter
