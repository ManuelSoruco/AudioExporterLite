// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "UObject/ObjectMacros.h"

#include <type_traits>

// Per-product export seam: each consuming UI module maps AF_UI_API to its own module API macro
// via PublicDefinitions so cross-module callers (product module files) can link the adapter's
// statics. Contexts that never consume it cross-module fall back to an empty expansion.
#ifndef AF_UI_API
#define AF_UI_API
#endif

class SWindow;
class SWidget;
class SNotificationItem;
class IDetailLayoutBuilder;

#if WITH_EDITOR

#include "Foundation/AudioFactoryPluginWindowTypes.h"

class FNotifyHook;
class IDetailsView;
namespace AudioExporter {
enum class EAudioFactoryPluginWindowValidationState : uint8
{
	Unknown,
	Valid,
	Warning,
	Error,
};

// Explicit asset-class discriminator for count rows. The adapter filters counts
// by product asset-type support on this enum, never by substring-matching a
// localized label (which breaks in non-English editors).
enum class EAudioFactoryAssetCountType : uint8
{
	Generic,
	MetaSounds,
	SoundCues,
	SoundWaves,
};

struct FAudioFactoryPluginWindowAssetCount
{
	FAudioFactoryPluginWindowAssetCount() = default;
	FAudioFactoryPluginWindowAssetCount(
		FText InLabel,
		int32 InCount,
		FText InTooltip = FText::GetEmpty(),
		EAudioFactoryAssetCountType InType = EAudioFactoryAssetCountType::Generic)
		: Label(MoveTemp(InLabel))
		, Count(InCount)
		, Tooltip(MoveTemp(InTooltip))
		, Type(InType)
	{
	}

	FText Label;
	int32 Count = 0;
	FText Tooltip;
	EAudioFactoryAssetCountType Type = EAudioFactoryAssetCountType::Generic;
};

struct FAudioFactoryPluginWindowValidationResult
{
	static FAudioFactoryPluginWindowValidationResult Make(
		EAudioFactoryPluginWindowValidationState InState,
		FText InMessage,
		bool bInCanApply = true,
		bool bInCanRunPrimaryAction = true);

	static FAudioFactoryPluginWindowValidationResult Valid(FText InMessage = FText::GetEmpty());
	static FAudioFactoryPluginWindowValidationResult Warning(FText InMessage);
	static FAudioFactoryPluginWindowValidationResult Error(FText InMessage);

	EAudioFactoryPluginWindowValidationState State = EAudioFactoryPluginWindowValidationState::Unknown;
	FText Message;
	bool bCanApply = true;
	bool bCanRunPrimaryAction = true;
};

/**
 * Primary action contract for a settings window.
 * New callers return acceptance and user-facing failure text; legacy void callers remain source
 * compatible while the adapter observes whether they requested the owning window to close.
 */
struct FAudioFactoryPluginPrimaryAction
{
	using FResultCallback = TFunction<bool(const UObject*, FText&)>;
	using FLegacyCallback = TFunction<void(const UObject*)>;

	/** Accept either the result-returning contract or the legacy close-on-success contract. */
	template <typename CallbackType>
	FAudioFactoryPluginPrimaryAction& operator=(CallbackType&& InCallback)
	{
		using DecayedCallbackType = std::decay_t<CallbackType>;
		if constexpr (std::is_invocable_r_v<bool, DecayedCallbackType&, const UObject*, FText&>)
		{
			ResultCallback = FResultCallback(Forward<CallbackType>(InCallback));
			LegacyCallback = FLegacyCallback();
		}
		else
		{
			static_assert(
				std::is_invocable_r_v<void, DecayedCallbackType&, const UObject*>,
				"AudioFactory primary actions must return bool with FText& or use the legacy UObject-only signature.");
			ResultCallback = FResultCallback();
			LegacyCallback = FLegacyCallback(Forward<CallbackType>(InCallback));
		}
		return *this;
	}

	/** Return whether either accepted callback contract is present. */
	bool IsBound() const
	{
		return static_cast<bool>(ResultCallback) || static_cast<bool>(LegacyCallback);
	}

	/** Preserve the existing boolean presence checks used by product UI sources. */
	operator bool() const
	{
		return IsBound();
	}

