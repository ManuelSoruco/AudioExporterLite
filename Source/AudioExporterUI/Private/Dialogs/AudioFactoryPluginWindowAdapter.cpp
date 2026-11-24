// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Dialogs/AudioFactoryPluginWindowAdapter.h"

#if WITH_EDITOR

#include "Application/SlateWindowHelper.h"
#include "Controls/SAudioFactoryPluginWindowFrame.h"
#include "DetailsViewArgs.h"
#include "DetailLayoutBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IDetailCustomization.h"
#include "IDetailPropertyRow.h"
#include "IDetailsView.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Misc/AutomationTest.h"
#include "UObject/FieldIterator.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
namespace AudioExporter {

#define LOCTEXT_NAMESPACE "AudioFactoryPluginWindowAdapter"

FAudioFactoryPluginWindowValidationResult FAudioFactoryPluginWindowValidationResult::Make(
	EAudioFactoryPluginWindowValidationState InState,
	FText InMessage,
	bool bInCanApply,
	bool bInCanRunPrimaryAction)
{
	FAudioFactoryPluginWindowValidationResult Result;
	Result.State = InState;
	Result.Message = MoveTemp(InMessage);
	// An Error is never actionable, regardless of which convenience factory or
	// product callback produced it. Keep the result's permissions consistent
	// with its state before IsActionEnabled consumes the result.
	Result.bCanApply = InState == EAudioFactoryPluginWindowValidationState::Error ? false : bInCanApply;
	Result.bCanRunPrimaryAction = InState == EAudioFactoryPluginWindowValidationState::Error ? false : bInCanRunPrimaryAction;
	return Result;
}

FAudioFactoryPluginWindowValidationResult FAudioFactoryPluginWindowValidationResult::Valid(FText InMessage)
{
	if (InMessage.IsEmpty())
	{
		InMessage = LOCTEXT("ValidationReady", "Ready");
	}
	return Make(EAudioFactoryPluginWindowValidationState::Valid, MoveTemp(InMessage));
}

FAudioFactoryPluginWindowValidationResult FAudioFactoryPluginWindowValidationResult::Warning(FText InMessage)
{
	return Make(EAudioFactoryPluginWindowValidationState::Warning, MoveTemp(InMessage));
}

FAudioFactoryPluginWindowValidationResult FAudioFactoryPluginWindowValidationResult::Error(FText InMessage)
{
	return Make(EAudioFactoryPluginWindowValidationState::Error, MoveTemp(InMessage), false, false);
}

bool FAudioFactoryPluginPrimaryAction::Execute(const UObject* SourceSettingsObject, FText& OutError) const
{
	if (ResultCallback)
	{
		return ResultCallback(SourceSettingsObject, OutError);
	}

	if (LegacyCallback)
	{
		LegacyCallback(SourceSettingsObject);
		return true;
	}

	OutError = LOCTEXT("PrimaryActionMissingCallback", "The primary action was not available.");
	return false;
}

namespace AudioFactoryPluginWindowLifecycle
{
	static bool ShouldCopyProperty(const FProperty* Property, const FAudioFactoryPluginWindowCopyOptions& Options)
	{
		if (Property == nullptr)
		{
			return false;
		}

		if (Options.bSkipDeprecatedProperties && Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			return false;
		}

		if (Options.bSkipTransientProperties &&
			Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
		{
			return false;
		}

		if (!Options.bCopyConfigProperties && Property->HasAnyPropertyFlags(CPF_Config))
		{
			return false;
		}

		if (!Options.bCopyEditableProperties && Property->HasAnyPropertyFlags(CPF_Edit))
		{
			return false;
		}

		return Property->HasAnyPropertyFlags(CPF_Config | CPF_Edit);
	}

	static void SetError(FText* OutError, const FText& Error)
	{
		if (OutError != nullptr)
		{
			*OutError = Error;
		}
	}
}

UObject* FAudioFactoryPluginWindowLifecycle::CreateStagedSettingsObject(
	UObject* SourceSettingsObject,
	UObject* Outer,
	FName Name)
{
	if (SourceSettingsObject == nullptr)
	{
		return nullptr;
	}

	UObject* ResolvedOuter = Outer != nullptr ? Outer : GetTransientPackage();
	UObject* StagedSettingsObject = NewObject<UObject>(
		ResolvedOuter,
		SourceSettingsObject->GetClass(),
		Name,
		RF_Transient);

	FAudioFactoryPluginWindowCopyOptions CopyOptions;
	CopySettingsProperties(SourceSettingsObject, StagedSettingsObject, CopyOptions);
	return StagedSettingsObject;
}

bool FAudioFactoryPluginWindowLifecycle::CopySettingsProperties(
	const UObject* SourceSettingsObject,
	UObject* TargetSettingsObject,
	const FAudioFactoryPluginWindowCopyOptions& Options,
	FText* OutError)
{
	if (SourceSettingsObject == nullptr || TargetSettingsObject == nullptr)
	{
		AudioFactoryPluginWindowLifecycle::SetError(
			OutError,
			LOCTEXT("CopyMissingObject", "A source or target settings object is missing."));
		return false;
	}

	if (SourceSettingsObject->GetClass() != TargetSettingsObject->GetClass())
	{
		AudioFactoryPluginWindowLifecycle::SetError(
			OutError,
			FText::Format(
				LOCTEXT("CopyClassMismatch", "Settings object class mismatch: {0} cannot be copied into {1}."),
				FText::FromString(SourceSettingsObject->GetClass()->GetName()),
				FText::FromString(TargetSettingsObject->GetClass()->GetName())));
		return false;
	}

	for (TFieldIterator<FProperty> PropertyIt(SourceSettingsObject->GetClass(), EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		if (!AudioFactoryPluginWindowLifecycle::ShouldCopyProperty(Property, Options))
		{
			continue;
		}

		Property->CopyCompleteValue_InContainer(TargetSettingsObject, SourceSettingsObject);
	}

	return true;
}

bool FAudioFactoryPluginWindowLifecycle::ApplyStagedSettings(
	UObject* SourceSettingsObject,
	const UObject* StagedSettingsObject,
	const FAudioFactoryPluginWindowApplyOptions& Options,
	FText* OutError)
{
	if (SourceSettingsObject == nullptr || StagedSettingsObject == nullptr)
	{
		AudioFactoryPluginWindowLifecycle::SetError(
			OutError,
			LOCTEXT("ApplyMissingObject", "A source or staged settings object is missing."));
		return false;
	}

	if (Options.bCallPostEditChange)
	{
		SourceSettingsObject->Modify();
		SourceSettingsObject->PreEditChange(nullptr);
	}

	if (!CopySettingsProperties(StagedSettingsObject, SourceSettingsObject, Options.CopyOptions, OutError))
	{
		return false;
	}

	if (Options.bCallPostEditChange)
	{
		SourceSettingsObject->PostEditChange();
	}

	if (Options.bSaveConfig)
	{
		// Suite isolation: while an automation test is
		// executing, window commits still apply staged values onto the source settings object in
		// memory, but they must NEVER rewrite the project's committed Default*.ini release inputs.
		// Every product settings window binds the real class default object as its source, so a
		// suite-driven commit used to snapshot whatever the loaded config hierarchy held at that
		// moment -- test fixtures, cross-test mutations of the shared object, or stale user-layer
		// overrides -- straight into the release ini, resurrecting rejected defaults over manual
		// reverts. GetCurrentTest() is non-null for a test's whole lifecycle (body plus latent
		// commands) and null for every real-user click, so persistence outside automation behaves
		// exactly as before.
		if (FAutomationTestFramework::Get().GetCurrentTest() != nullptr)
		{
			return true;
		}

		// DefaultConfig classes persist to the Default ini, and that file is the one actually
		// read at startup. SaveConfig() writes the per-project/user ini instead, so flushing
		// GetDefaultConfigFilename() afterward would flush a file SaveConfig never wrote -- a
		// no-op on stale data (split-brain). TryUpdateDefaultConfigFile writes and flushes the
		// Default ini itself, so persistence and the startup read agree.
		if (SourceSettingsObject->GetClass()->HasAnyClassFlags(CLASS_DefaultConfig))
		{
			SourceSettingsObject->TryUpdateDefaultConfigFile();
		}
		else
		{
			const TCHAR* ConfigFilename = Options.ConfigFilename.IsEmpty() ? nullptr : *Options.ConfigFilename;
			SourceSettingsObject->SaveConfig(Options.RequiredPropertyFlags, ConfigFilename);

			if (Options.bFlushConfig && GConfig != nullptr)
			{
				const FString ResolvedFilename = Options.ConfigFilename.IsEmpty()
					? SourceSettingsObject->GetDefaultConfigFilename()
					: Options.ConfigFilename;
				if (!ResolvedFilename.IsEmpty())
				{
					GConfig->Flush(false, ResolvedFilename);
				}
			}
		}
	}

	return true;
}

bool FAudioFactoryPluginWindowLifecycle::ResetStagedSettingsToClassDefaults(
	UObject* StagedSettingsObject,
	const FAudioFactoryPluginWindowCopyOptions& Options,
	FText* OutError)
{
	if (StagedSettingsObject == nullptr)
	{
		AudioFactoryPluginWindowLifecycle::SetError(
			OutError,
			LOCTEXT("ResetMissingObject", "A staged settings object is missing."));
		return false;
	}

	const UObject* ClassDefaults = StagedSettingsObject->GetClass()->GetDefaultObject();
	return CopySettingsProperties(ClassDefaults, StagedSettingsObject, Options, OutError);
}

void FAudioFactoryPluginWindowLifecycle::DiscardStagedSettingsObject(UObject* StagedSettingsObject)
{
	if (StagedSettingsObject != nullptr)
	{
		StagedSettingsObject->ClearFlags(RF_Standalone);
	}
}

TSharedRef<IDetailsView> FAudioFactoryPluginWindowDetails::CreateDetailsView(
	const FAudioFactoryPluginWindowDetailsViewOptions& Options)
{
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsViewArgs.bShowObjectLabel = false;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bAllowSearch = Options.bAllowSearch;
	DetailsViewArgs.bSearchInitialKeyFocus = false;
	DetailsViewArgs.bShowScrollBar = Options.bShowScrollBar;
	DetailsViewArgs.bShowOptions = Options.bShowOptions;
	DetailsViewArgs.bShowPropertyMatrixButton = false;
	DetailsViewArgs.bAllowFavoriteSystem = false;
	DetailsViewArgs.bShowHiddenPropertiesWhilePlayingOption = false;
	DetailsViewArgs.bShowKeyablePropertiesOption = false;
	DetailsViewArgs.bShowAnimatedPropertiesOption = false;
	DetailsViewArgs.DefaultsOnlyVisibility = Options.bAllowAdvancedProperties
		? EEditDefaultsOnlyNodeVisibility::Show
		: EEditDefaultsOnlyNodeVisibility::Hide;
	DetailsViewArgs.ViewIdentifier = Options.ViewIdentifier;
	DetailsViewArgs.NotifyHook = Options.NotifyHook;

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	return PropertyEditorModule.CreateDetailView(DetailsViewArgs);
}

TSharedRef<IDetailsView> FAudioFactoryPluginWindowDetails::CreateDetailsViewForObject(
	UObject* SettingsObject,
	const FAudioFactoryPluginWindowDetailsViewOptions& Options)
{
	TSharedRef<IDetailsView> DetailsView = CreateDetailsView(Options);
	DetailsView->SetObject(SettingsObject, true);
	return DetailsView;
}

class SAudioFactoryPluginSettingsFrameAdapter;

namespace AudioFactoryPluginPropertyReset
{
	static TStrongObjectPtr<UObject> BuildProductDefaults(
		UObject* StagedSettingsObject,
		const TFunction<bool(UObject*, FText&)>& ResetDefaults)
	{
		if (!IsValid(StagedSettingsObject) || !ResetDefaults)
		{
			return TStrongObjectPtr<UObject>();
		}

		TStrongObjectPtr<UObject> ProductDefaults(NewObject<UObject>(
			GetTransientPackage(),
			StagedSettingsObject->GetClass(),
			NAME_None,
			RF_Transient));
		FText ErrorText;
		if (!ProductDefaults.IsValid() || !ResetDefaults(ProductDefaults.Get(), ErrorText))
		{
			UE_LOG(LogTemp, Error, TEXT("%s: Failed to build product defaults for %s: %s"),
				*FString(__FUNCTION__),
				*GetNameSafe(StagedSettingsObject->GetClass()),
				*ErrorText.ToString());
			return TStrongObjectPtr<UObject>();
		}

		return ProductDefaults;
	}

	static bool IsDifferentFromProductDefault(
		const UObject* StagedSettingsObject,
		const UObject* ProductDefaults,
		const FProperty* Property)
	{
		return IsValid(StagedSettingsObject)
			&& IsValid(ProductDefaults)
			&& Property != nullptr
			&& !Property->Identical_InContainer(StagedSettingsObject, ProductDefaults);
	}

	static bool ResetProperty(
		UObject* StagedSettingsObject,
		const UObject* ProductDefaults,
		const TSharedPtr<IPropertyHandle>& PropertyHandle)
	{
		FProperty* Property = PropertyHandle.IsValid() ? PropertyHandle->GetProperty() : nullptr;
		if (!IsValid(StagedSettingsObject) || !IsValid(ProductDefaults) || Property == nullptr)
		{
			return false;
		}

		PropertyHandle->NotifyPreChange();
		Property->CopyCompleteValue_InContainer(StagedSettingsObject, ProductDefaults);
		PropertyHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
		PropertyHandle->NotifyFinishedChangingProperties();
		return true;
	}

	class FCustomization final : public IDetailCustomization
	{
	public:
		FCustomization(
			UObject* InStagedSettingsObject,
			TFunction<bool(UObject*, FText&)> InResetDefaults,
			TFunction<void(IDetailLayoutBuilder&)> InCustomizeDetails)
			: StagedSettingsObject(InStagedSettingsObject)
			, ProductDefaults(BuildProductDefaults(InStagedSettingsObject, InResetDefaults))
			, CustomizeProductDetails(MoveTemp(InCustomizeDetails))
		{
		}

		virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
		{
			if (CustomizeProductDetails)
			{
				CustomizeProductDetails(DetailBuilder);
			}

			UObject* StagedObject = StagedSettingsObject.Get();
			if (!IsValid(StagedObject) || !ProductDefaults.IsValid())
			{
				return;
			}

			for (TFieldIterator<FProperty> PropertyIt(StagedObject->GetClass(), EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
			{
				FProperty* Property = *PropertyIt;
				if (Property == nullptr
					|| !Property->HasAnyPropertyFlags(CPF_Edit)
					|| Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
				{
					continue;
				}

				const TSharedRef<IPropertyHandle> PropertyHandle = DetailBuilder.GetProperty(Property->GetFName(), StagedObject->GetClass());
				IDetailPropertyRow* PropertyRow = DetailBuilder.EditDefaultProperty(PropertyHandle);
				if (PropertyRow == nullptr || !PropertyHandle->IsValidHandle())
				{
					continue;
				}

				const TWeakObjectPtr<UObject> WeakStagedObject = StagedSettingsObject;
				const TWeakObjectPtr<UObject> WeakProductDefaults(ProductDefaults.Get());
				PropertyRow->OverrideResetToDefault(FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([WeakStagedObject, WeakProductDefaults](const TSharedPtr<IPropertyHandle> Handle)
					{
						return IsDifferentFromProductDefault(
							WeakStagedObject.Get(),
							WeakProductDefaults.Get(),
							Handle.IsValid() ? Handle->GetProperty() : nullptr);
					}),
					FResetToDefaultHandler::CreateLambda([WeakStagedObject, WeakProductDefaults](const TSharedPtr<IPropertyHandle> Handle)
					{
						ResetProperty(WeakStagedObject.Get(), WeakProductDefaults.Get(), Handle);
					})));
			}
		}

		bool ResetPropertyByName(const FName PropertyName)
		{
			UObject* StagedObject = StagedSettingsObject.Get();
			FProperty* Property = IsValid(StagedObject)
				? StagedObject->GetClass()->FindPropertyByName(PropertyName)
				: nullptr;
			if (Property == nullptr || !ProductDefaults.IsValid())
			{
				return false;
			}

			StagedObject->PreEditChange(Property);
			Property->CopyCompleteValue_InContainer(StagedObject, ProductDefaults.Get());
			FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
			StagedObject->PostEditChangeProperty(ChangedEvent);
			return true;
		}

		// Read-only twin of the registered FIsResetToDefaultVisible override lambda: evaluates the
		// exact predicate the arrow visibility uses (staged value differs from the product-defaults
		// snapshot) for a named property, without notifying or touching any state.
		bool IsResetToDefaultVisibleByName(const FName PropertyName) const
		{
			UObject* StagedObject = StagedSettingsObject.Get();
			FProperty* Property = IsValid(StagedObject)
				? StagedObject->GetClass()->FindPropertyByName(PropertyName)
				: nullptr;
			return IsDifferentFromProductDefault(StagedObject, ProductDefaults.Get(), Property);
		}

	private:
		TWeakObjectPtr<UObject> StagedSettingsObject;
		TStrongObjectPtr<UObject> ProductDefaults;
		TFunction<void(IDetailLayoutBuilder&)> CustomizeProductDetails;
	};
}

namespace AudioFactoryPluginWindowAdapter
{

	struct FLiveSettingsWindowEntry
	{
		TWeakPtr<SWindow> Window;
		TWeakPtr<SAudioFactoryPluginSettingsFrameAdapter> SettingsFrame;
	};

	static TMap<EAudioFactoryProductId, FLiveSettingsWindowEntry>& GetLiveSettingsWindows()
	{
		static TMap<EAudioFactoryProductId, FLiveSettingsWindowEntry> LiveSettingsWindows;
		return LiveSettingsWindows;
	}

	// Contract: true only while Slate still owns the window. Product settings windows are added
	// as NATIVE CHILDREN of the editor root (the shared parenting idiom), and native
	// children live in their parent's child list, not in GetInteractiveTopLevelWindows() - a
	// top-level-only Contains misreads every parented live window as destroyed and breaks the
	// single-window contract in a real editor session. FSlateWindowHelper recurses child lists.
	static bool IsWindowAliveInSlate(const TSharedRef<SWindow>& Window)
	{
		return FSlateApplication::IsInitialized()
			&& FSlateWindowHelper::ContainsWindow(FSlateApplication::Get().GetInteractiveTopLevelWindows(), Window);
	}

	enum class EPrimaryActionKind : uint8
	{
		None,
		Apply,
		Ok,
		PrimaryAction,
	};

	static FText TextOrDefault(const FText& Candidate, const FText& Fallback)
	{
		return Candidate.IsEmpty() ? Fallback : Candidate;
	}

	static FName MakeViewIdentifier(const FAudioFactoryPluginWindowSpec& Spec)
	{
		if (!Spec.DetailsViewIdentifier.IsNone())
		{
			return Spec.DetailsViewIdentifier;
		}

		if (!Spec.PluginId.IsNone())
		{
			return FName(*FString::Printf(TEXT("AudioFactory.PluginSettings.%s"), *Spec.PluginId.ToString()));
		}

		return TEXT("AudioFactory.PluginSettings");
	}

	static bool HasAssetTypeFilter(const FAudioFactoryPluginAssetTypeSupport& AssetTypes);
	static bool IsSupportedAssetCount(
		const FAudioFactoryPluginWindowAssetCount& AssetCount,
		const FAudioFactoryPluginAssetTypeSupport& AssetTypes);

	static TArray<FAudioFactoryInlineCount> MakeInlineCounts(
		const FAudioFactoryPluginSettingsFrameSpec& Spec,
		UObject* StagedSettingsObject)
	{
		if (Spec.OnBuildCounts && StagedSettingsObject != nullptr)
		{
			return Spec.OnBuildCounts(StagedSettingsObject);
		}

		if (!Spec.Counts.IsEmpty())
		{
			return Spec.Counts;
		}

		TArray<FAudioFactoryInlineCount> Counts;
		Counts.Reserve(Spec.NativeSpec.AssetCounts.Num());
		for (const FAudioFactoryPluginWindowAssetCount& AssetCount : Spec.NativeSpec.AssetCounts)
		{
			if (!IsSupportedAssetCount(AssetCount, Spec.AssetTypes))
			{
				continue;
			}

			Counts.Add(FAudioFactoryInlineCount::Make(
				AssetCount.Label,
				FText::AsNumber(AssetCount.Count),
				AssetCount.Tooltip));
		}
		return Counts;
	}

	static bool HasAssetTypeFilter(const FAudioFactoryPluginAssetTypeSupport& AssetTypes)
	{
		return AssetTypes.bSupportsMetaSounds || AssetTypes.bSupportsSoundCues || AssetTypes.bSupportsSoundWaves;
	}

	static bool IsSupportedAssetCount(
		const FAudioFactoryPluginWindowAssetCount& AssetCount,
		const FAudioFactoryPluginAssetTypeSupport& AssetTypes)
	{
		if (!HasAssetTypeFilter(AssetTypes))
		{
			return true;
		}

		// Filter on the explicit asset-class enum, never on the (localized) label text.
		switch (AssetCount.Type)
		{
		case EAudioFactoryAssetCountType::MetaSounds:
			return AssetTypes.bSupportsMetaSounds;
		case EAudioFactoryAssetCountType::SoundCues:
			return AssetTypes.bSupportsSoundCues;
		case EAudioFactoryAssetCountType::SoundWaves:
			return AssetTypes.bSupportsSoundWaves;
		case EAudioFactoryAssetCountType::Generic:
		default:
			return true;
		}
	}

	static FText MakeWindowTitle(const FAudioFactoryPluginWindowSpec& Spec)
	{
		return TextOrDefault(
			Spec.WindowTitle,
			TextOrDefault(Spec.HeaderTitle, NSLOCTEXT("AudioFactoryPluginWindowAdapter", "DefaultWindowTitle", "AudioFactory Settings")));
	}

	static TSet<const SWidget*>& GetRegisteredSettingsFrameWidgets()
	{
		static TSet<const SWidget*> RegisteredWidgets;
		return RegisteredWidgets;
	}

	struct FSettingsFrameRegistrationToken
	{
		explicit FSettingsFrameRegistrationToken(const SWidget* InWidget)
			: Widget(InWidget)
		{
		}

		~FSettingsFrameRegistrationToken()
		{
			GetRegisteredSettingsFrameWidgets().Remove(Widget);
		}

		const SWidget* Widget = nullptr;
	};
}

#if WITH_DEV_AUTOMATION_TESTS
// Refusal-toast coverage hooks, same spirit as the Pro window's deferred-rejection hooks.
// Incremented ONLY after FSlateNotificationManager::AddNotification actually returns a real
// item in ShowActionFailure's non-modal branch, so deleting that call leaves the count at
// zero and turns coverage red instead of green. On the failure path (AddNotification returns
// null) nothing is recorded: no count increment, no text, no item.
//
// These live at global scope, NOT inside namespace AudioFactoryPluginWindowAdapter, because
// their only writers are ShowActionFailure (a member of this file's global-scope frame widget)
// and the FAudioFactoryPluginWindowAdapter testing accessors (also defined at global scope);
// unqualified lookup from those scopes cannot enter the helper namespace.
static int32 ActionFailureToastCountForTesting = 0;
static FString LastActionFailureToastTextForTesting;
static TSharedPtr<SNotificationItem> LastActionFailureToastItemForTesting;
#endif

class SAudioFactoryPluginSettingsFrameAdapter : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioFactoryPluginSettingsFrameAdapter)
		: _Spec()
	{
	}
		SLATE_ARGUMENT(FAudioFactoryPluginSettingsFrameSpec, Spec)
		SLATE_ARGUMENT(TWeakPtr<SWindow>, OwnerWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Spec = InArgs._Spec;
		OwnerWindow = InArgs._OwnerWindow;
		RegistrationToken = MakeShared<AudioFactoryPluginWindowAdapter::FSettingsFrameRegistrationToken>(this);
		AudioFactoryPluginWindowAdapter::GetRegisteredSettingsFrameWidgets().Add(this);

		if (Spec.NativeSpec.SourceSettingsObject != nullptr)
		{
			SourceSettingsObject.Reset(Spec.NativeSpec.SourceSettingsObject);
		}

		if (Spec.NativeSpec.StagedSettingsObject != nullptr)
		{
			StagedSettingsObject.Reset(Spec.NativeSpec.StagedSettingsObject);
		}
		else if (Spec.NativeSpec.SourceSettingsObject != nullptr)
		{
			StagedSettingsObject.Reset(FAudioFactoryPluginWindowLifecycle::CreateStagedSettingsObject(Spec.NativeSpec.SourceSettingsObject));
		}

		UpdateValidation();
		ResolvedPathContext = BuildPathContext();
		UpdatePathContextBinding(ResolvedPathContext);

		// This adapter is what SetWidgetToFocusOnActivate is actually pointed at by every call
		// site, but the real Escape/Enter handling lives on the CHILD frame below
		// (SAudioFactoryPluginWindowFrame::OnKeyDown). FSlateApplication::SetUserFocus only walks
		// UP to ancestors looking for a focusable widget, never down into children, so focus never
		// reaches the frame and Escape/Enter go unreachable. Keep the frame's OnKeyDown as the
		// single source of truth for the key handling itself; the adapter just needs to be
		// focusable and forward the event to the child that already knows what to do with it, so
		// callers keep focusing exactly what BuildPluginSettingsFrame returns.
		TSharedRef<SAudioFactoryPluginWindowFrame> Frame = BuildFrame();

		FrameWidget = Frame;

		ChildSlot
		[
			Frame
		];
	}

	// Makes SetWidgetToFocusOnActivate(SettingsContent) actually reach keyboard focus: without
	// this override SCompoundWidget's default SupportsKeyboardFocus() is false, so
	// FSlateApplication::SetUserFocus walks up to the nearest focusable ancestor (the SWindow
	// itself) and Escape/Enter never reach OnKeyDown below. Forwarding to the child frame keeps
	// the actual key-handling logic in exactly one place (SAudioFactoryPluginWindowFrame::OnKeyDown).
	virtual bool SupportsKeyboardFocus() const override
	{
		return true;
	}

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
	{
		if (const TSharedPtr<SAudioFactoryPluginWindowFrame> Frame = FrameWidget.Pin())
		{
			return Frame->OnKeyDown(MyGeometry, InKeyEvent);
		}
		return FReply::Unhandled();
	}

	/** Replace the invocation data in place while preserving the window, details view, and frame tree. */
	void Retarget(FAudioFactoryPluginSettingsFrameSpec InSpec)
	{
		if (DetailsView.IsValid() && StagedSettingsObject.IsValid())
		{
			DetailsView->UnregisterInstancedCustomPropertyLayout(StagedSettingsObject->GetClass());
		}
		FAudioFactoryPluginWindowLifecycle::DiscardStagedSettingsObject(StagedSettingsObject.Get());

		Spec = MoveTemp(InSpec);
		SourceSettingsObject.Reset(Spec.NativeSpec.SourceSettingsObject);
		if (Spec.NativeSpec.StagedSettingsObject != nullptr)
		{
			StagedSettingsObject.Reset(Spec.NativeSpec.StagedSettingsObject);
		}
		else if (Spec.NativeSpec.SourceSettingsObject != nullptr)
		{
			StagedSettingsObject.Reset(FAudioFactoryPluginWindowLifecycle::CreateStagedSettingsObject(Spec.NativeSpec.SourceSettingsObject));
		}
		else
		{
			StagedSettingsObject.Reset();
		}

		ResolvedPathContext = BuildPathContext();
		UpdatePathContextBinding(ResolvedPathContext);

		if (DetailsView.IsValid())
		{
			InstallPropertyResetCustomization();
			DetailsView->SetObject(StagedSettingsObject.Get(), true);
		}

		RefreshActionBindings();
		UpdateValidation();
	}

private:
	void InstallPropertyResetCustomization()
	{
		PropertyResetCustomization.Reset();
		if (!DetailsView.IsValid()
			|| !StagedSettingsObject.IsValid()
			|| (!Spec.NativeSpec.OnResetDefaults && !Spec.NativeSpec.OnCustomizeDetails))
		{
			return;
		}

		PropertyResetCustomization = MakeShared<AudioFactoryPluginPropertyReset::FCustomization>(
			StagedSettingsObject.Get(),
			Spec.NativeSpec.OnResetDefaults,
			Spec.NativeSpec.OnCustomizeDetails);
		const TSharedPtr<AudioFactoryPluginPropertyReset::FCustomization> ResetCustomization = PropertyResetCustomization;
		DetailsView->RegisterInstancedCustomPropertyLayout(
			StagedSettingsObject->GetClass(),
			FOnGetDetailCustomizationInstance::CreateLambda([ResetCustomization]() -> TSharedRef<IDetailCustomization>
			{
				return ResetCustomization.ToSharedRef();
			}));
	}

	TSharedRef<SWidget> BuildContextContent()
	{
		if (!Spec.NativeSpec.OnBuildContextContent || !StagedSettingsObject.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		return Spec.NativeSpec.OnBuildContextContent(StagedSettingsObject.Get());
	}

	TOptional<FAudioFactoryPathContext> BuildPathContext() const
	{
		if (Spec.OnBuildPathContext && StagedSettingsObject.IsValid())
		{
			return Spec.OnBuildPathContext(StagedSettingsObject.Get());
		}

		return Spec.PathContext;
	}

	TArray<FAudioFactoryInlineCount> GetResolvedCounts() const
	{
		return ResolvedCounts;
	}

	void UpdatePathContextBinding(const TOptional<FAudioFactoryPathContext>& InPathContext)
	{
		if (!PathContextBinding.IsValid())
		{
			PathContextBinding = MakeShared<FAudioFactoryPathContext>();
		}

		if (InPathContext.IsSet())
		{
			*PathContextBinding = InPathContext.GetValue();
		}
		else
		{
			*PathContextBinding = FAudioFactoryPathContext();
		}

		if (!PathContextProxy.IsSet() && InPathContext.IsSet())
		{
			const TSharedRef<FAudioFactoryPathContext> Binding = PathContextBinding.ToSharedRef();
			FAudioFactoryPathContext Proxy;
			Proxy.Path = TAttribute<FText>::CreateLambda([Binding]()
			{
				return Binding->Path.IsSet() ? Binding->Path.Get() : FText::GetEmpty();
			});
			Proxy.OnAction = [Binding]()
			{
				return Binding->OnAction ? Binding->OnAction() : FReply::Handled();
			};
			Proxy.IsResetToDefaultVisible = TAttribute<bool>::CreateLambda([Binding]()
			{
				return Binding->IsResetToDefaultVisible.Get(false);
			});
			Proxy.OnResetToDefault = [Binding]()
			{
				return Binding->OnResetToDefault ? Binding->OnResetToDefault() : FReply::Handled();
			};
			PathContextProxy = MoveTemp(Proxy);
		}

		if (PathContextProxy.IsSet())
		{
			PathContextProxy->Label = PathContextBinding->Label;
			PathContextProxy->ToolTip = PathContextBinding->ToolTip;
		}
	}

	TOptional<FAudioFactoryPathContext> GetPathContextForFrame() const
	{
		return PathContextProxy;
	}

	void RefreshActionBindingArray(
		TArray<TSharedPtr<FAudioFactoryFooterAction>>& ActionBindings,
		const TArray<FAudioFactoryFooterAction>& NewActions,
		const TCHAR* ActionRowName)
	{
		if (ActionBindings.Num() != NewActions.Num())
		{
			ensureMsgf(
				false,
				TEXT("AudioFactory settings retarget for %s changed the %s action count from %d to %d; preserving the existing Slate action row."),
				*Spec.NativeSpec.PluginId.ToString(),
				ActionRowName,
				ActionBindings.Num(),
				NewActions.Num());
		}

		const int32 NumBindingsToRefresh = FMath::Min(ActionBindings.Num(), NewActions.Num());
		for (int32 ActionIndex = 0; ActionIndex < NumBindingsToRefresh; ++ActionIndex)
		{
			if (ActionBindings[ActionIndex].IsValid())
			{
				*ActionBindings[ActionIndex] = NewActions[ActionIndex];
			}
		}

		for (int32 ActionIndex = NumBindingsToRefresh; ActionIndex < ActionBindings.Num(); ++ActionIndex)
		{
			if (ActionBindings[ActionIndex].IsValid())
			{
				ActionBindings[ActionIndex]->bIsEnabled = false;
				ActionBindings[ActionIndex]->IsEnabled = false;
				ActionBindings[ActionIndex]->OnClicked = TFunction<FReply()>();
			}
		}
	}

	void RefreshActionBindings()
	{
		RefreshActionBindingArray(HeaderActionBindings, BuildHeaderActions(), TEXT("header"));
		RefreshActionBindingArray(FooterActionBindings, BuildFooterActions(), TEXT("footer"));
	}

	enum class EActionRowKind : uint8
	{
		Header,
		Footer,
	};

	TSharedRef<SAudioFactoryPluginWindowFrame> BuildFrame()
	{
		return SNew(SAudioFactoryPluginWindowFrame)
			.ProductId(Spec.ProductId)
			.Counts(TAttribute<TArray<FAudioFactoryInlineCount>>::Create(
				TAttribute<TArray<FAudioFactoryInlineCount>>::FGetter::CreateSP(
					this,
					&SAudioFactoryPluginSettingsFrameAdapter::GetResolvedCounts)))
			.PathContext(GetPathContextForFrame())
			// Validation feedback remains available through the primary-action tooltip/dialog,
			// not through a settings-window status banner.
			.Status_Lambda([]() -> FAudioFactoryStatus { return FAudioFactoryStatus(); })
			.OnCancelRequested([this]()
			{
				HandleCancelClicked();
			})
			.OnPrimaryRequested([this]()
			{
				HandlePrimaryButtonClicked();
			})
			.HeaderTools()
			[
				BuildHeaderToolActions()
			]
			.ContextContent()
			[
				BuildContextContent()
			]
			.Body()
			[
				BuildDetailsBody()
			]
			.FooterUtilityActions()
			[
				BuildFooterUtilityActions()
			]
			.CancelAction()
			[
				BuildCancelAction()
			]
			.PrimaryAction()
			[
				BuildPrimaryAction()
			];
	}

	TSharedRef<SWidget> BuildIconActionRow(
		const TArray<FAudioFactoryFooterAction>& ActionsToBuild,
		const bool bRefreshValidationAfterClick,
		const bool bUseFooterTextButtons,
		EActionRowKind ActionRowKind)
	{
		TArray<TSharedPtr<FAudioFactoryFooterAction>>& ActionBindings =
			ActionRowKind == EActionRowKind::Header ? HeaderActionBindings : FooterActionBindings;
		ActionBindings.Reset();
		ActionBindings.Reserve(ActionsToBuild.Num());

		TSharedRef<SHorizontalBox> Actions = SNew(SHorizontalBox);
		int32 NumActions = 0;

		for (const FAudioFactoryFooterAction& SourceAction : ActionsToBuild)
		{
			FAudioFactoryFooterAction Action = SourceAction;
			const TSharedPtr<FAudioFactoryFooterAction> ActionBinding = MakeShared<FAudioFactoryFooterAction>(SourceAction);
			ActionBindings.Add(ActionBinding);
			Action.IsEnabled = TAttribute<bool>::CreateLambda([ActionBinding]()
			{
				return ActionBinding->IsEnabled.IsSet() ? ActionBinding->IsEnabled.Get() : ActionBinding->bIsEnabled;
			});
			Action.OnClicked = [ActionBinding]()
			{
				return ActionBinding->OnClicked ? ActionBinding->OnClicked() : FReply::Handled();
			};
			if (bRefreshValidationAfterClick)
			{
				TFunction<FReply()> OriginalOnClicked = Action.OnClicked;
				const TWeakPtr<SAudioFactoryPluginSettingsFrameAdapter> WeakFrame = SharedThis(this);
				Action.OnClicked = [WeakFrame, OriginalOnClicked]()
				{
					const FReply Reply = OriginalOnClicked ? OriginalOnClicked() : FReply::Handled();
					// If the product callback destroyed this widget, skip revalidation instead of
					// calling through a raw pointer to the dead adapter.
					if (const TSharedPtr<SAudioFactoryPluginSettingsFrameAdapter> PinnedThis = WeakFrame.Pin())
					{
						PinnedThis->UpdateValidation();
					}
					return Reply;
				};
			}

			Actions->AddSlot()
			.AutoWidth()
			.Padding(NumActions > 0 ? FMargin(6.0f, 0.0f, 0.0f, 0.0f) : FMargin(0.0f))
			[
				bUseFooterTextButtons
					? StaticCastSharedRef<SWidget>(
						SNew(SAudioFactoryFooterTextAction)
						.Action(Action))
					: Action.bUseLabel
					? StaticCastSharedRef<SWidget>(
						SNew(SAudioFactoryHeaderLabeledButton)
						.IconName(Action.IconName)
						.Label(Action.Label)
						.ToolTipText(Action.ToolTip)
						.IsEnabled(Action.IsEnabled.IsSet() ? Action.IsEnabled : TAttribute<bool>(Action.bIsEnabled))
						.OnClicked(Action.OnClicked))
					: StaticCastSharedRef<SWidget>(
						SNew(SAudioFactoryUtilityIconAction)
						.Action(Action))
			];
			++NumActions;
		}

		return NumActions > 0 ? StaticCastSharedRef<SWidget>(Actions) : StaticCastSharedRef<SWidget>(SNullWidget::NullWidget);
	}

	TArray<FAudioFactoryFooterAction> BuildHeaderActions() const
	{
		if (!Spec.OnBuildHeaderToolActions || !StagedSettingsObject.IsValid())
		{
			return TArray<FAudioFactoryFooterAction>();
		}

		return Spec.OnBuildHeaderToolActions(StagedSettingsObject.Get());
	}

	TSharedRef<SWidget> BuildHeaderToolActions()
	{
		return BuildIconActionRow(BuildHeaderActions(), true, false, EActionRowKind::Header);
	}

	TSharedRef<SWidget> BuildDetailsBody()
	{
		if (StagedSettingsObject.IsValid())
		{
			FAudioFactoryPluginWindowDetailsViewOptions DetailsOptions;
			DetailsOptions.ViewIdentifier = AudioFactoryPluginWindowAdapter::MakeViewIdentifier(Spec.NativeSpec);
			DetailsOptions.bAllowAdvancedProperties = true;
			DetailsOptions.bShowScrollBar = true;

			DetailsView = FAudioFactoryPluginWindowDetails::CreateDetailsView(DetailsOptions);
			InstallPropertyResetCustomization();
			DetailsView->SetObject(StagedSettingsObject.Get(), true);
			DetailsView->OnFinishedChangingProperties().AddSP(
				this,
				&SAudioFactoryPluginSettingsFrameAdapter::HandleFinishedChangingProperties);
		}

		// Body backdrop is the recessed engine surface, details view placed directly with zero
		// padding so rows run edge-to-edge exactly like Project Settings. No group border, no gutter.
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FAudioFactoryStyle::Get().PluginWindowTokens().Recessed)
			.Padding(0.0f)
			[
				DetailsView.IsValid()
					? StaticCastSharedRef<SWidget>(DetailsView.ToSharedRef())
					: StaticCastSharedRef<SWidget>(
						SNew(SBox)
						.Padding(FMargin(12.0f))
						[
							SNew(STextBlock)
							.Text(LOCTEXT("MissingSettingsObject", "No staged settings object is available."))
							.ColorAndOpacity(FSlateColor(FAudioFactoryStyle::Get().PluginWindowTokens().StatusWarning))
						])
			];
	}

	TArray<FAudioFactoryFooterAction> BuildFooterActions()
	{
		TArray<FAudioFactoryFooterAction> FooterActions;
		if (Spec.OnBuildExtraFooterActions && StagedSettingsObject.IsValid())
		{
			FooterActions.Append(Spec.OnBuildExtraFooterActions(StagedSettingsObject.Get()));
		}

		if (Spec.NativeSpec.bShowCopyFileListButton && Spec.NativeSpec.OnCopyFileList)
		{
			FooterActions.Add(FAudioFactoryFooterAction::MakeLabeled(
				TEXT("AudioFactory.Glyph.Copy"),
				AudioFactoryPluginWindowAdapter::TextOrDefault(Spec.NativeSpec.CopyFileListLabel, LOCTEXT("CopyFileListLabel", "Copy File List")),
				LOCTEXT("CopyFileListTooltip", "Copy the list of files this run will write to the clipboard."),
				[this]()
				{
					return HandleCopyFileListClicked();
				}));
		}

		// When the flag is false or no callback exists, do not add a report action or show its button.
		if (HasOpenReportAction())
		{
			FooterActions.Add(FAudioFactoryFooterAction::MakeLabeled(
				TEXT("AudioFactory.Glyph.Logs"),
				AudioFactoryPluginWindowAdapter::TextOrDefault(Spec.NativeSpec.OpenReportLabel, LOCTEXT("OpenReportLabel", "Logs")),
				LOCTEXT("OpenReportTooltip", "Open the folder where logs and reports are saved."),
				[this]()
				{
					return HandleOpenReportClicked();
				}));
		}

		return FooterActions;
	}

	TSharedRef<SWidget> BuildFooterUtilityActions()
	{
		return BuildIconActionRow(BuildFooterActions(), true, true, EActionRowKind::Footer);
	}

	TSharedRef<SWidget> BuildCancelAction()
	{
		if (!Spec.NativeSpec.bShowCancelButton)
		{
			return SNullWidget::NullWidget;
		}

		return SNew(SBox)
			.MinDesiredWidth(88.0f)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(AudioFactoryPluginWindowAdapter::TextOrDefault(Spec.NativeSpec.CancelLabel, LOCTEXT("CancelButton", "Cancel")))
				.ContentPadding(FMargin(16.0f, 4.0f))
				.OnClicked(this, &SAudioFactoryPluginSettingsFrameAdapter::HandleCancelClicked)
			];
	}

	TSharedRef<SWidget> BuildPrimaryAction()
	{
		if (GetPrimaryActionKind() == AudioFactoryPluginWindowAdapter::EPrimaryActionKind::None)
		{
			return SNullWidget::NullWidget;
		}

		const FAudioFactoryProductIdentity& Identity = FAudioFactoryProductRegistry::Get(Spec.ProductId);
		FAudioFactoryPrimaryAction Action;
		Action.Label = GetPrimaryActionText();
		Action.ToolTip = GetPrimaryActionToolTip();
		Action.IsEnabled = TAttribute<bool>::Create(
			TAttribute<bool>::FGetter::CreateSP(this, &SAudioFactoryPluginSettingsFrameAdapter::IsPrimaryButtonEnabled));
		Action.OnClicked = [this]()
		{
			return HandlePrimaryButtonClicked();
		};

		return SNew(SAudioFactoryPrimaryActionButton)
			.Action(Action)
			.ProductId(Identity.ProductId)
			.Visibility(TAttribute<EVisibility>::Create(
				TAttribute<EVisibility>::FGetter::CreateSP(
					this,
					&SAudioFactoryPluginSettingsFrameAdapter::GetPrimaryActionVisibility)));
	}

	void UpdateValidation()
	{
		if (!StagedSettingsObject.IsValid())
		{
			ValidationResult = FAudioFactoryPluginWindowValidationResult::Error(
				LOCTEXT("ValidationNoStagedObject", "No staged settings object is available."));
		}
		else if (Spec.NativeSpec.OnValidate)
		{
			ValidationResult = Spec.NativeSpec.OnValidate(StagedSettingsObject.Get());
			if (ValidationResult.Message.IsEmpty())
			{
				ValidationResult.Message = LOCTEXT("ValidationReadyFallback", "Ready");
			}
		}
		else
		{
			ValidationResult = FAudioFactoryPluginWindowValidationResult::Valid(LOCTEXT("ValidationReadyText", "Ready"));
		}

		// Recompute counts on this same revalidation path so staged selection changes update
		// the header and the primary action state together.
		ResolvedCounts = AudioFactoryPluginWindowAdapter::MakeInlineCounts(Spec, StagedSettingsObject.Get());

		// Recompute the bound button state and invalidate this frame so Slate observes the
		// result immediately after a footer or utility callback changes staged state.
		Invalidate(EInvalidateWidgetReason::Paint);
	}

	bool CommitStagedSettings(FText& OutError)
	{
		if (!SourceSettingsObject.IsValid() || !StagedSettingsObject.IsValid())
		{
			OutError = LOCTEXT("CommitMissingObject", "A source or staged settings object is missing.");
			return false;
		}

		bIsApplying = true;
		ON_SCOPE_EXIT
		{
			bIsApplying = false;
		};

		if (Spec.NativeSpec.OnApplySettings)
		{
			if (!Spec.NativeSpec.OnApplySettings(SourceSettingsObject.Get(), StagedSettingsObject.Get(), OutError))
			{
				return false;
			}
		}
		else
		{
			FAudioFactoryPluginWindowApplyOptions ApplyOptions;
			ApplyOptions.bSaveConfig = Spec.NativeSpec.bSaveConfigOnDefaultApply;
			ApplyOptions.bFlushConfig = Spec.NativeSpec.bFlushConfigOnDefaultApply;
			if (!FAudioFactoryPluginWindowLifecycle::ApplyStagedSettings(
				SourceSettingsObject.Get(),
				StagedSettingsObject.Get(),
				ApplyOptions,
				&OutError))
			{
				return false;
			}
		}

		UpdateValidation();
		return true;
	}

	void ShowActionFailure(const FText& ErrorText, const bool bShowModal = true)
	{
		// Keep the callback's non-empty text exactly. When it supplied no text, show a useful
		// fallback that directs the user to the Output Log for the real detail.
		const FText DialogText = ErrorText.IsEmpty()
			? LOCTEXT("SettingsActionFailedNoDetails", "The settings action failed without details. See the Output Log for more information.")
			: ErrorText;
		if (!bShowModal)
		{
			// Non-modal action failures (refused primary actions) must always produce visible
			// feedback, but without disturbing any pinned settings-window surface: validation
			// state, the primary button's enabled state and tooltip, and the rendered text census
			// stay untouched so a refused press keeps the window exactly as it was. The editor
			// notification area hosts the refusal instead; it hosts itself even when no settings
			// window is alive at all (for example a second Convert pressed after the first one
			// closed the window), so no refusal degrades to silence.
			FNotificationInfo NotificationInfo(DialogText);
			NotificationInfo.ExpireDuration = 5.0f;
			NotificationInfo.bUseSuccessFailIcons = true;
			if (const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(NotificationInfo))
			{
				Notification->SetCompletionState(SNotificationItem::CS_Fail);
				// Remember a callback-surfaced refusal so the click handler does not post a second toast.
				bNonModalFailureToastShownForCurrentAction = true;
#if WITH_DEV_AUTOMATION_TESTS
				// Recorded only on the success branch: a null AddNotification return must leave
				// the coverage counters untouched so deleting this call cannot pass invisibly.
				++ActionFailureToastCountForTesting;
				LastActionFailureToastTextForTesting = DialogText.ToString();
				LastActionFailureToastItemForTesting = Notification;
#endif
			}
			return;
		}
		ValidationResult = FAudioFactoryPluginWindowValidationResult::Error(DialogText);
		Invalidate(EInvalidateWidgetReason::Paint);
		if (bShowModal)
		{
			FMessageDialog::Open(
				EAppMsgType::Ok,
				DialogText,
				LOCTEXT("SettingsActionFailedTitle", "Settings action failed"));
		}
	}

	void RequestClose()
	{
		if (const TSharedPtr<SWindow> Window = OwnerWindow.Pin())
		{
			Window->RequestDestroyWindow();
		}
	}

	FReply HandleCancelClicked()
	{
		FAudioFactoryPluginWindowLifecycle::DiscardStagedSettingsObject(StagedSettingsObject.Get());
		if (Spec.NativeSpec.OnCancel)
		{
			Spec.NativeSpec.OnCancel();
		}
		RequestClose();
		return FReply::Handled();
	}

	FReply HandleApplyClicked()
	{
		FText ErrorText;
		if (!CommitStagedSettings(ErrorText))
		{
			// When Apply cannot commit, keep the window and staged edits intact while showing its exact error.
			ShowActionFailure(ErrorText);
		}
		return FReply::Handled();
	}

	FReply HandleOkClicked()
	{
		FText ErrorText;
		if (!CommitStagedSettings(ErrorText))
		{
			// When OK cannot commit, keep the window and staged edits intact while showing its exact error.
			ShowActionFailure(ErrorText);
			return FReply::Handled();
		}

		if (Spec.NativeSpec.bCloseOnOk)
		{
			RequestClose();
		}
		return FReply::Handled();
	}

	/** Execute the primary action and preserve acceptance truth for legacy product callbacks. */
	bool InvokePrimaryAction(FText& OutError)
	{
		if (!Spec.NativeSpec.OnPrimaryAction.IsBound())
		{
			OutError = LOCTEXT("PrimaryActionUnavailable", "The primary action is not available.");
			return false;
		}

		if (Spec.NativeSpec.OnPrimaryAction.ResultCallback)
		{
			const bool bAccepted = Spec.NativeSpec.OnPrimaryAction.ResultCallback(SourceSettingsObject.Get(), OutError);
			if (!bAccepted && OutError.IsEmpty())
			{
				OutError = LOCTEXT("PrimaryActionRejected", "The request was not accepted. The settings window remains open.");
			}
			return bAccepted;
		}

		bool bCloseWasRequested = false;
		const TSharedPtr<SWindow> Window = OwnerWindow.Pin();
		if (Window.IsValid())
		{
			Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride::CreateLambda(
				[&bCloseWasRequested](const TSharedRef<SWindow>& RequestedWindow)
				{
					bCloseWasRequested = true;
					FSlateApplication::Get().RequestDestroyWindow(RequestedWindow);
				}));
		}

		Spec.NativeSpec.OnPrimaryAction.LegacyCallback(SourceSettingsObject.Get());

		if (Window.IsValid())
		{
			// Restore the normal deferred close path before the action returns to Slate.
			Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		}

		if (!bCloseWasRequested)
		{
			OutError = LOCTEXT("LegacyPrimaryActionRejected", "The request was not accepted. The settings window remains open.");
		}
		return bCloseWasRequested;
	}

