// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Assets/SoundVolumeUtils.h"

#include "CoreGlobals.h"
#include "Misc/App.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
namespace AudioExporter {

DEFINE_LOG_CATEGORY_STATIC(LogAFSoundVolumeUtils, Log, All);

TOptional<float> SoundVolumeUtils::GetSoundVolume(const USoundBase* InSound)
{
	// A null sound has no volume measurement; unset distinguishes unavailable from genuine unity volume.
	if (!InSound)
	{
		return TOptional<float>();
	}

	if (const USoundCue* SoundCue = Cast<USoundCue>(InSound))
	{
		return SoundCue->VolumeMultiplier;
	}

	if (const USoundWave* SoundWave = Cast<USoundWave>(InSound))
	{
		return SoundWave->Volume;
	}

	// Unsupported sound types have no volume value exposed by this utility; do not fabricate unity.
	return TOptional<float>();
}

bool SoundVolumeUtils::SetSoundVolume(USoundBase* InSound, float InNewVolume)
{
	if (!InSound)
	{
		return false;
	}

	// A non-finite volume is a hard rejection before any property mutation; the original asset value remains unchanged.
	if (!FMath::IsFinite(InNewVolume))
	{
		UE_LOG(LogAFSoundVolumeUtils, Error, TEXT("SetSoundVolume: rejected non-finite volume for '%s'."), *InSound->GetPathName());
		return false;
	}

	if (InNewVolume < 0.0f)
	{
		UE_LOG(LogAFSoundVolumeUtils, Error, TEXT("SetSoundVolume: rejected negative linear volume %.6f for '%s'."), InNewVolume, *InSound->GetPathName());
		return false;
	}

	if (USoundCue* SoundCue = Cast<USoundCue>(InSound))
	{
		SoundCue->VolumeMultiplier = InNewVolume;
		return true;
	}

	if (USoundWave* SoundWave = Cast<USoundWave>(InSound))
	{
		SoundWave->Volume = InNewVolume;
		return true;
	}

	return false;
}

bool SoundVolumeUtils::ShouldPostEditChangeForVolumeEdit(const USoundBase* InSound)
{
#if WITH_EDITOR
	if (!InSound)
	{
		return false;
	}

	// USoundWave::PostEditChange rebuilds encoded audio. Headless automation only needs
	// the volume property mutation, and codec modules such as BINKA may be unavailable.
	const bool bHeadlessAutomation = IsRunningCommandlet() || FApp::IsUnattended() || !FApp::CanEverRender();
	if (bHeadlessAutomation && InSound->IsA<USoundWave>())
	{
		return false;
	}

	return true;
#else
	return false;
#endif
}

void SoundVolumeUtils::NotifySoundVolumeEdited(USoundBase* InSound, bool bMarkPackageDirty)
{
	if (!InSound)
	{
		return;
	}

#if WITH_EDITOR
	if (ShouldPostEditChangeForVolumeEdit(InSound))
	{
		InSound->PostEditChange();
	}
#endif

	if (bMarkPackageDirty)
	{
		(void)InSound->MarkPackageDirty();
	}
}
} // namespace AudioExporter
