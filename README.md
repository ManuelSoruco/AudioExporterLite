<p align="center">
  <img src="Screenshots/AudioExporter.png" width="520" alt="Audio Exporter">
</p>

<h1 align="center">Audio Exporter</h1>

<p align="center">
  <strong>Batch export MetaSounds, SoundCues, and SoundWaves to .wav files with a single click.</strong><br>
  <em>By <a href="https://audiofactory.studio/">AudioFactory</a></em>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Engine-Unreal%205.6--5.8-blue" alt="UE 5.6-5.8">
  <img src="https://img.shields.io/badge/Platform-Win64%20validated-green" alt="Win64 validated">
  <img src="https://img.shields.io/badge/License-MIT-green" alt="MIT">
  <img src="https://img.shields.io/badge/Type-Editor%20Plugin-purple" alt="Editor Plugin">
</p>

---

## The Problem

Procedural audio in Unreal Engine - MetaSounds and SoundCues - is powerful but ephemeral. You can't simply right-click and "Save as WAV." Sound designers regularly need to bake out variations of procedural sounds for use in other projects, middleware tools, or for performance optimization on target platforms. Plain SoundWave assets need one-shot export too, without round-tripping through a DAW.

**Audio Exporter** solves this with a streamlined, one-click batch export workflow for all three asset types.

## Features

- **Batch Export** - Select any combination of MetaSoundSources, SoundCues, and SoundWaves in the Content Browser, right-click, and export them all at once
- **SoundWave Export** - Export plain SoundWave assets as static sources: one file per asset, no variation rendering
- **Variation Generation** - Automatically render multiple variations of each procedural asset (1-5 per asset) to capture the randomized behavior
- **One-Shot & Loop Modes** - Separate variation and length controls for one-shots and looping sounds
- **16-bit PCM WAV Export** - Writes standard 16-bit PCM WAV files
- **Normalization** - Optional peak normalization to -1 dBFS for all exported files
- **Real-time Monitoring** - Listen to the audio as it's being rendered
- **Clean Starts** - Exported files begin at the source's first frame. The engine's own start-up silence is removed (measured 0 frames on UE 5.6, 5.7, and 5.8); silence you authored inside a SoundWave, a MetaSound delay, or a SoundCue Delay node is content and is preserved
- **Export Logging** - Optional TXT-only report (`AudioExporterTextLog.txt`) saved at the session root for archival and troubleshooting. CSV reporting remains available in Audio Exporter Pro, Audio Converter, and Audio Normalizer (Lite writes text reports only)
- **Custom Export Path** - Choose where your WAVs land, or use the auto-timestamped default location. Exported WAVs are grouped in an `Audio` subfolder inside the session folder, next to the report

<p align="center">
  <img src="Screenshots/AudioExporter.png" width="480" alt="Audio Exporter Settings Window">
</p>

## Installation

```
YourProject/
    Plugins/
        AudioExporter/     <- Copy this folder here
```

1. Copy the `AudioExporter` folder into your project's `Plugins/` directory
2. Open your project - the plugin loads automatically
3. Select audio assets in the Content Browser -> right-click -> **Export Audio...**

### Requirements

- Unreal Engine **5.6** to **5.8**
- Win64 package validation evidence is current; Mac/Linux are not claimed until matching package gates exist.
- **MetaSounds** plugin (enabled by default)
- **AudioWidgets** plugin (enabled by default)

## The AudioFactory Plugin Family

This package ships only **Audio Exporter**. The sibling links below are separate AudioFactory products and are included here only as store cross-references.

| Plugin | Description |
|--------|-------------|
| **Audio Exporter** | Batch export MetaSounds, SoundCues, and SoundWaves to WAV |
| **[Audio Exporter Pro](https://www.fab.com/listings/3090ed2d-bda1-4635-b281-2fd29d17d935)** | Advanced export with quality controls, naming templates, and offline rendering |
| **[Audio Converter](https://www.fab.com/listings/4b08a0c5-3789-4320-98a2-a4d3267266af)** | Batch convert SoundWaves - downmix, resample, normalize, trim silence |
| **[Audio Normalizer](https://www.fab.com/listings/a10ec377-327d-4bec-a989-ed2eecadbe08)** | BS.1770-5-style loudness normalization for Unreal audio assets |

---

<p align="center">
  <br>
  <strong>Built by <a href="https://audiofactory.studio/">AudioFactory</a></strong><br>
  <em>R&D studio specializing in custom Unreal Engine audio tooling.</em><br>
  <br>
  <a href="https://audiofactory.studio/">audiofactory.studio</a> | <a href="https://audiofactory.studio/contact/">Support</a>
</p>
