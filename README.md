# NV3D-Glass

Capture a side-by-side stereo source (game, video player, Katanga producer) and
re-present it as true frame-sequential **NVIDIA 3D Vision** output via
[NV3DLib](https://github.com/oneup03/NV3D-Lib). No DLL injection into the
captured app; runs as a standalone .exe with an ImGui control panel.

## Requirements

- Windows 10 1903+ / Windows 11 (WGC capture)
- NVIDIA GeForce GPU with the legacy 3D Vision driver installed (425.31 or
  one of the community-extended builds for newer cards)
- A 3D Vision-certified display (120 Hz panel + IR emitter, or HDMI 1.4
  frame-packed display) configured as a 3D Vision target in NVCP
- "Set up stereoscopic 3D" enabled in the NVIDIA Control Panel
- For source: any windowed app that already produces side-by-side stereo
  (one frame split into left half / right half), or a
  [Katanga](https://github.com/bo3b/Katanga) / Geo-11 producer

## Install

Download the latest pre-release from the
[Releases](../../releases) page, unzip, and run `NV3D-Glass.exe`. The
binary is self-contained (statically-linked CRT) — no installer.

## Quick start

1. Launch `NV3D-Glass.exe`. The control panel opens.
2. **Capture Source** — pick `Window`, choose the SbS-producing app from
   the dropdown. Or pick `Monitor` to capture a whole display, or `Katanga`
   to receive from a Geo-11/Katanga producer over the shared-memory channel.
3. **3D Vision Output** — leave `(auto)` to let NV3DLib pick the
   3D-Vision-certified display, or pick one explicitly.
4. Click **Start**. The control panel minimises and a click-through FSE
   popup appears on the 3D Vision display. Put on the shutter glasses;
   you should see stereo separation.
5. Click the taskbar icon (or press **Ctrl+F8**) to bring the control
   panel back. Click **Stop** to tear down the 3D output cleanly.

## Hotkeys

| Hotkey   | Action                                                      |
|----------|-------------------------------------------------------------|
| Ctrl+F8  | Show / hide the 3D Vision FSE window (also kills/spawns the focus watcher) |
| Ctrl+F9  | Toggle eye-swap (reverses left/right; ~300 ms FSE blink while the presenter restarts) |

Both are global — they fire even when the captured game has foreground.

## Tray menu

Right-click the system tray icon for: Show/Hide panel, Start/Stop, Quit.
Double-click toggles panel visibility. Closing the panel with the **X**
button quits the app (and tears down the FSE popup cleanly).

## Settings & log

- `NV3D-Glass.ini` (next to the .exe) — picked source, output monitor,
  eye-swap, LightBoost / Driver Fix toggles, hotkey bindings, panel
  geometry.
- `NV3D-Glass.log` (next to the .exe) — all `[NV3D]` messages from the
  presenter + suppressor + LightBoost subsystems, plus app-level
  start/stop/shutdown checkpoints. First file to attach when reporting
  an issue.

## Notes

- **LightBoost** (control panel toggle): applies the custom display
  timing from NV3DLib's embedded nvtimings DB for low-persistence
  shutter-friendly backlight strobing. Disabled if your panel isn't
  in the DB or already matches.
- **3DVision Driver Fix** (control panel toggle): in-process hooks that
  suppress the legacy 3D Vision driver's OSD warnings, "press Ctrl+T"
  prompts, and the rating/info overlay. Turn off if you suspect the
  hooks are interacting badly with another tool.
- **Click-through FSE popup**: the 3D output window is `WS_EX_LAYERED |
  WS_EX_TRANSPARENT` — mouse clicks pass through to whatever is
  underneath, so you can interact with the captured game without alt-tab.
- **Monitor capture caveat**: if you pick the *same* monitor as both
  source and output, the app refuses to start (would feed back into
  itself). Use Window capture instead, or pick a different display.

## Building from source

Requires Visual Studio 2022 with the **Desktop development with C++**
workload (v143 toolset, Windows 10 SDK).

```powershell
git clone --recurse-submodules https://github.com/oneup03/NV3D-Glass.git
cd NV3D-Glass
.\external\NV3D-Lib\tools\build.ps1 -Configuration Release-MT -Platform x64
msbuild NV3D-Glass.sln /p:Configuration=Release-MT /p:Platform=x64
```

The vcxproj's PreBuildEvent runs the NV3DLib build for you, so the
second step alone is normally enough. Output is at
`bin\x64\Release-MT\NV3D-Glass.exe`.

**In VS Code**: open the folder, then **Ctrl+Shift+B** runs the default
build task (Release-MT|x64). **F5** launches the built .exe under the
Visual Studio debugger.

## Project structure

```
NV3D-Glass/
  src/                  ImGui control panel + WGC/DXGI/Katanga capture front-end
  external/
    NV3D-Lib/           Git submodule: D3D9Ex + NvAPI 3D Vision presenter
    imgui/              Git submodule: Dear ImGui (control panel UI)
  .github/workflows/    CI: builds Release-MT|x64, publishes a "latest" pre-release
  .vscode/              Build + debug tasks
```

## License

LGPL-2.1-or-later. See SPDX headers on individual source files and the
`LICENSE` file at the repo root.

## Acknowledgments

- [NV3DLib](https://github.com/oneup03/NV3D-Lib) — the 3D Vision presenter
- [VRto3D](https://github.com/oneup03/VRto3D) — reference for the FSE
  D3D9Ex + click-through + focus-management patterns
- [SR-Loom](https://github.com/effcol/SR-Loom) — reference structure for
  the standalone-app shell + WGC perf patterns
- [Katanga](https://github.com/bo3b/Katanga) / [Geo-11](https://github.com/bo3b/3Dmigoto-DarkStarSword-Branch) —
  upstream of the shared-memory producer protocol we read in Katanga mode
- [Dear ImGui](https://github.com/ocornut/imgui) — control panel UI