	FReply HandlePrimaryActionClicked()
	{
		bNonModalFailureToastShownForCurrentAction = false;
		FText ErrorText;
		if (!CommitStagedSettings(ErrorText))
		{
			// When the primary action cannot commit, keep the window and staged edits intact while showing its exact error.
			ShowActionFailure(ErrorText);
			return FReply::Handled();
		}

		if (!InvokePrimaryAction(ErrorText))
		{
			// Busy or otherwise refused primary actions keep the existing window and all of its
			// current UI state unchanged, but a refusal is never a dead click: the non-modal
			// failure path surfaces the callback's exact reason as an editor notification.
			// Commit failures above retain their established error dialog.
			if (!bNonModalFailureToastShownForCurrentAction)
			{
				ShowActionFailure(ErrorText, /*bShowModal=*/false);
			}
			return FReply::Handled();
		}

		if (Spec.NativeSpec.OnPrimaryAction.ResultCallback)
		{
			RequestClose();
		}
		return FReply::Handled();
	}

	FReply HandleOpenReportClicked()
	{
		// When the consumer does not opt in, omit the report button even if a callback exists.
		// If this guard fails, the click is handled without opening a report.
		if (HasOpenReportAction())
		{
			Spec.NativeSpec.OnOpenReport();
		}
		return FReply::Handled();
	}