	/** Invoke the callback and return whether the requested action was accepted. */
	bool Execute(const UObject* SourceSettingsObject, FText& OutError) const;

	/** Preserve the existing one-argument call syntax for untouched shared aliases. */
	void operator()(const UObject* SourceSettingsObject) const
	{
		FText IgnoredError;
		Execute(SourceSettingsObject, IgnoredError);
	}

	FResultCallback ResultCallback;
	FLegacyCallback LegacyCallback;
};

struct FAudioFactoryPluginWindowSpec
{
	FName PluginId;
	FText WindowTitle;
	FText HeaderTitle;
	FText HeaderSubtitle;

	UObject* SourceSettingsObject = nullptr;
	UObject* StagedSettingsObject = nullptr;
	TArray<FAudioFactoryPluginWindowAssetCount> AssetCounts;
	TFunction<TSharedRef<SWidget>(UObject*)> OnBuildContextContent;

	FName DetailsViewIdentifier;
	FVector2D InitialSize = FVector2D(720.0f, 760.0f);

	TFunction<FAudioFactoryPluginWindowValidationResult(const UObject*)> OnValidate;
	// Optional staged-settings predicate that controls whether the primary action is shown.
	// When unset, the primary action remains visible for backwards compatibility.
	TFunction<bool(const UObject*)> OnIsPrimaryActionVisible;
	// Return false only after writing the actual user-facing failure text to OutError;
	// the adapter keeps the window open and shows that text exactly once.
	TFunction<bool(UObject*, const UObject*, FText&)> OnApplySettings;
	// Return false only after writing the actual user-facing failure text to OutError;
	// the adapter keeps the window open and shows that text exactly once.
	TFunction<bool(UObject*, FText&)> OnResetDefaults;
	// Optional product-specific details customization. The adapter invokes this inside its
	// instanced customization so product rows and per-property reset arrows can coexist.
	TFunction<void(IDetailLayoutBuilder&)> OnCustomizeDetails;
	FAudioFactoryPluginPrimaryAction OnPrimaryAction;
	TFunction<void()> OnCancel;
	TFunction<void()> OnOpenReport;
	TFunction<void()> OnCopyFileList;

	FText PrimaryActionLabel;
	FText ApplyLabel;
	FText OkLabel;
	FText CancelLabel;
	FText OpenReportLabel;
	FText CopyFileListLabel;

	bool bShowApplyButton = false;
	bool bShowOkButton = true;
	bool bShowCancelButton = true;
	bool bShowOpenReportButton = false;
	bool bShowCopyFileListButton = false;
	bool bCloseOnOk = true;
	bool bSaveConfigOnDefaultApply = true;
	bool bFlushConfigOnDefaultApply = true;
};

struct FAudioFactoryPluginWindowCopyOptions
{
	bool bCopyConfigProperties = true;
	bool bCopyEditableProperties = true;
	bool bSkipTransientProperties = true;
	bool bSkipDeprecatedProperties = true;
};

struct FAudioFactoryPluginWindowApplyOptions
{
	FAudioFactoryPluginWindowCopyOptions CopyOptions;
	bool bSaveConfig = true;
	bool bFlushConfig = true;
	bool bCallPostEditChange = true;
	uint64 RequiredPropertyFlags = CPF_Config;
	FString ConfigFilename;
};

class FAudioFactoryPluginWindowLifecycle
{
public:
	static UObject* CreateStagedSettingsObject(UObject* SourceSettingsObject, UObject* Outer = nullptr, FName Name = NAME_None);

	static bool CopySettingsProperties(
		const UObject* SourceSettingsObject,
		UObject* TargetSettingsObject,
		const FAudioFactoryPluginWindowCopyOptions& Options,
		FText* OutError = nullptr);

	static bool ApplyStagedSettings(
		UObject* SourceSettingsObject,
		const UObject* StagedSettingsObject,
		const FAudioFactoryPluginWindowApplyOptions& Options,
		FText* OutError = nullptr);

	static bool ResetStagedSettingsToClassDefaults(
		UObject* StagedSettingsObject,
		const FAudioFactoryPluginWindowCopyOptions& Options,
		FText* OutError = nullptr);

