// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

/**
 * AudioFactoryAudioUI.h -- Full umbrella include for AudioFactory shared UI library.
 *
 * Includes ALL THREE LAYERS:
 *   LAYER 0: Foundation  (palette, brushes, constants, base widgets)
 *   LAYER 1: Controls    (button, dropdown, toggle, sections, icon button)
 *   LAYER 2: Audio/DSP   (knob styles, meter strip, GR meter, value formatters)
 *
 * For plugins that do NOT need audio/DSP widgets (no AudioWidgets dependency),
 * include "AudioFactoryUI.h" instead -- it pulls in Layer 0 + Layer 1 only.
 *
 * USAGE -- call once in StartupModule() before any widget is created:
 * @code
 *   FAudioFactoryTheme::SetTheme(EAudioFactoryTheme::Dark);
 * @endcode
 *
 * Build.cs integration (generated alias module, source-only, no module dependency):
 * @code
 *   // Each product compiles this UITheme as its own module
 *   // (Plugins/<Product>/Source/<Product>UITheme).
 *   PrivateDefinitions.Add("AF_UI_API=YOURMODULE_API");
 *   // Layer 2 requires:
 *   PrivateDependencyModuleNames.Add("AudioWidgets");
 * @endcode
 */

// -- Layer 0 + Layer 1 (via AudioFactoryUI.h) ----------------------------------
#include "AudioFactoryUI.h"

// -- Layer 2: Audio/DSP --------------------------------------------------------

#include "Audio/AudioFactoryAudioTypes.h"
#include "Audio/AudioFactoryAudioTheme.h"
#include "Audio/IAudioFactoryMeterSource.h"
#include "Audio/Widgets/SAudioFactoryKnob.h"
#include "Audio/Widgets/SAudioFactoryLabeledKnob.h"
#include "Audio/Widgets/SAudioFactoryMeterStrip.h"
#include "Audio/Widgets/SAudioFactoryGainReductionMeter.h"

