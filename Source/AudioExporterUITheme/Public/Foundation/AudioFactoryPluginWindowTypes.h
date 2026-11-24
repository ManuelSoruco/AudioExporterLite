// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryStyle.h"
#include "Input/Reply.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"

#ifndef AF_UI_API
#define AF_UI_API
#endif

namespace AudioExporter {
enum class EAudioFactoryProductId : uint8
{
	AudioExporter,
	AudioExporterPro,
	AudioConverter,
	AudioNormalizer,
};

struct AF_UI_API FAudioFactoryProductIdentity
{
	EAudioFactoryProductId ProductId = EAudioFactoryProductId::AudioExporter;
	FText BrandName;
	FText ProductName;
	FLinearColor AccentColor = FLinearColor::White;
	FName GlyphName;
	FText PrimaryVerb;
};

struct AF_UI_API FAudioFactoryProductRegistry
{
	static const FAudioFactoryProductIdentity& Get(EAudioFactoryProductId ProductId)
	{
		static const FAudioFactoryProductIdentity AudioExporterIdentity{
			EAudioFactoryProductId::AudioExporter,
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioExporterBrandName", "AudioFactory"),
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioExporterProductName", "Audio Exporter"),
			// Source: Plugins/AudioExporter/Resources/Icon128.png (audited accent)
			FLinearColor(FColor(0xA1, 0x5E, 0x1E)),
			TEXT("AudioFactory.Glyph.Export"),
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioExporterPrimaryVerb", "Export")
		};

		static const FAudioFactoryProductIdentity AudioExporterProIdentity{
			EAudioFactoryProductId::AudioExporterPro,
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioExporterProBrandName", "AudioFactory"),
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioExporterProProductName", "Audio Exporter Pro"),
			// Source: Plugins/AudioExporterPro/Resources/Icon128.png (audited accent)
			FLinearColor(FColor(0xA8, 0x2F, 0x13)),
			TEXT("AudioFactory.Glyph.ExportPro"),
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioExporterProPrimaryVerb", "Export")
		};

		static const FAudioFactoryProductIdentity AudioConverterIdentity{
			EAudioFactoryProductId::AudioConverter,
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioConverterBrandName", "AudioFactory"),
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioConverterProductName", "Audio Converter"),
			// Source: Plugins/AudioConverter/Resources/Icon128.png (audited accent)
			FLinearColor(FColor(0x68, 0x3C, 0x9D)),
			TEXT("AudioFactory.Glyph.Convert"),
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioConverterPrimaryVerb", "Convert")
		};

		static const FAudioFactoryProductIdentity AudioNormalizerIdentity{
			EAudioFactoryProductId::AudioNormalizer,
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioNormalizerBrandName", "AudioFactory"),
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioNormalizerProductName", "Audio Normalizer"),
			// Source: Plugins/AudioNormalizer/Resources/Icon128.png (audited accent)
			FLinearColor(FColor(0x29, 0x6F, 0xA2)),
			TEXT("AudioFactory.Glyph.Normalize"),
			NSLOCTEXT("AudioFactoryPluginWindow", "AudioNormalizerPrimaryVerb", "Normalize")
		};

		switch (ProductId)
		{
		case EAudioFactoryProductId::AudioExporterPro:
			return AudioExporterProIdentity;
		case EAudioFactoryProductId::AudioConverter:
			return AudioConverterIdentity;
		case EAudioFactoryProductId::AudioNormalizer:
			return AudioNormalizerIdentity;
		case EAudioFactoryProductId::AudioExporter:
		default:
			return AudioExporterIdentity;
		}
	}

	static FString ResolveMarketplaceURL(const EAudioFactoryProductId ProductId)
	{
		const TCHAR* PluginName = TEXT("AudioExporter");
		switch (ProductId)
		{
		case EAudioFactoryProductId::AudioExporterPro:
			PluginName = TEXT("AudioExporterPro");
			break;
		case EAudioFactoryProductId::AudioConverter:
			PluginName = TEXT("AudioConverter");
			break;
		case EAudioFactoryProductId::AudioNormalizer:
			PluginName = TEXT("AudioNormalizer");
			break;
		case EAudioFactoryProductId::AudioExporter:
		default:
			break;
		}

		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName))
		{
			return Plugin->GetDescriptor().MarketplaceURL;
		}

