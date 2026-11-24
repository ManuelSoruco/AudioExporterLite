// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryConstants.h"
#include "Foundation/AudioFactoryPluginWindowTypes.h"
#include "Foundation/AudioFactoryStyle.h"
#include "Foundation/AudioFactoryTheme.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

namespace AudioExporter {
namespace AudioFactorySelectedAssetsWindowPrivate
{
	static constexpr float RowHeight = 26.0f;
	static constexpr float IconSize = 14.0f;
	static constexpr float TitleBarHeight = 26.0f;

	inline FName AssetColumn()
	{
		return TEXT("Asset");
	}

	inline FName ClassColumn()
	{
		return TEXT("Class");
	}

	inline FName PathColumn()
	{
		return TEXT("Path");
	}

	inline FName StatusColumn()
	{
		return TEXT("Status");
	}

	inline FText CompactPathForCell(const FText& PathText, const int32 MaxChars = 72)
	{
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
		if (Segments.Num() <= 2)
		{
			return FText::FromString(NormalizedPath);
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

	inline const FSlateBrush* ResolveAssetIcon(const FAudioFactorySelectedAsset& Asset)
	{
		FName IconName = Asset.IconName;
		if (IconName.IsNone())
		{
			const FString ClassLower = Asset.AssetClass.ToString().ToLower();
			if (ClassLower.Contains(TEXT("metasound")))
			{
				IconName = FAudioFactoryClassIconNames::MetaSoundSource();
			}
			else if (ClassLower.Contains(TEXT("soundwave")))
			{
				IconName = FAudioFactoryClassIconNames::SoundWave();
			}
			else if (ClassLower.Contains(TEXT("soundcue")))
			{
				IconName = FAudioFactoryClassIconNames::SoundCue();
			}
		}

		if (IconName == FAudioFactoryClassIconNames::MetaSoundSource())
		{
			if (const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(TEXT("MetaSoundStyle")))
			{
				if (const FSlateBrush* Brush = Style->GetOptionalBrush(IconName, nullptr, nullptr))
				{
					return Brush;
				}
			}

			if (const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(TEXT("AudioInsights")))
			{
				if (const FSlateBrush* Brush = Style->GetOptionalBrush(TEXT("AudioInsights.Icon.SoundDashboard.MetaSound"), nullptr, nullptr))
				{
					return Brush;
				}
			}
		}

		if (IconName == FAudioFactoryClassIconNames::SoundWave())
		{
			// The AudioInsights dashboard brush is the only genuine SoundWave icon the engine
			// ships; the app-style names below are lookalike fallbacks, so they must come last.
			if (const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(TEXT("AudioInsights")))
			{
				if (const FSlateBrush* Brush = Style->GetOptionalBrush(TEXT("AudioInsights.Icon.SoundDashboard.SoundWave"), nullptr, nullptr))
				{
					return Brush;
				}
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

		if (!IconName.IsNone())
		{
			if (const FSlateBrush* Brush = FAppStyle::GetOptionalBrush(IconName, nullptr, nullptr))
			{
				return Brush;
			}
		}
		return FStyleDefaults::GetNoBrush();
	}

	class SSelectedAssetRow : public SMultiColumnTableRow<TSharedPtr<FAudioFactorySelectedAsset>>
	{
	public:
		using FAssetPtr = TSharedPtr<FAudioFactorySelectedAsset>;

		SLATE_BEGIN_ARGS(SSelectedAssetRow)
		{
		}
			SLATE_ARGUMENT(FAssetPtr, Asset)
			SLATE_ARGUMENT(bool, bShowStatusColumn)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
		{
			Asset = InArgs._Asset;
			bShowStatusColumn = InArgs._bShowStatusColumn;
			SMultiColumnTableRow<FAssetPtr>::Construct(SMultiColumnTableRow::FArguments(), InOwnerTable);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
		{
			const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();
			if (!Asset.IsValid())
			{
				return MakeTextCell(FText::GetEmpty(), FText::GetEmpty(), Tokens.Surface, Tokens.TextMuted);
			}

			const FAudioFactorySelectedAsset& Item = *Asset.Get();
			const bool bIncluded = Item.IsIncluded();
			const FLinearColor RowTextColor = bIncluded ? Tokens.TextPrimary : Tokens.TextMuted;
			const FLinearColor RowIconColor = bIncluded ? Tokens.TextMuted : Tokens.TextDisabled;
			// Excluded rows recess (darker), matching how the engine dims de-emphasized content.
			// SurfaceAlt is the header chrome color and now sits LIGHTER than Surface, so using it
			// here would make excluded rows pop instead of fade.
			const FLinearColor RowBgColor = bIncluded ? Tokens.Surface : Tokens.Recessed;
			const FText StatusText = bIncluded
				? NSLOCTEXT("AudioFactorySelectedAssetsWindow", "IncludedStatus", "Included")
				: NSLOCTEXT("AudioFactorySelectedAssetsWindow", "ExcludedStatus", "Excluded");
			const FLinearColor StatusColor = bIncluded ? Tokens.StatusReady : Tokens.StatusWarning;
			const FText AssetToolTip = Item.Reason.IsEmpty() ? Item.DisplayName : Item.Reason;

			if (ColumnName == AssetColumn())
			{
				return MakeCell(RowBgColor,
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SBox)
						.WidthOverride(IconSize)
						.HeightOverride(IconSize)
						[
							SNew(SImage)
							.Image(ResolveAssetIcon(Item))
							.ColorAndOpacity(FSlateColor(RowIconColor))
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						MakeTextBlock(Item.DisplayName, AssetToolTip, FAppStyle::GetFontStyle("SmallFontBold"), RowTextColor)
					]);
			}

			if (ColumnName == ClassColumn())
			{
				return MakeTextCell(Item.AssetClass, Item.AssetClass, RowBgColor, RowTextColor);
			}

			if (ColumnName == PathColumn())
			{
				return MakeTextCell(CompactPathForCell(Item.AssetPath), Item.AssetPath, RowBgColor, RowTextColor);
			}

			if (bShowStatusColumn && ColumnName == StatusColumn())
			{
				// Excluded rows keep their skip reason (e.g. Normalizer's notes) on hover;
				// included rows fall back to the status text itself ("Included").
				const FText StatusToolTip = Item.Reason.IsEmpty() ? StatusText : Item.Reason;
				return MakeTextCell(StatusText, StatusToolTip, RowBgColor, StatusColor, FAppStyle::GetFontStyle("SmallFontBold"));
			}

			return MakeTextCell(FText::GetEmpty(), FText::GetEmpty(), RowBgColor, RowTextColor);
		}

	private:
		static TSharedRef<STextBlock> MakeTextBlock(
			const FText& Text,
			const FText& ToolTip,
			const FSlateFontInfo& Font,
			const FLinearColor& TextColor)
		{
			return SNew(STextBlock)
				.Text(Text)
				.ToolTipText(ToolTip)
				.Font(Font)
				.ColorAndOpacity(FSlateColor(TextColor))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
		}

		static TSharedRef<SWidget> MakeCell(const FLinearColor& BackgroundColor, const TSharedRef<SWidget>& Content)
		{
			return SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(BackgroundColor)
				.Padding(FMargin(0.0f))
				[
					SNew(SBox)
					.HeightOverride(RowHeight)
					.MinDesiredWidth(0.0f)
					.VAlign(VAlign_Center)
					[
						Content
					]
				];
		}

		static TSharedRef<SWidget> MakeTextCell(
			const FText& Text,
			const FText& ToolTip,
			const FLinearColor& BackgroundColor,
			const FLinearColor& TextColor,
			const FSlateFontInfo& Font = FAppStyle::GetFontStyle("SmallFont"))
		{
			return MakeCell(BackgroundColor,
				SNew(SBox)
				.MinDesiredWidth(0.0f)
				.Padding(FMargin(8.0f, 0.0f))
				.VAlign(VAlign_Center)
				[
					MakeTextBlock(Text, ToolTip, Font, TextColor)
				]);
		}

		FAssetPtr Asset;
		bool bShowStatusColumn = false;
	};
}

class AF_UI_API SAudioFactorySelectedAssetsWindow : public SCompoundWidget
{
public:
	using FAssetPtr = TSharedPtr<FAudioFactorySelectedAsset>;

	SLATE_BEGIN_ARGS(SAudioFactorySelectedAssetsWindow)
		: _Title(NSLOCTEXT("AudioFactorySelectedAssetsWindow", "DefaultTitle", "Selected Assets"))
		, _Assets()
		, _bShowSearch(true)
		, _bShowStatusColumn(false)
	{
	}
		SLATE_ARGUMENT(FText, Title)
		SLATE_ARGUMENT(TArray<FAudioFactorySelectedAsset>, Assets)
		SLATE_ARGUMENT(bool, bShowSearch)
		SLATE_ARGUMENT(bool, bShowStatusColumn)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		const FAudioFactoryPluginWindowTokens& Tokens = FAudioFactoryStyle::Get().PluginWindowTokens();

		SearchEnabled = InArgs._bShowSearch;
		bShowStatusColumn = InArgs._bShowStatusColumn;
		RawAssets.Empty(InArgs._Assets.Num());
		for (const FAudioFactorySelectedAsset& Asset : InArgs._Assets)
		{
			RawAssets.Add(MakeShared<FAudioFactorySelectedAsset>(Asset));
		}
		FilterAssets();

		TSharedRef<SHeaderRow> HeaderRow = SNew(SHeaderRow)
			+ SHeaderRow::Column(AudioFactorySelectedAssetsWindowPrivate::AssetColumn())
			.DefaultLabel(NSLOCTEXT("AudioFactorySelectedAssetsWindow", "AssetHeader", "Asset"))
			.HAlignHeader(HAlign_Left)
			.HAlignCell(HAlign_Fill)
			.SortMode(this, &SAudioFactorySelectedAssetsWindow::GetColumnSortMode, AudioFactorySelectedAssetsWindowPrivate::AssetColumn())
			.OnSort(this, &SAudioFactorySelectedAssetsWindow::HandleSort)
			.FillWidth(0.38f)
			+ SHeaderRow::Column(AudioFactorySelectedAssetsWindowPrivate::ClassColumn())
			.DefaultLabel(NSLOCTEXT("AudioFactorySelectedAssetsWindow", "ClassHeader", "Class"))
			.HAlignHeader(HAlign_Left)
			.HAlignCell(HAlign_Fill)
			.SortMode(this, &SAudioFactorySelectedAssetsWindow::GetColumnSortMode, AudioFactorySelectedAssetsWindowPrivate::ClassColumn())
			.OnSort(this, &SAudioFactorySelectedAssetsWindow::HandleSort)
			.FillWidth(0.22f)
			+ SHeaderRow::Column(AudioFactorySelectedAssetsWindowPrivate::PathColumn())
			.DefaultLabel(NSLOCTEXT("AudioFactorySelectedAssetsWindow", "PathHeader", "Path"))
			.HAlignHeader(HAlign_Left)
			.HAlignCell(HAlign_Fill)
			.SortMode(this, &SAudioFactorySelectedAssetsWindow::GetColumnSortMode, AudioFactorySelectedAssetsWindowPrivate::PathColumn())
			.OnSort(this, &SAudioFactorySelectedAssetsWindow::HandleSort)
			.FillWidth(0.26f);

		if (bShowStatusColumn)
		{
			HeaderRow->AddColumn(
				SHeaderRow::Column(AudioFactorySelectedAssetsWindowPrivate::StatusColumn())
				.DefaultLabel(NSLOCTEXT("AudioFactorySelectedAssetsWindow", "StatusHeader", "Status"))
				.HAlignHeader(HAlign_Left)
				.HAlignCell(HAlign_Fill)
				.SortMode(this, &SAudioFactorySelectedAssetsWindow::GetColumnSortMode, AudioFactorySelectedAssetsWindowPrivate::StatusColumn())
				.OnSort(this, &SAudioFactorySelectedAssetsWindow::HandleSort)
				.FillWidth(0.14f));
		}

		SAssignNew(AssetListWidget, SListView<FAssetPtr>)
		.ListItemsSource(&FilteredAssets)
		.OnGenerateRow(this, &SAudioFactorySelectedAssetsWindow::GenerateAssetRow)
		.SelectionMode(ESelectionMode::None)
		.HeaderRow(HeaderRow);

		TSharedRef<SVerticalBox> Layout = SNew(SVerticalBox);

		Layout->AddSlot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(Tokens.SurfaceAlt)
			.Padding(FMargin(10.0f, 0.0f))
			[
				SNew(SBox)
			.HeightOverride(AudioFactorySelectedAssetsWindowPrivate::TitleBarHeight)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(InArgs._Title)
						.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
						.ColorAndOpacity(FSlateColor(Tokens.TextPrimary))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 10.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return BuildSummaryText();
						})
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.ColorAndOpacity(FSlateColor(Tokens.TextMuted))
					]
				]
			]
		];

