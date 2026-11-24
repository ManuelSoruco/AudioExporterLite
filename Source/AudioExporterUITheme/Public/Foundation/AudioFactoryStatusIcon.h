// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Foundation/AudioFactoryPluginWindowTypes.h"
#include "Foundation/AudioFactoryStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/ISlateStyle.h"

#ifndef AF_UI_API
#define AF_UI_API
#endif

/**
 * Shared severity -> engine-icon and severity -> token-color resolution for the
 * frame's status strip, the task-log view, and the progress body.
 * Single owner of the mapping so every surface renders the same 14x14
 * "Icons.*WithColor" engine icon family and the same token colors.
 */
namespace AudioExporter {
struct AF_UI_API FAudioFactoryStatusIcon
{
	struct FStatusIconResolution
	{
		const FSlateBrush* Brush = nullptr;
		// True when we fell back to a plain (uncolored) icon and need to tint it
		// ourselves; false when the resolved brush is a "WithColor" variant that
		// is already baked-in tinted.
		bool bNeedsTint = false;
	};

	static FStatusIconResolution Resolve(const EAudioFactoryStatusSeverity Severity)
	{
		FName PrimaryName;
		FName FallbackName;
		switch (Severity)
		{
		case EAudioFactoryStatusSeverity::Error:
			PrimaryName = TEXT("Icons.ErrorWithColor");
			FallbackName = TEXT("Icons.Error");
			break;
		case EAudioFactoryStatusSeverity::Warning:
			PrimaryName = TEXT("Icons.WarningWithColor");
			FallbackName = TEXT("Icons.Warning");
			break;
		case EAudioFactoryStatusSeverity::Ready:
			PrimaryName = TEXT("Icons.SuccessWithColor");
			FallbackName = TEXT("Icons.Check");
			break;
		case EAudioFactoryStatusSeverity::Neutral:
		default:
			PrimaryName = TEXT("Icons.InfoWithColor");
			FallbackName = TEXT("Icons.Info");
			break;
		}

		FStatusIconResolution Resolution;
		if (const FSlateBrush* PrimaryBrush = FAppStyle::GetOptionalBrush(PrimaryName, nullptr, nullptr))
		{
			Resolution.Brush = PrimaryBrush;
			Resolution.bNeedsTint = false;
			return Resolution;
		}

		Resolution.Brush = FAppStyle::GetOptionalBrush(FallbackName, nullptr, FStyleDefaults::GetNoBrush());
		Resolution.bNeedsTint = true;
		return Resolution;
	}

	static FLinearColor SeverityColor(
		const FAudioFactoryPluginWindowTokens& Tokens,
		const EAudioFactoryStatusSeverity Severity)
	{
		switch (Severity)
		{
		case EAudioFactoryStatusSeverity::Error:   return Tokens.StatusError;
		case EAudioFactoryStatusSeverity::Warning: return Tokens.StatusWarning;
		case EAudioFactoryStatusSeverity::Ready:   return Tokens.StatusReady;
		default:                                   return Tokens.StatusNeutral;
		}
	}
};
} // namespace AudioExporter