		return FString();
	}
};

// Primary actions use Unreal's stock "PrimaryButton" treatment without a product-color
// override. This keeps the more polished native primary style while preserving the editor's
// standard color language.
struct AF_UI_API FAudioFactoryProductStyles
{
	static const FButtonStyle& GetPrimaryButtonStyle(const EAudioFactoryProductId ProductId)
	{
		static const TArray<FButtonStyle> Styles = BuildPrimaryButtonStyles();
		const int32 Index = static_cast<int32>(ProductId);
		return Styles.IsValidIndex(Index) ? Styles[Index] : Styles[0];
	}

private:
	static FButtonStyle BuildPrimaryButtonStyle(const EAudioFactoryProductId)
	{
		return FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton");
	}

	static TArray<FButtonStyle> BuildPrimaryButtonStyles()
	{
		// Index must line up with EAudioFactoryProductId's numeric values.
		TArray<FButtonStyle> Result;
		Result.Reserve(4);
		Result.Add(BuildPrimaryButtonStyle(EAudioFactoryProductId::AudioExporter));
		Result.Add(BuildPrimaryButtonStyle(EAudioFactoryProductId::AudioExporterPro));
		Result.Add(BuildPrimaryButtonStyle(EAudioFactoryProductId::AudioConverter));
		Result.Add(BuildPrimaryButtonStyle(EAudioFactoryProductId::AudioNormalizer));
		return Result;
	}
};

struct AF_UI_API FAudioFactoryClassIconNames
{
	static FName MetaSoundSource()
	{
		return TEXT("ClassIcon.MetasoundSource");
	}

	static FName SoundWave()
	{
		return TEXT("ClassIcon.SoundWave");
	}

	static FName SoundCue()
	{
		return TEXT("ClassIcon.SoundCue");
	}
};

struct AF_UI_API FAudioFactoryInlineCount
{
	static FAudioFactoryInlineCount Make(
		FText InLabel,
		TAttribute<FText> InValue,
		FText InToolTip = FText::GetEmpty(),
		TAttribute<bool> InIsEnabled = true)
	{
		FAudioFactoryInlineCount Count;
		Count.Label = MoveTemp(InLabel);
		Count.Value = MoveTemp(InValue);
		Count.ToolTip = MoveTemp(InToolTip);
		Count.IsEnabled = MoveTemp(InIsEnabled);
		return Count;
	}

	static FAudioFactoryInlineCount MakeWithIcon(
		FText InLabel,
		TAttribute<FText> InValue,
		FName InIconName,
		FText InToolTip = FText::GetEmpty(),
		TAttribute<bool> InIsEnabled = true)
	{
		FAudioFactoryInlineCount Count = Make(MoveTemp(InLabel), MoveTemp(InValue), MoveTemp(InToolTip));
		Count.IsEnabled = MoveTemp(InIsEnabled);
		Count.IconName = InIconName;
		return Count;
	}

	FText Label;
	TAttribute<FText> Value;
	FText ToolTip;
	FName IconName;
	TAttribute<bool> IsEnabled = true;
};

enum class EAudioFactoryAssetInclusionState : uint8
{
	Included,
	Excluded
};

struct AF_UI_API FAudioFactorySelectedAsset
{
	FAudioFactorySelectedAsset() = default;

	FAudioFactorySelectedAsset(
		FText InDisplayName,
		FText InAssetPath,
		FText InAssetType,
		EAudioFactoryAssetInclusionState InInclusionState,
		FText InReason,
		FName InIconName = NAME_None)
		: DisplayName(MoveTemp(InDisplayName))
		, AssetPath(MoveTemp(InAssetPath))
		, AssetClass(MoveTemp(InAssetType))
		, InclusionState(InInclusionState)
		, Reason(MoveTemp(InReason))
		, IconName(InIconName)
	{
	}