	bool HasOpenReportAction() const
	{
		return Spec.NativeSpec.bShowOpenReportButton && Spec.NativeSpec.OnOpenReport;
	}

	FReply HandleCopyFileListClicked()
	{
		if (Spec.NativeSpec.OnCopyFileList)
		{
			Spec.NativeSpec.OnCopyFileList();
		}
		return FReply::Handled();
	}

	FReply HandlePrimaryButtonClicked()
	{
		// A product may intentionally hide its primary action until staged settings
		// contain work to perform. Treat that hidden state as a quiet no-op for both
		// mouse/direct activation and the frame's Enter shortcut.
		if (!IsPrimaryActionVisible())
		{
			return FReply::Handled();
		}

		// Refresh before checking the bound enabled state so Enter and a direct
		// primary-button activation cannot use validation from before the latest
		// staged-settings change, including a native per-property reset.
		UpdateValidation();

		if (GetPrimaryActionKind() == AudioFactoryPluginWindowAdapter::EPrimaryActionKind::PrimaryAction &&
			!IsPrimaryButtonEnabled())
		{
			ShowPrimaryActionValidationDialog();
			return FReply::Handled();
		}

		if (!IsPrimaryButtonEnabled())
		{
			return FReply::Handled();
		}

		switch (GetPrimaryActionKind())
		{
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::Apply:
			return HandleApplyClicked();
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::Ok:
			return HandleOkClicked();
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::PrimaryAction:
			return HandlePrimaryActionClicked();
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::None:
		default:
			return FReply::Handled();
		}
	}

