// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"

/**
 * Process-global refcounted lease registry for the editor "Allow Background Audio" override that
 * overlapping export sessions must hold together.
 *
 * Why this exists: independent snapshot/restore pairs do not compose. Under an
 * L-start, P-start, L-finish, P-finish overlap the second exporter snapshots the FIRST exporter's
 * forced override as if it were the user's original value, so the final restore corrupts the
 * shared setting. The registry gives every exporter ONE owner for the custody chain:
 * the first acquirer records the true original value, later acquirers only increment the hold
 * count, and the value is written back exactly once -- by the last releaser.
 *
 * Cross-module access: modules in this plugin link it directly. Other plugins can reach the same
 * registry WITHOUT a build dependency through the single versioned extern "C" entry point below
 * (resolve it at runtime with FModuleManager + GetDllExport). Keep the v1 ABI stable;
 * AudioExportLease_GetApi returns nullptr for unknown versions so a mismatching caller can fall
 * back to its own snapshot/restore instead of corrupting state.
 *
 * Deliberately engine-agnostic: pure value/state coordination only. Reading and writing
 * ULevelEditorMiscSettings stays in each manager, which already owns those includes and their
 * shutdown-safety guards.
 */

/** v1 ABI surface handed out by AudioExportLease_GetApi. Function order is part of the contract. */
struct FAudioExportLeaseApiV1
{
	/**
	 * Acquires one hold on the background-audio override.
	 * @param bOriginalValue - The caller's reading of the setting at acquire time. Recorded ONLY
	 *        when this call creates the lease (hold count 0 -> 1); ignored otherwise, because a
	 *        later acquirer's reading is just the first holder's forced override.
	 * @return True when this call became the first holder (its value was recorded).
	 */
	bool (*AcquireBackgroundAudio)(const bool bOriginalValue);

	/**
	 * Releases one hold.
	 * @param bOutOriginalValue - Receives the recorded pre-lease setting value.
	 * @return True when this was the LAST hold and bOutOriginalValue carries the value to restore.
	 *         False while other holders remain (the forced override stays active), or when no
	 *         lease was held (misuse; logged).
	 */
	bool (*ReleaseBackgroundAudio)(bool& bOutOriginalValue);
};

extern "C"
{
	/**
	 * Versioned ABI anchor for cross-module consumers without a build dependency.
	 * @param ApiVersion - Requested ABI version (currently 1).
	 * @return Pointer to a process-lifetime FAudioExportLeaseApiV1, or nullptr for an unsupported
	 *         version. Never unloaded; safe to cache the returned pointer.
	 */
	AUDIOEXPORTERRUNTIME_API void* AudioExportLease_GetApi(const int32 ApiVersion);
}

/**
 * Direct link-time wrappers over the same registry (used by this plugin's own editor manager).
 * Semantics identical to the matching members of FAudioExportLeaseApiV1.
 */
AUDIOEXPORTERRUNTIME_API bool AudioExportLease_AcquireBackgroundAudio(const bool bOriginalValue);
AUDIOEXPORTERRUNTIME_API bool AudioExportLease_ReleaseBackgroundAudio(bool& bOutOriginalValue);