	static FAudioFactorySelectedAsset Make(
		FText InDisplayName,
		FText InAssetPath,
		FText InAssetType,
		EAudioFactoryAssetInclusionState InInclusionState = EAudioFactoryAssetInclusionState::Included,
		FText InReason = FText::GetEmpty(),
		FName InIconName = NAME_None)
	{
		return FAudioFactorySelectedAsset(
			MoveTemp(InDisplayName),
			MoveTemp(InAssetPath),
			MoveTemp(InAssetType),
			InInclusionState,
			MoveTemp(InReason),
			InIconName);
	}

	bool IsIncluded() const
	{
		return InclusionState == EAudioFactoryAssetInclusionState::Included;
	}

	FText DisplayName;
	FText AssetPath;
	FText AssetClass;
	EAudioFactoryAssetInclusionState InclusionState = EAudioFactoryAssetInclusionState::Included;
	FText Reason;
	FName IconName = NAME_None;
};

struct AF_UI_API FAudioFactoryPathContext
{
	FText Label;
	TAttribute<FText> Path;
	FText ToolTip;
	TFunction<FReply()> OnAction;
	TAttribute<bool> IsResetToDefaultVisible;
	TFunction<FReply()> OnResetToDefault;
};

enum class EAudioFactoryStatusSeverity : uint8
{
	Neutral,
	Ready,
	Warning,
	Error,
};

struct AF_UI_API FAudioFactoryStatus
{
	FAudioFactoryStatus() = default;
	FAudioFactoryStatus(EAudioFactoryStatusSeverity InSeverity, FText InMessage, FText InToolTip = FText::GetEmpty())
		: Severity(InSeverity)
		, Message(MoveTemp(InMessage))
		, ToolTip(MoveTemp(InToolTip))
	{
	}

	EAudioFactoryStatusSeverity Severity = EAudioFactoryStatusSeverity::Neutral;
	FText Message;
	FText ToolTip;
};

struct AF_UI_API FAudioFactoryFooterAction
{
	FAudioFactoryFooterAction() = default;
	FAudioFactoryFooterAction(
		FName InIconName,
		FText InToolTip,
		TFunction<FReply()> InOnClicked = TFunction<FReply()>(),
		TAttribute<bool> InIsEnabled = true)
		: IconName(InIconName)
		, ToolTip(MoveTemp(InToolTip))
		, IsEnabled(MoveTemp(InIsEnabled))
		, OnClicked(MoveTemp(InOnClicked))
	{
	}

	static FAudioFactoryFooterAction MakeLabeled(
		FName InIconName,
		FText InLabel,
		FText InToolTip,
		TFunction<FReply()> InOnClicked = TFunction<FReply()>(),
		TAttribute<bool> InIsEnabled = true)
	{
		FAudioFactoryFooterAction Action(InIconName, MoveTemp(InToolTip), MoveTemp(InOnClicked), MoveTemp(InIsEnabled));
		Action.Label = MoveTemp(InLabel);
		Action.bUseLabel = true;
		return Action;
	}

	static FAudioFactoryFooterAction MakeShowSelected(
		TFunction<FReply()> InOnClicked = TFunction<FReply()>(),
		TAttribute<bool> InIsEnabled = true)
	{
		return MakeLabeled(
			TEXT("AudioFactory.Glyph.SelectAll"),
			NSLOCTEXT("AudioFactoryPluginWindow", "ShowSelectedAction", "Show Selected"),
			NSLOCTEXT("AudioFactoryPluginWindow", "ShowSelectedActionTooltip", "Show the selected assets."),
			MoveTemp(InOnClicked),
			MoveTemp(InIsEnabled));
	}

	FName IconName;
	FText Label;
	FText ToolTip;
	bool bIsEnabled = true;
	bool bUseLabel = false;
	TAttribute<bool> IsEnabled = true;
	TFunction<FReply()> OnClicked;
};

struct AF_UI_API FAudioFactoryPrimaryAction
{
	FText Label;
	FText ToolTip;
	bool bIsEnabled = true;
	TAttribute<bool> IsEnabled = true;
	TFunction<FReply()> OnClicked;
};
} // namespace AudioExporter