	void HandleFinishedChangingProperties(const FPropertyChangedEvent& /*PropertyChangedEvent*/)
	{
		UpdateValidation();
	}

	AudioFactoryPluginWindowAdapter::EPrimaryActionKind GetPrimaryActionKind() const
	{
		if (Spec.NativeSpec.OnPrimaryAction)
		{
			return AudioFactoryPluginWindowAdapter::EPrimaryActionKind::PrimaryAction;
		}
		if (Spec.NativeSpec.bShowOkButton)
		{
			return AudioFactoryPluginWindowAdapter::EPrimaryActionKind::Ok;
		}
		if (Spec.NativeSpec.bShowApplyButton)
		{
			return AudioFactoryPluginWindowAdapter::EPrimaryActionKind::Apply;
		}
		return AudioFactoryPluginWindowAdapter::EPrimaryActionKind::None;
	}

	bool IsActionEnabled(const AudioFactoryPluginWindowAdapter::EPrimaryActionKind ActionKind) const
	{
		return !bIsApplying
			&& SourceSettingsObject.IsValid()
			&& StagedSettingsObject.IsValid()
			&& ValidationResult.bCanApply
			&& ValidationResult.State != EAudioFactoryPluginWindowValidationState::Error
			&& (ActionKind != AudioFactoryPluginWindowAdapter::EPrimaryActionKind::PrimaryAction ||
				(IsPrimaryActionVisible() && ValidationResult.bCanRunPrimaryAction));
	}

