// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWindow.h"

/**
 * IAudioFactoryWindowHost
 *
 * Abstract interface for widgets that need to resize their parent window
 * (collapsible sections, advanced panels, etc.).
 *
 * Utility plugins implement it in their window class, or pass nullptr
 * if they do not need auto-resize on section expand/collapse.
 */
namespace AudioExporter {
class IAudioFactoryWindowHost
{
public:
	virtual ~IAudioFactoryWindowHost() = default;

	/**
	 * Resize the host window by the given pixel delta.
	 * Positive values make the window taller; negative values shrink it.
	 * @param DeltaHeight  Pixel delta to apply to window height.
	 */
	virtual void ResizeWindowByDelta(float DeltaHeight) = 0;

	/**
	 * Returns the host SWindow, or null if the window is not available.
	 * Used by widgets that need to query current window size.
	 */
	virtual TSharedPtr<SWindow> GetHostWindow() const = 0;
};
} // namespace AudioExporter

