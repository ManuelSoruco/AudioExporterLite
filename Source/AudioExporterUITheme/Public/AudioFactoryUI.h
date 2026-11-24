// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

/**
 * AudioFactoryUI.h -- Umbrella include for AudioFactory shared UI library (Layer 0 + Layer 1).
 *
 * For plugins that also need audio/DSP widgets (knobs, meters), include
 * "AudioFactoryAudioUI.h" instead -- it adds Layer 2 on top of this.
 *
 * LAYER 0: Foundation
 *   - FAudioFactoryPalette     (struct -- theme color token set)
 *   - EAudioFactoryTheme       (enum -- Dark catalog)
 *   - FAudioFactoryTheme       (static class -- palette, color getters, utilities)
 *   - FAudioFactoryBrushes     (inline brush factory namespace -- 25+ brushes)
 *   - FAudioFactoryConstants   (spacing, radii, font-size constants)
 *   - FAudioFactoryPluginWindowTypes (v2.0 product/window data contract)
 *
 * LAYER 0 Widgets:
 *   - SAudioFactoryGradientPanel
 *
 * LAYER 1: Window chrome (v2.0 native frame)
 *   - SAudioFactoryPluginWindowFrame  (native frame; strip/footer widgets below)
 *   - SAudioFactoryStatusStrip        (status/validation strip, extracted from the frame)
 *   - SAudioFactoryTaskLogView        (native SListView log, engine status icons)
 *   - SAudioFactoryProgressBody       (headline + 4px progress bar + task log)
 *   - SAudioFactorySelectedAssetsWindow
 *
 * LAYER 1: Interactive Controls
 *   - IAudioFactoryWindowHost    (resize interface for collapsible sections)
 *   - SAudioFactoryButton        (rounded toggle / radio button strip)
 *   - SAudioFactoryDropdown      (fully-themed dropdown on SMenuAnchor)
 *   - SAudioFactoryToggle        (styled toggle switch)
 *   - SAudioFactorySliderStyle   (FSliderStyle factory, header-only)
 *   - SAudioFactoryCollapsibleSection
 *   - SAudioFactoryActionBar
 *   - SAudioFactoryParamRow
 *   - SAudioFactoryPathPicker
 *
 * LAYER 1: Data-Driven Panel (settings dialogs)
 *   - EAFParamControl            (enum -- control type discriminator)
 *   - FAudioFactoryParamSpec     (struct -- single param row descriptor + static builders)
 *   - FAudioFactorySectionSpec   (struct -- collapsible section with param list)
 *   - SAudioFactoryDetailsPanel  (widget -- renders param specs, layout-consistent across plugins)
 *
 * Build.cs integration (generated alias module, source-only, no module dependency):
 * @code
 *   // Each product compiles this UITheme as its own module
 *   // (Plugins/<Product>/Source/<Product>UITheme).
 *   PrivateDefinitions.Add("AF_UI_API=YOURMODULE_API");
 * @endcode
 *
 * USAGE -- call once in StartupModule() before any window is created:
 * @code
 *   FAudioFactoryTheme::SetTheme(EAudioFactoryTheme::Dark);
 * @endcode
 */

// -- Layer 0: Foundation -------------------------------------------------------
#include "Foundation/AudioFactoryPalette.h"
#include "Foundation/AudioFactoryConstants.h"
#include "Foundation/AudioFactoryStyle.h"
#include "Foundation/AudioFactoryPluginWindowTypes.h"
#include "Foundation/AudioFactoryTheme.h"         // also pulls in AudioFactoryBrushes.h

// -- Layer 0: Foundation Widgets -----------------------------------------------
#include "Foundation/Widgets/SAudioFactoryGradientPanel.h"

// -- Layer 1: Controls ---------------------------------------------------------
#include "Controls/IAudioFactoryWindowHost.h"
#include "Controls/SAudioFactoryButton.h"
#include "Controls/SAudioFactoryDropdown.h"
#include "Controls/SAudioFactoryToggle.h"
#include "Controls/SAudioFactorySliderStyle.h"
#include "Controls/SAudioFactoryCollapsibleSection.h"
#include "Controls/SAudioFactoryActionBar.h"
#include "Controls/SAudioFactoryParamRow.h"
#include "Controls/SAudioFactoryPathPicker.h"
#include "Controls/SAudioFactoryStatusStrip.h"
#include "Controls/SAudioFactoryTaskLogView.h"
#include "Controls/SAudioFactoryProgressBody.h"
#include "Controls/SAudioFactoryPluginWindowFrame.h"
#include "Controls/SAudioFactorySelectedAssetsWindow.h"

// -- Layer 1: Data-Driven Panel --------------------------------------------
#include "Controls/AudioFactoryParamSpec.h"
#include "Controls/SAudioFactoryDetailsPanel.h"