	bool IsPrimaryActionVisible() const
	{
		if (!Spec.NativeSpec.OnIsPrimaryActionVisible)
		{
			return true;
		}

		return StagedSettingsObject.IsValid()
			&& Spec.NativeSpec.OnIsPrimaryActionVisible(StagedSettingsObject.Get());
	}

	EVisibility GetPrimaryActionVisibility() const
	{
		return IsPrimaryActionVisible() ? EVisibility::Visible : EVisibility::Collapsed;
	}

	bool IsApplyEnabled() const
	{
		return IsActionEnabled(AudioFactoryPluginWindowAdapter::EPrimaryActionKind::Apply);
	}

	bool IsPrimaryActionEnabled() const
	{
		return IsActionEnabled(AudioFactoryPluginWindowAdapter::EPrimaryActionKind::PrimaryAction);
	}

	bool IsPrimaryButtonEnabled() const
	{
		return IsActionEnabled(GetPrimaryActionKind());
	}

	void ShowPrimaryActionValidationDialog() const
	{
		const FText ErrorMessage(AudioFactoryPluginWindowAdapter::TextOrDefault(
			ValidationResult.Message,
			LOCTEXT("PrimaryActionUnavailableDialog", "The primary action is not available yet.")));

		FMessageDialog::Open(
			EAppMsgType::Ok,
			ErrorMessage,
			LOCTEXT("PrimaryActionUnavailableTitle", "Action not available"));
	}

