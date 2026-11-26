<p align="center">
  <img src="https://github.com/ManuelSoruco/AudioExporterLite/releases/download/v2.0.0/AudioExporter_Banner.png" width="720" alt="Audio Exporter">
</p>

<h1 align="center">Audio Exporter</h1>

<p align="center">
  <strong>Bounce your procedural and linear Unreal audio to WAV without leaving the editor.</strong><br>
  <em>By <a href="https://audiofactory.studio/">AudioFactory</a>. Open source under the MIT License.</em>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Engine-Unreal%205.6%2B-blue" alt="Unreal Engine 5.6+">
  <img src="https://img.shields.io/badge/License-MIT-yellow" alt="MIT">
  <img src="https://img.shields.io/badge/Type-Editor%20Plugin-purple" alt="Editor plugin">
  <img src="https://img.shields.io/badge/Exports-3%20Asset%20Types-green" alt="3 asset types">
</p>

---

This repository is version 2.0.0 of the plugin, released as source code. It targets **Unreal Engine 5.6+**.

## The problem

MetaSounds and Sound Cues are procedural: they play differently every time, and Unreal has no "Save as WAV". Sound designers still need those sounds as files, to use in other projects, in middleware or in a DAW. Plain Sound Waves need the same one-click export.

## The solution

Audio Exporter renders a whole selection of MetaSounds, Sound Cues and Sound Waves to WAV files in one pass:

- **Batch export** from the Content Browser: right-click, **Export Audio...**
- **Variations** of each procedural sound, 1 to 5, with separate settings for one-shots and loops
- **32-bit float rendering**, written to 16-bit 48 kHz WAV
- **Clean starts**: every file begins on the source's first frame, and silence you authored is kept
- **Optional peak normalization** to -1 dBFS

<p align="center">
  <img src="https://github.com/ManuelSoruco/AudioExporterLite/releases/download/v2.0.0/AudioExporter.png" width="480" alt="Audio Exporter settings window">
</p>

## What it exports

**3 asset types:** MetaSound Sources, Sound Cues and Sound Waves. Sound Waves export as they are, one file per asset.

**Output:** 16-bit PCM WAV at 48 kHz.

## Workflow

- **Listen while exporting.** The audio plays as it renders.
- **Session folders.** Each run lands in a timestamped folder under `Saved/AudioExporter/`, or under an export root you choose. The WAV files go in its `Audio` subfolder.
- **Export log.** An optional text report is saved next to the audio.
- **Cancel at any point** and keep the files already written.
- **Native Unreal window.** Enter runs the export, Escape cancels.
- **Blueprint API**, synchronous and asynchronous.

## Installation

### From Fab (recommended)

Get **[Audio Exporter on Fab](https://www.fab.com/listings/47dd1516-1b1c-413e-83d8-4cc03dcf805a)**, add it to your library, and install it to your engine from the Epic Games Launcher. Then enable it in your project under **Edit > Plugins**.

### Alternative: from source

This repository is the same plugin as source code. Unreal builds it on your machine.

1. Copy this repository into your project as `Plugins/AudioExporter/` (so that `AudioExporter.uplugin` sits directly inside that folder).
2. Open the project. When Unreal offers to build the plugin, accept.
3. Select audio assets in the Content Browser, right-click and choose **Export Audio...**

```
YourProject/
`-- Plugins/
    `-- AudioExporter/
        |-- AudioExporter.uplugin
        `-- Source/
```

### Requirements

- Unreal Engine **5.6+**
- The C++ toolchain Unreal requires for your platform
- The **MetaSounds** and **AudioWidgets** plugins (enabled by default in the engine)

Built and tested on Windows. The plugin is an editor tool; nothing runs in a packaged game.

## Project structure

```
AudioExporter/
|-- AudioExporter.uplugin
|-- Config/
`-- Source/
    |-- AudioExporter/          # Editor module: Content Browser menu entry
    |-- AudioExporterUI/        # Settings and progress windows
    |-- AudioExporterUITheme/   # Shared look of the windows
    |-- AudioExporterRuntime/   # Rendering, variations and WAV writing
    |-- AudioExporterUtils/     # PCM reading and audio helpers
    `-- AudioExporterTests/     # Automation tests
```

## Tests

The `AudioExporterTests` module holds the automation tests, under `AudioFactory.AudioExporter`. It loads only when Unreal starts an automation run, so it never shows up in normal use. Some tests need sample sounds that are not in this repository and fail without them.

## Make it yours

This plugin is shared as is. We do not review pull requests or issues. Fork it and change it however you like: the MIT License lets you use it in your own projects, commercial ones included.

## The AudioFactory plugin family

| Plugin | What it does |
|--------|--------------|
| **[Audio Exporter](https://www.fab.com/listings/47dd1516-1b1c-413e-83d8-4cc03dcf805a)** | Batch export MetaSounds, Sound Cues and Sound Waves to WAV. Open source. |
| **[Audio Effects Studio](https://github.com/ManuelSoruco/AudioEffectsStudio)** | Visual editors for every built-in Unreal audio effect. Open source. |
| **[Audio Exporter Pro](https://www.fab.com/listings/3090ed2d-bda1-4635-b281-2fd29d17d935)** | 24-bit and 32-bit float, more sample rates, loop crossfades, naming templates and offline rendering |
| **[Audio Converter](https://www.fab.com/listings/4b08a0c5-3789-4320-98a2-a4d3267266af)** | Batch convert Sound Waves: downmix, resample, normalize, trim silence |
| **[Audio Normalizer](https://www.fab.com/listings/a10ec377-327d-4bec-a989-ed2eecadbe08)** | Loudness normalization for Unreal audio assets |

## Version history

- **2.0 (September 2026).** Rebuilt fully native in C++ and Slate. 32-bit float rendering engine. Sound Wave export. Optional peak normalization. Engine start-up silence removed from every file. Text export log. Supported on Unreal Engine 5.6, 5.7 and 5.8.
- **1.0.** First release on Fab.

## License

MIT License. See [LICENSE](LICENSE).

---

<p align="center">
  <strong>Built by <a href="https://audiofactory.studio/">AudioFactory</a></strong><br>
  <em>Tech-Audio R&amp;D Lab</em><br>
  <a href="https://audiofactory.studio/">audiofactory.studio</a> | <a href="https://audiofactory.studio/contact/">Support</a>
</p>
