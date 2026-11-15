# Trackspin OBS plugin

Source code of `trackspin.dll`, the OBS Studio plugin of [Trackspin](https://trackspin.app): a "now playing" card
for streams. Licensed under the GNU General Public License, version 2 or later (see [LICENSE](LICENSE)).

## What is in here

- `src/` — the plugin: the "Trackspin" source type in OBS, its settings, and the code that starts the Trackspin
  agent and passes the settings to it.
- `data/` — locale files and icons used by the plugin.
- `cmake/`, `CMakeLists.txt`, `CMakePresets.json`, `buildspec.json`, `.github/` — build files based on the
  [OBS plugin template](https://github.com/obsproject/obs-plugintemplate).

The plugin works together with the Trackspin agent (`trackspin-agent.exe`), a separate program that reads what is
playing and serves the card on `127.0.0.1`. The plugin starts it as a separate process and talks to it over local
HTTP; it doesn't link to it. The agent is not part of this repository and is distributed under its own terms with
Trackspin from the Microsoft Store and from trackspin.app.

## Build (Windows x64)

Requirements: Visual Studio 2022 with the C++ workload, CMake 3.28+, PowerShell 7.

```powershell
.github/scripts/Build-Windows.ps1 -Target x64 -Configuration RelWithDebInfo
.github/scripts/Package-Windows.ps1 -Target x64 -Configuration RelWithDebInfo
```

The build downloads the OBS Studio sources and prebuilt dependencies listed in `buildspec.json`. The result is in
`release/RelWithDebInfo/trackspin/`.

## Versions

Each release of Trackspin is tagged here with the same version as the plugin it ships (`buildspec.json`).

## Contact

support@trackspin.app