	static void DiscardStagedSettingsObject(UObject* StagedSettingsObject);
};


struct FAudioFactoryPluginWindowDetailsViewOptions
{
	FName ViewIdentifier;
	FNotifyHook* NotifyHook = nullptr;
	bool bAllowSearch = false;
	bool bShowScrollBar = true;
	bool bAllowAdvancedProperties = true;
	bool bShowOptions = true;
};

class FAudioFactoryPluginWindowDetails
{
public:
	static TSharedRef<IDetailsView> CreateDetailsView(const FAudioFactoryPluginWindowDetailsViewOptions& Options);
	static TSharedRef<IDetailsView> CreateDetailsViewForObject(
		UObject* SettingsObject,
		const FAudioFactoryPluginWindowDetailsViewOptions& Options);
};

struct FAudioFactoryPluginAssetTypeSupport
{
	bool bSupportsMetaSounds = false;
	bool bSupportsSoundCues = false;
	bool bSupportsSoundWaves = false;
};

struct FAudioFactoryPluginSettingsFrameSpec
{
	FAudioFactoryPluginWindowSpec NativeSpec;
	EAudioFactoryProductId ProductId = EAudioFactoryProductId::AudioExporter;
	FAudioFactoryPluginAssetTypeSupport AssetTypes;
	TArray<FAudioFactoryInlineCount> Counts;
	TOptional<FAudioFactoryPathContext> PathContext;
	TFunction<TArray<FAudioFactoryInlineCount>(UObject*)> OnBuildCounts;
	TFunction<TOptional<FAudioFactoryPathContext>(UObject*)> OnBuildPathContext;
	TFunction<TArray<FAudioFactoryFooterAction>(UObject*)> OnBuildHeaderToolActions;
	TFunction<TArray<FAudioFactoryFooterAction>(UObject*)> OnBuildExtraFooterActions;
};

class AF_UI_API FAudioFactoryPluginWindowAdapter
{
public:
	static TSharedRef<SWindow> OpenPluginSettingsWindow(FAudioFactoryPluginSettingsFrameSpec Spec);
	/** Return the live settings window for a product, or an invalid pointer after stale cleanup. */
	static TSharedPtr<SWindow> FindLiveSettingsWindow(EAudioFactoryProductId ProductId);
	/** Keep a live settings window visible after an invocation refusal without changing its UI state. */
	static bool ShowLiveSettingsFailure(EAudioFactoryProductId ProductId, const FText& ErrorText);
	static TSharedRef<SWidget> BuildPluginSettingsFrame(
		FAudioFactoryPluginSettingsFrameSpec Spec,
		TWeakPtr<SWindow> OwnerWindow = nullptr);

	// Automation seam: invokes the same validation refresh used by production UI callbacks.
	static void ForceRevalidateForAutomation(const TSharedRef<SWidget>& SettingsFrame);
	// Automation seam: invokes the same per-property reset path used by the native Details arrow.
	static bool ResetPropertyToDefaultForAutomation(const TSharedRef<SWidget>& SettingsFrame, FName PropertyName);
	// Read-only automation seam: reports what the registered per-property reset-arrow override's
	// visibility predicate would return for a named property (true = arrow visible), without
	// touching any widget or settings state.
	static bool IsResetToDefaultVisibleByNameForTesting(const TSharedRef<SWidget>& SettingsFrame, FName PropertyName);
#if WITH_DEV_AUTOMATION_TESTS
	// Refusal-toast coverage hooks, same spirit as the Pro window's deferred-rejection hooks:
	// the counter increments ONLY inside ShowActionFailure's non-modal branch AFTER
	// FSlateNotificationManager::AddNotification actually returns a real item, so deleting the
	// real notification call turns coverage red instead of leaving it green. When AddNotification
	// fails (returns null), nothing is recorded: no count, no text, no item. The stored text is
	// the exact user-facing toast copy; the stored item is the exact SNotificationItem the last
	// successful AddNotification returned.
	static void ResetActionFailureToastHooksForTesting();
	static int32 GetActionFailureToastCountForTesting();
	static FString GetLastActionFailureToastTextForTesting();
	static TSharedPtr<SNotificationItem> GetLastActionFailureToastItemForTesting();
#endif
};
} // namespace AudioExporter

#endif // WITH_EDITOR
