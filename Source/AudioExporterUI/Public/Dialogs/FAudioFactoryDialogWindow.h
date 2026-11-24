// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryPluginWindowTypes.h"

#ifndef AF_UI_API
#define AF_UI_API
#endif

class SWindow;

/** Creation parameters for FAudioFactoryDialogWindow. */
namespace AudioExporter {
struct AF_UI_API FAudioFactoryDialogWindowArgs
{
	EAudioFactoryProductId ProductId = EAudioFactoryProductId::AudioExporter;
	FText TaskTitle;
	FVector2D ClientSize = FVector2D(560.0f, 420.0f);
	FVector2D MinSize = FVector2D(480.0f, 320.0f);
};

/**
 * One correct shell for every AudioFactory secondary window.
 *
 * - Title grammar: "<Product> — <Task>"; the "AudioFactory /" prefix no
 *   longer appears in OS title bars.
 * - UserSized with a required MinSize; AutoCenter(PreferredWorkArea) +
 *   SaneWindowPlacement; no maximize / minimize buttons.
 * - Show() parents the window via AddWindowAsNativeChild so it can never fall
 *   behind the editor (the shared parenting idiom), and uses
 *   AddModalWindow for modal dialogs.
 */
class AF_UI_API FAudioFactoryDialogWindow
{
public:
	static FText MakeTitle(EAudioFactoryProductId ProductId, const FText& TaskTitle);

	static TSharedRef<SWindow> Create(const FAudioFactoryDialogWindowArgs& Args);

	static void Show(TSharedRef<SWindow> Window, const TSharedPtr<SWindow>& ParentWindow, bool bModal);
};
} // namespace AudioExporter
