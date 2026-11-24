// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryPluginWindowTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

#ifndef AF_UI_API
#define AF_UI_API
#endif

/** One row in a task log: severity drives the 14x14 engine icon + row tone. */
namespace AudioExporter {
struct AF_UI_API FAudioFactoryTaskLogEntry
{
	FAudioFactoryTaskLogEntry() = default;

	FAudioFactoryTaskLogEntry(
		EAudioFactoryStatusSeverity InSeverity,
		FText InText,
		FText InToolTip = FText::GetEmpty())
		: Severity(InSeverity)
		, Text(MoveTemp(InText))
		, ToolTip(MoveTemp(InToolTip))
	{
	}

	EAudioFactoryStatusSeverity Severity = EAudioFactoryStatusSeverity::Neutral;
	FText Text;
	FText ToolTip;
	FString RowKey;
};

/**
 * Native SListView task log: 22px rows, 14x14 engine status icons
 * (Icons.SuccessWithColor family), SmallFont, ellipsis + full-text tooltip.
 */
class AF_UI_API SAudioFactoryTaskLogView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryTaskLogView)
		: _LogItems(nullptr)
	{
	}
		SLATE_ARGUMENT(const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>>*, LogItems)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Point the list at a new item source and rebuild the visible rows. */
	void SetLogItems(const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>>* InLogItems);

	/** Refresh the list and scroll to the newest entry. */
	void RequestScrollToBottom();

#if WITH_DEV_AUTOMATION_TESTS
	/** Headless test seam: generates one row for the item so tests can assert the row's
	 * live-bound text via STextBlock::GetText() (which evaluates the binding on demand).
	 * GenerateRow must stay private; the seam resolves the access boundary without the
	 * define-private-public include hack (MSVC access mangling breaks that linkage). */
	TSharedRef<ITableRow> GenerateRowForTesting(
		TSharedPtr<FAudioFactoryTaskLogEntry> Item,
		const TSharedRef<STableViewBase>& OwnerTable) const
	{
		return GenerateRow(Item, OwnerTable);
	}
#endif

private:
	static const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>>& GetEmptyLogItems();

	TSharedRef<ITableRow> GenerateRow(
		TSharedPtr<FAudioFactoryTaskLogEntry> Item,
		const TSharedRef<STableViewBase>& OwnerTable) const;

	const TArray<TSharedPtr<FAudioFactoryTaskLogEntry>>* LogItems = nullptr;
	TSharedPtr<SListView<TSharedPtr<FAudioFactoryTaskLogEntry>>> ListView;
};
} // namespace AudioExporter