		if (SearchEnabled)
		{
			Layout->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 8.0f, 0.0f, 6.0f))
			[
				SNew(SBox)
				.Padding(FMargin(12.0f, 0.0f, 12.0f, 0.0f))
				[
					SNew(SSearchBox)
					.HintText(NSLOCTEXT("AudioFactorySelectedAssetsWindow", "SearchPlaceholder", "Filter by name, path, class..."))
					.OnTextChanged(this, &SAudioFactorySelectedAssetsWindow::HandleSearchTextChanged)
				]
			];
		}

		Layout->AddSlot()
		.FillHeight(1.0f)
		.Padding(FMargin(12.0f, 0.0f, 12.0f, 8.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(Tokens.DividerColor)
			.Padding(FMargin(0.0f))
			[
				SNew(SOverlay)

				+ SOverlay::Slot()
				[
					AssetListWidget.ToSharedRef()
				]

				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Visibility(TAttribute<EVisibility>::CreateLambda([this]()
					{
						return FilteredAssets.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
					}))
					.Text(NSLOCTEXT("AudioFactorySelectedAssetsWindow", "NoAssets", "No assets match the current filter."))
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FSlateColor(Tokens.TextMuted))
				]
			]
		];

		ChildSlot
		[
			Layout
		];
	}

private:
	TSharedRef<ITableRow> GenerateAssetRow(FAssetPtr Asset, const TSharedRef<STableViewBase>& OwnerTable) const
	{
		return SNew(AudioFactorySelectedAssetsWindowPrivate::SSelectedAssetRow, OwnerTable)
			.Asset(Asset)
			.bShowStatusColumn(bShowStatusColumn);
	}

	void FilterAssets()
	{
		const FString Query = SearchFilter.ToString().ToLower();
		FilteredAssets.Reset();
		FilteredAssets.Reserve(RawAssets.Num());

		for (const FAssetPtr& Asset : RawAssets)
		{
			if (Asset.IsValid() && MatchesFilter(*Asset, Query))
			{
				FilteredAssets.Add(Asset);
			}
		}

		SortFilteredAssets();

		if (AssetListWidget.IsValid())
		{
			AssetListWidget->RequestListRefresh();
		}
	}

	bool MatchesFilter(const FAudioFactorySelectedAsset& Asset, const FString& QueryLower) const
	{
		if (QueryLower.IsEmpty())
		{
			return true;
		}

		const FString DisplayName = Asset.DisplayName.ToString().ToLower();
		const FString AssetType = Asset.AssetClass.ToString().ToLower();
		const FString AssetPath = Asset.AssetPath.ToString().ToLower();
		if (DisplayName.Contains(QueryLower)
			|| AssetType.Contains(QueryLower)
			|| AssetPath.Contains(QueryLower))
		{
			return true;
		}

		if (!bShowStatusColumn)
		{
			return false;
		}

		return (Asset.IsIncluded() ? FString(TEXT("included")) : FString(TEXT("excluded"))).Contains(QueryLower);
	}

	EColumnSortMode::Type GetColumnSortMode(const FName ColumnId) const
	{
		return SortColumn == ColumnId ? SortMode : EColumnSortMode::None;
	}

	void HandleSort(const EColumnSortPriority::Type SortPriority, const FName& ColumnId, const EColumnSortMode::Type NewSortMode)
	{
		SortColumn = ColumnId;
		SortMode = NewSortMode;
		SortFilteredAssets();
		if (AssetListWidget.IsValid())
		{
			AssetListWidget->RequestListRefresh();
		}
	}

	void SortFilteredAssets()
	{
		if (SortColumn.IsNone() || SortMode == EColumnSortMode::None)
		{
			return;
		}

		const FName ActiveSortColumn = SortColumn;
		const EColumnSortMode::Type ActiveSortMode = SortMode;
		FilteredAssets.StableSort([ActiveSortColumn, ActiveSortMode](const FAssetPtr& Left, const FAssetPtr& Right)
		{
			if (!Left.IsValid() || !Right.IsValid())
			{
				return Left.IsValid() && !Right.IsValid();
			}

			if (ActiveSortColumn == AudioFactorySelectedAssetsWindowPrivate::StatusColumn())
			{
				if (Left->IsIncluded() != Right->IsIncluded())
				{
					return ActiveSortMode == EColumnSortMode::Ascending ? Left->IsIncluded() : !Left->IsIncluded();
				}
				return false;
			}

			FString LeftValue;
			FString RightValue;
			if (ActiveSortColumn == AudioFactorySelectedAssetsWindowPrivate::AssetColumn())
			{
				LeftValue = Left->DisplayName.ToString().ToLower();
				RightValue = Right->DisplayName.ToString().ToLower();
			}
			else if (ActiveSortColumn == AudioFactorySelectedAssetsWindowPrivate::ClassColumn())
			{
				LeftValue = Left->AssetClass.ToString().ToLower();
				RightValue = Right->AssetClass.ToString().ToLower();
			}
			else
			{
				LeftValue = Left->AssetPath.ToString().ToLower();
				RightValue = Right->AssetPath.ToString().ToLower();
			}

			const int32 Comparison = LeftValue.Compare(RightValue, ESearchCase::CaseSensitive);
			if (Comparison == 0)
			{
				return false;
			}
			return ActiveSortMode == EColumnSortMode::Ascending ? Comparison < 0 : Comparison > 0;
		});
	}

	FText BuildSummaryText() const
	{
		int32 IncludedCount = 0;
		int32 ExcludedCount = 0;

		for (const FAssetPtr& Asset : FilteredAssets)
		{
			if (!Asset.IsValid())
			{
				continue;
			}
			if (Asset->IsIncluded())
			{
				++IncludedCount;
			}
			else
			{
				++ExcludedCount;
			}
		}

		return FText::Format(
			NSLOCTEXT("AudioFactorySelectedAssetsWindow", "SummaryText", "{0} assets | {1} included | {2} excluded"),
			FText::AsNumber(FilteredAssets.Num()),
			FText::AsNumber(IncludedCount),
			FText::AsNumber(ExcludedCount));
	}

	void HandleSearchTextChanged(const FText& InText)
	{
		SearchFilter = InText;
		FilterAssets();
	}

	bool SearchEnabled = true;
	bool bShowStatusColumn = false;
	FText SearchFilter;
	TArray<FAssetPtr> RawAssets;
	TArray<FAssetPtr> FilteredAssets;
	TSharedPtr<SListView<FAssetPtr>> AssetListWidget;
	FName SortColumn;
	EColumnSortMode::Type SortMode = EColumnSortMode::None;
};
} // namespace AudioExporter