	FText GetPrimaryActionText() const
	{
		const FAudioFactoryProductIdentity& Identity = FAudioFactoryProductRegistry::Get(Spec.ProductId);
		switch (GetPrimaryActionKind())
		{
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::Apply:
			return AudioFactoryPluginWindowAdapter::TextOrDefault(Spec.NativeSpec.ApplyLabel, LOCTEXT("PrimaryApplyButton", "Apply"));
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::Ok:
			return AudioFactoryPluginWindowAdapter::TextOrDefault(Spec.NativeSpec.OkLabel, LOCTEXT("PrimaryOkButton", "OK"));
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::PrimaryAction:
			return AudioFactoryPluginWindowAdapter::TextOrDefault(Spec.NativeSpec.PrimaryActionLabel, Identity.PrimaryVerb);
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::None:
		default:
			return FText::GetEmpty();
		}
	}

	FText GetPrimaryActionToolTip() const
	{
		if ((ValidationResult.State == EAudioFactoryPluginWindowValidationState::Warning ||
			ValidationResult.State == EAudioFactoryPluginWindowValidationState::Error) &&
			!ValidationResult.Message.IsEmpty())
		{
			return ValidationResult.Message;
		}

		switch (GetPrimaryActionKind())
		{
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::Apply:
			return LOCTEXT("PrimaryApplyTooltip", "Apply staged settings without closing the window.");
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::Ok:
			return LOCTEXT("PrimaryOkTooltip", "Apply staged settings and close the window.");
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::PrimaryAction:
			return LOCTEXT("PrimaryActionTooltip", "Apply staged settings and run the primary action.");
		case AudioFactoryPluginWindowAdapter::EPrimaryActionKind::None:
		default:
			return FText::GetEmpty();
		}
	}

