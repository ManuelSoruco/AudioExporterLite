// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Controls/SAudioFactoryTaskLogView.h"

#include "Foundation/AudioFactoryStatusIcon.h"
#include "Foundation/AudioFactoryStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
namespace AudioExporter {

namespace AudioFactoryTaskLogViewPrivate
{
	constexpr float RowHeight = 22.0f;
	constexpr float IconSize = 14.0f;
}

void SAudioFactoryTaskLogView::Construct(const FArguments& InArgs)
{
	LogItems = InArgs._LogItems;

	SAssignNew(ListView, SListView<TSharedPtr<FAudioFactoryTaskLogEntry>>)
	.ListItemsSource(LogItems != nullptr ? LogItems : &GetEmptyLogItems())
	.OnGenerateRow(this, &SAudioFactoryTaskLogView::GenerateRow)
	.SelectionMode(ESelectionMode::None);

	ChildSlot
	[
		ListView.ToSharedRef()
	];
}

void SAudioFactoryTaskLogView::SetLogItems(const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>>* InLogItems)
{
	LogItems = InLogItems;
	if (ListView.IsValid())
	{
		ListView->SetItemsSource(LogItems != nullptr ? LogItems : &GetEmptyLogItems());
		ListView->RequestListRefresh();
	}
}

void SAudioFactoryTaskLogView::RequestScrollToBottom()
{
	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
		ListView->ScrollToBottom();
	}
}

const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>>& SAudioFactoryTaskLogView::GetEmptyLogItems()
{
	static const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>> EmptyItems;
	return EmptyItems;
}

TSharedRef<ITableRow> SAudioFactoryTaskLogView::GenerateRow(
	TSharedPtr<FAudioFactoryTaskLogEntry> Item,
	const TSharedRef<STableViewBase>& OwnerTable) const
{
	const TSharedPtr<FAudioFactoryTaskLogEntry> ItemForBinding = Item;

	return SNew(STableRow<TSharedPtr<FAudioFactoryTaskLogEntry>>, OwnerTable)
		[
			SNew(SBox)
			.HeightOverride(AudioFactoryTaskLogViewPrivate::RowHeight)
			.Padding(FMargin(4.0f, 0.0f, 4.0f, 0.0f))
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(AudioFactoryTaskLogViewPrivate::IconSize)
					.HeightOverride(AudioFactoryTaskLogViewPrivate::IconSize)
					[
						SNew(SImage)
						.Image(TAttribute<const FSlateBrush*>::CreateLambda([ItemForBinding]()
						{
							const EAudioFactoryStatusSeverity Severity =
								ItemForBinding.IsValid() ? ItemForBinding->Severity : EAudioFactoryStatusSeverity::Neutral;
							const FAudioFactoryStatusIcon::FStatusIconResolution Icon = FAudioFactoryStatusIcon::Resolve(Severity);
							return Icon.Brush;
						}))
						.ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda([ItemForBinding]()
						{
							const EAudioFactoryStatusSeverity Severity =
								ItemForBinding.IsValid() ? ItemForBinding->Severity : EAudioFactoryStatusSeverity::Neutral;
							const FAudioFactoryStatusIcon::FStatusIconResolution Icon = FAudioFactoryStatusIcon::Resolve(Severity);
							return FSlateColor(Icon.bNeedsTint
								? FAudioFactoryStatusIcon::SeverityColor(FAudioFactoryStyle::Get().PluginWindowTokens(), Severity)
								: FLinearColor::White);
						}))
					]
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(TAttribute<FText>::CreateLambda([ItemForBinding]()
					{
						return ItemForBinding.IsValid() ? ItemForBinding->Text : FText::GetEmpty();
					}))
					.ToolTipText(TAttribute<FText>::CreateLambda([ItemForBinding]()
					{
						const FText RowText = ItemForBinding.IsValid() ? ItemForBinding->Text : FText::GetEmpty();
						return ItemForBinding.IsValid() && !ItemForBinding->ToolTip.IsEmpty()
							? ItemForBinding->ToolTip
							: RowText;
					}))
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.ColorAndOpacity(FSlateColor(FAudioFactoryStyle::Get().PluginWindowTokens().TextPrimary))
				]
			]
		];
}
} // namespace AudioExporter
