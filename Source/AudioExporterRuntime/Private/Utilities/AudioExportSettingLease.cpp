// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Utilities/AudioExportSettingLease.h"

#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogAudioExportLease, Log, All);

namespace
{
	// Process-lifetime custody state for the shared editor background-audio override. Every export
	// session (either product) holds one count; OriginalValue is the user's true pre-lease setting
	// recorded by whoever created the lease, and it is handed back exactly once -- to the last
	// releaser -- so overlapping sessions can never restore each other's forced override.
	FCriticalSection LeaseCriticalSection;
	int32 HoldCount = 0;
	bool bHasRecordedOriginal = false;
	bool RecordedOriginalValue = false;
}

bool AudioExportLease_AcquireBackgroundAudio(const bool bOriginalValue)
{
	FScopeLock Lock(&LeaseCriticalSection);
	++HoldCount;
	const bool bBecameFirstHolder = HoldCount == 1;
	if (bBecameFirstHolder)
	{
		bHasRecordedOriginal = true;
		RecordedOriginalValue = bOriginalValue;
	}
	return bBecameFirstHolder;
}

bool AudioExportLease_ReleaseBackgroundAudio(bool& bOutOriginalValue)
{
	FScopeLock Lock(&LeaseCriticalSection);
	if (HoldCount <= 0 || !bHasRecordedOriginal)
	{
		UE_LOG(LogAudioExportLease, Warning, TEXT("%s: Release requested without a held background-audio lease."), *FString(__FUNCTION__));
		return false;
	}

	--HoldCount;
	if (HoldCount > 0)
	{
		// Other sessions still hold the override active; the original stays in custody until the
		// last holder releases.
		return false;
	}

	bOutOriginalValue = RecordedOriginalValue;
	HoldCount = 0;
	bHasRecordedOriginal = false;
	RecordedOriginalValue = false;
	return true;
}

void* AudioExportLease_GetApi(const int32 ApiVersion)
{
	if (ApiVersion != 1)
	{
		return nullptr;
	}

	static FAudioExportLeaseApiV1 Api;
	Api.AcquireBackgroundAudio = &AudioExportLease_AcquireBackgroundAudio;
	Api.ReleaseBackgroundAudio = &AudioExportLease_ReleaseBackgroundAudio;
	return &Api;
}