	bool ResetPropertyToDefaultForAutomation(const FName PropertyName)
	{
		if (!PropertyResetCustomization.IsValid()
			|| !PropertyResetCustomization->ResetPropertyByName(PropertyName))
		{
			return false;
		}

		if (DetailsView.IsValid())
		{
			DetailsView->ForceRefresh();
		}
		UpdateValidation();
		return true;
	}

	bool IsResetToDefaultVisibleByName(const FName PropertyName) const
	{
		return PropertyResetCustomization.IsValid()
			&& PropertyResetCustomization->IsResetToDefaultVisibleByName(PropertyName);
	}

	friend class FAudioFactoryPluginWindowAdapter;

	FAudioFactoryPluginSettingsFrameSpec Spec;
	TArray<FAudioFactoryInlineCount> ResolvedCounts;
	TOptional<FAudioFactoryPathContext> ResolvedPathContext;
	TStrongObjectPtr<UObject> SourceSettingsObject;
	TStrongObjectPtr<UObject> StagedSettingsObject;
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<AudioFactoryPluginPropertyReset::FCustomization> PropertyResetCustomization;
	TWeakPtr<SWindow> OwnerWindow;
	TWeakPtr<SAudioFactoryPluginWindowFrame> FrameWidget;
	TSharedPtr<AudioFactoryPluginWindowAdapter::FSettingsFrameRegistrationToken> RegistrationToken;
	TSharedPtr<FAudioFactoryPathContext> PathContextBinding;
	TOptional<FAudioFactoryPathContext> PathContextProxy;
	TArray<TSharedPtr<FAudioFactoryFooterAction>> HeaderActionBindings;
	TArray<TSharedPtr<FAudioFactoryFooterAction>> FooterActionBindings;
	FAudioFactoryPluginWindowValidationResult ValidationResult;
	bool bIsApplying = false;
	bool bNonModalFailureToastShownForCurrentAction = false;
};

static TSharedPtr<SAudioFactoryPluginSettingsFrameAdapter> CheckedSettingsFrameCast(
	const TSharedRef<SWidget>& SettingsFrame)
{
	// If the caller passes any other widget, report the exact type and return before the
	// unchecked shared-pointer downcast can corrupt memory.
	if (!AudioFactoryPluginWindowAdapter::GetRegisteredSettingsFrameWidgets().Contains(&SettingsFrame.Get()))
	{
		ensureMsgf(
			false,
			TEXT("AudioFactoryPluginWindowAdapter automation expected SAudioFactoryPluginSettingsFrameAdapter, received %s."),
			*SettingsFrame->GetTypeAsString());
		return nullptr;
	}

	const TSharedPtr<SWidget> WidgetPtr = SettingsFrame;
	const TSharedPtr<SAudioFactoryPluginSettingsFrameAdapter> TypedFrame = StaticCastSharedPtr<SAudioFactoryPluginSettingsFrameAdapter>(WidgetPtr);
	if (!TypedFrame.IsValid())
	{
		// If the registry entry is no longer live, fail the automation seam without invoking
		// production behavior through an invalid pointer.
		ensureMsgf(false, TEXT("AudioFactoryPluginWindowAdapter automation received an invalid settings frame."));
		return nullptr;
	}

	return TypedFrame;
}

TSharedRef<SWindow> FAudioFactoryPluginWindowAdapter::OpenPluginSettingsWindow(FAudioFactoryPluginSettingsFrameSpec Spec)
{
	if (const TSharedPtr<SWindow> ExistingWindow = FindLiveSettingsWindow(Spec.ProductId))
	{
		ExistingWindow->SetContent(BuildPluginSettingsFrame(MoveTemp(Spec), ExistingWindow));
		ExistingWindow->BringToFront(true);
		return ExistingWindow.ToSharedRef();
	}

	const FVector2D InitialSize = Spec.NativeSpec.InitialSize;
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(AudioFactoryPluginWindowAdapter::MakeWindowTitle(Spec.NativeSpec))
		.ClientSize(InitialSize)
		.SizingRule(ESizingRule::UserSized)
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	const TWeakPtr<SWindow> WeakWindow = Window;
	Window->SetContent(BuildPluginSettingsFrame(MoveTemp(Spec), WeakWindow));
	FSlateApplication::Get().AddWindow(Window);
	return Window;
}

TSharedPtr<SWindow> FAudioFactoryPluginWindowAdapter::FindLiveSettingsWindow(EAudioFactoryProductId ProductId)
{
	AudioFactoryPluginWindowAdapter::FLiveSettingsWindowEntry* Entry =
		AudioFactoryPluginWindowAdapter::GetLiveSettingsWindows().Find(ProductId);
	if (Entry == nullptr)
	{
		return nullptr;
	}

	const TSharedPtr<SWindow> Window = Entry->Window.Pin();
	if (!Window.IsValid()
		|| !AudioFactoryPluginWindowAdapter::IsWindowAliveInSlate(Window.ToSharedRef()))
	{
		AudioFactoryPluginWindowAdapter::GetLiveSettingsWindows().Remove(ProductId);
		return nullptr;
	}

	return Window;
}

bool FAudioFactoryPluginWindowAdapter::ShowLiveSettingsFailure(
	EAudioFactoryProductId ProductId,
	const FText& ErrorText)
{
	// Superseded: refusal text now surfaces via the non-modal ShowActionFailure toast
	// while window state stays untouched.

	AudioFactoryPluginWindowAdapter::FLiveSettingsWindowEntry* Entry =
		AudioFactoryPluginWindowAdapter::GetLiveSettingsWindows().Find(ProductId);
	if (Entry == nullptr)
	{
		return false;
	}

	const TSharedPtr<SWindow> Window = Entry->Window.Pin();
	const TSharedPtr<SAudioFactoryPluginSettingsFrameAdapter> SettingsFrame = Entry->SettingsFrame.Pin();
	if (!Window.IsValid()
		|| !SettingsFrame.IsValid()
		|| !AudioFactoryPluginWindowAdapter::IsWindowAliveInSlate(Window.ToSharedRef()))
	{
		AudioFactoryPluginWindowAdapter::GetLiveSettingsWindows().Remove(ProductId);
		return false;
	}

	SettingsFrame->ShowActionFailure(ErrorText, /*bShowModal=*/false);
	Window->BringToFront(true);
	return true;
}

TSharedRef<SWidget> FAudioFactoryPluginWindowAdapter::BuildPluginSettingsFrame(
	FAudioFactoryPluginSettingsFrameSpec Spec,
	TWeakPtr<SWindow> OwnerWindow)
{
	const EAudioFactoryProductId ProductId = Spec.ProductId;
	if (AudioFactoryPluginWindowAdapter::FLiveSettingsWindowEntry* Entry =
		AudioFactoryPluginWindowAdapter::GetLiveSettingsWindows().Find(ProductId))
	{
		const TSharedPtr<SWindow> ExistingWindow = Entry->Window.Pin();
		if (ExistingWindow.IsValid()
			&& AudioFactoryPluginWindowAdapter::IsWindowAliveInSlate(ExistingWindow.ToSharedRef()))
		{
			if (const TSharedPtr<SAudioFactoryPluginSettingsFrameAdapter> ExistingFrame = Entry->SettingsFrame.Pin())
			{
				ExistingFrame->Retarget(MoveTemp(Spec));
				return ExistingFrame.ToSharedRef();
			}
		}

		AudioFactoryPluginWindowAdapter::GetLiveSettingsWindows().Remove(ProductId);
	}

	const TSharedRef<SAudioFactoryPluginSettingsFrameAdapter> SettingsFrame = SNew(SAudioFactoryPluginSettingsFrameAdapter)
		.Spec(MoveTemp(Spec))
		.OwnerWindow(OwnerWindow);

	AudioFactoryPluginWindowAdapter::FLiveSettingsWindowEntry& Entry =
		AudioFactoryPluginWindowAdapter::GetLiveSettingsWindows().FindOrAdd(ProductId);
	Entry.Window = OwnerWindow;
	Entry.SettingsFrame = SettingsFrame;
	return SettingsFrame;
}

void FAudioFactoryPluginWindowAdapter::ForceRevalidateForAutomation(const TSharedRef<SWidget>& SettingsFrame)
{
	// If the checked cast rejects the supplied widget, leave the automation request without
	// invoking production validation on the wrong object.
	if (const TSharedPtr<SAudioFactoryPluginSettingsFrameAdapter> TypedFrame = CheckedSettingsFrameCast(SettingsFrame))
	{
		TypedFrame->UpdateValidation();
	}
}

bool FAudioFactoryPluginWindowAdapter::ResetPropertyToDefaultForAutomation(
	const TSharedRef<SWidget>& SettingsFrame,
	const FName PropertyName)
{
	// If the checked cast rejects the supplied widget, do not invoke production reset behavior
	// on the wrong object.
	if (const TSharedPtr<SAudioFactoryPluginSettingsFrameAdapter> TypedFrame = CheckedSettingsFrameCast(SettingsFrame))
	{
		return TypedFrame->ResetPropertyToDefaultForAutomation(PropertyName);
	}

	return false;
}

bool FAudioFactoryPluginWindowAdapter::IsResetToDefaultVisibleByNameForTesting(
	const TSharedRef<SWidget>& SettingsFrame,
	const FName PropertyName)
{
	// Same checked-cast discipline as the neighboring automation seams: reject foreign widgets
	// instead of downcasting them, and report "not visible" rather than guessing.
	if (const TSharedPtr<SAudioFactoryPluginSettingsFrameAdapter> TypedFrame = CheckedSettingsFrameCast(SettingsFrame))
	{
		return TypedFrame->IsResetToDefaultVisibleByName(PropertyName);
	}

	return false;
}

#if WITH_DEV_AUTOMATION_TESTS
void FAudioFactoryPluginWindowAdapter::ResetActionFailureToastHooksForTesting()
{
	ActionFailureToastCountForTesting = 0;
	LastActionFailureToastTextForTesting.Reset();
	LastActionFailureToastItemForTesting.Reset();
}

int32 FAudioFactoryPluginWindowAdapter::GetActionFailureToastCountForTesting()
{
	return ActionFailureToastCountForTesting;
}

FString FAudioFactoryPluginWindowAdapter::GetLastActionFailureToastTextForTesting()
{
	return LastActionFailureToastTextForTesting;
}

TSharedPtr<SNotificationItem> FAudioFactoryPluginWindowAdapter::GetLastActionFailureToastItemForTesting()
{
	return LastActionFailureToastItemForTesting;
}

#endif

#undef LOCTEXT_NAMESPACE

} // namespace AudioExporter
#endif // WITH_EDITOR
