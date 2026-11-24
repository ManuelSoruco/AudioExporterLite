// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Dialogs/FAudioFactoryDialogWindow.h"

#include "Dialogs/AudioFactoryPluginWindowAdapter.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Internationalization/Text.h"
namespace AudioExporter {

#define LOCTEXT_NAMESPACE "AudioFactoryDialogWindow"

FText FAudioFactoryDialogWindow::MakeTitle(EAudioFactoryProductId ProductId, const FText& TaskTitle)
{
	const FAudioFactoryProductIdentity& Identity = FAudioFactoryProductRegistry::Get(ProductId);
	// Title grammar: "<Product> — <Task>". The registry ProductName carries
	// the plain product name ("Audio Exporter", ...) with no brand prefix.
	return FText::Format(
		LOCTEXT("DialogTitleFormat", "{0} — {1}"),
		Identity.ProductName,
		TaskTitle);
}

namespace AudioFactoryDialogWindowPrivate
{
	static bool IsSettingsTaskTitle(const FText& TaskTitle)
	{
		const FString* SourceString = FTextInspector::GetSourceString(TaskTitle);
		return SourceString != nullptr
			&& SourceString->Equals(TEXT("Settings"), ESearchCase::CaseSensitive);
	}
}

TSharedRef<SWindow> FAudioFactoryDialogWindow::Create(const FAudioFactoryDialogWindowArgs& Args)
{
	if (AudioFactoryDialogWindowPrivate::IsSettingsTaskTitle(Args.TaskTitle))
	{
		if (const TSharedPtr<SWindow> ExistingWindow = FAudioFactoryPluginWindowAdapter::FindLiveSettingsWindow(Args.ProductId))
		{
			ExistingWindow->BringToFront(true);
			return ExistingWindow.ToSharedRef();
		}
	}

	return SNew(SWindow)
		.Title(MakeTitle(Args.ProductId, Args.TaskTitle))
		.ClientSize(Args.ClientSize)
		.MinWidth(Args.MinSize.X)
		.MinHeight(Args.MinSize.Y)
		.AutoCenter(EAutoCenter::PreferredWorkArea)
		.SaneWindowPlacement(true)
		.SizingRule(ESizingRule::UserSized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);
}

void FAudioFactoryDialogWindow::Show(TSharedRef<SWindow> Window, const TSharedPtr<SWindow>& ParentWindow, bool bModal)
{
	if (bModal)
	{
		FSlateApplication::Get().AddModalWindow(Window, ParentWindow);
		return;
	}

	if (ParentWindow.IsValid())
	{
		// Parented so the dialog can never fall behind the editor (the
		// shared parenting idiom).
		FSlateApplication::Get().AddWindowAsNativeChild(Window, ParentWindow.ToSharedRef());
		return;
	}

	FSlateApplication::Get().AddWindow(Window);
}

#undef LOCTEXT_NAMESPACE
} // namespace AudioExporter
