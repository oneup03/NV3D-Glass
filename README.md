# NV3D-Glass

Capture a side-by-side stereo source (game, video player, Katanga producer) and
re-present it as true frame-sequential **NVIDIA 3D Vision** output via
[NV3DLib](https://github.com/oneup03/NV3D-Lib). No DLL injection into the
captured app; runs as a standalone .exe with an ImGui control panel.

## Requirements

- Windows 10 1903+ / Windows 11 (WGC capture)
- NVIDIA GPU with the legacy 3D Vision driver installed. The driver works on
  **any NVIDIA GPU and any current display-driver version** - it's installed
  alongside your main driver, not bundled into it. See
  [3DVision4All / Native install](https://oneup03.github.io/3DVision4All/docs/Native)
  for step-by-step instructions.
- A 3D Vision-certified display (120 Hz panel + IR emitter) configured as a 3D Vision target in NVCP
- "Set up stereoscopic 3D" enabled in the NVIDIA Control Panel
- For source: any windowed app that already produces side-by-side stereo
  (one frame split into left half / right half), or a
  [Geo-11](https://helixmod.blogspot.com/2022/06/announcing-new-geo-11-3d-driver.html)
  producer (see the Geo-11 setup note below)

## Install

Download the latest pre-release from the
[Releases](https://github.com/oneup03/NV3D-Glass/releases) page, unzip, and run `NV3D-Glass.exe`. The
binary is self-contained - no installer.

## Quick start

1. Launch `NV3D-Glass.exe`. The control panel opens.
2. **Capture Source** - pick `Window`, choose the SbS-producing app from
   the dropdown. Or pick `Monitor` to capture a whole display, or `Katanga`
   to receive from a Geo-11/Katanga producer over the shared-memory channel.
3. **3D Vision Output** - leave `(auto)` to let NV3DLib pick the
   3D-Vision-certified display, or pick one explicitly.
4. Click **Start**. The control panel minimises and a click-through FSE
   popup appears on the 3D Vision display. Put on the shutter glasses;
   you should see stereo separation.
5. Click the taskbar icon (or right-click the tray icon -> Show panel) to
   bring the control panel back. Press **Ctrl+F8** to hide / re-show the 3D
   output without stopping. Click **Stop** to tear down the 3D output cleanly.

## Hotkeys

| Hotkey   | Action                                                      |
|----------|-------------------------------------------------------------|
| Ctrl+F8  | Show / hide the 3D Vision FSE window (also kills/spawns the focus watcher) |
| Shift+F8 | Toggle eye-swap (reverses left/right; ~300 ms FSE blink while the presenter restarts) |

Both are global - they fire even when the captured game has foreground.

## Tray menu

Right-click the system tray icon for: Show/Hide panel, Start/Stop, Quit.
Double-click toggles panel visibility. Closing the panel with the **X**
button quits the app (and tears down the FSE popup cleanly).

## Settings & log

- `NV3D-Glass.ini` (next to the .exe) - picked source, output monitor,
  eye-swap, LightBoost / Driver Fix toggles, force-full-capture-Hz toggles,
  cursor lock / 3D cursor settings, hotkey bindings, panel geometry.
- `NV3D-Glass.log` (next to the .exe) - all `[NV3D]` messages from the
  presenter + suppressor + LightBoost subsystems, plus app-level
  start/stop/shutdown checkpoints. First file to attach when reporting
  an issue.

## Notes

- **LightBoost** (control panel toggle): applies the custom display
  timing from NV3DLib's embedded nvtimings DB for low-persistence
  shutter-friendly backlight strobing. Disabled if your panel isn't
  in the DB or already matches.
- **3DVision Driver Fix** (control panel toggle): in-process hooks that
  suppress the legacy 3D Vision driver's OSD warnings, depth adjustment, 
  and the rating/info overlay. Turn off if you suspect the
  hooks are interacting badly with another tool.
- **Click-through FSE popup**: the 3D output window is `WS_EX_LAYERED |
  WS_EX_TRANSPARENT` - mouse clicks pass through to whatever is
  underneath, so you can interact with the captured game without alt-tab.
- **Force full capture Hz** (control panel toggles, shown for Window /
  Monitor / Katanga sources): some games drop to a few fps when the mouse is
  idle and the 3D popup fully occludes them - WGC because DWM stops
  compositing the hidden source, and/or because Windows throttles the
  occluded game. Both toggles opt the game out of Windows power throttling
  and then keep it awake at ~120 Hz. Leave both off unless a source runs at a
  few fps hands-off, then enable whichever works for that game (or both).
  Only active while the 3D output is showing.
    - **Cursor jiggle** (`force_full_capture_hz`): nudges the cursor +1/-1 px
      (no net movement) each tick to force DWM to keep compositing. This is
      the one that fixes the WGC floor, but the global cursor events can make
      a raw-input game's camera shimmer.
    - **Game poke** (`force_capture_hz_postmsg`): posts a same-position
      `WM_MOUSEMOVE` straight to the game window to keep its render loop
      pumping. Gentler (no global cursor motion) and more portable, but only
      helps games that idle on lack of input - not the WGC compositing floor.
- **Lock cursor to captured window** (control panel toggle): while the 3D
  output is showing, clips the mouse to the focused window's client area so
  mouse-look games don't let the pointer wander onto the desktop or another
  monitor. The clip region follows whichever external window has focus, so
  alt+tabbing updates it - and focusing the control panel or the desktop
  frees the pointer, so it can never trap you. Only active while the FSE
  popup is visible; released on Stop / hide (Ctrl+F8) / quit.
- **Draw 3D cursor** (control panel toggle): draws a folded stereo arrow
  pointer into the 3D output at the mouse's position within the captured
  content. Because the captured frame never contains the OS cursor, this is
  the only pointer the 3D view can show. `Cursor depth` sets its parallax
  (0 = screen plane; +/- pushes it behind / in front), and `Cursor size`
  sets its height in pixels. Uses the same navigation-arrow shape as
  [3DVision4All](https://github.com/oneup03/3DVision4All).
- **Monitor capture caveat**: if you pick the *same* monitor as both
  source and output, the app refuses to start (would feed back into
  itself). Use Window capture instead, or pick a different display.
- **Geo-11 setup** (for the `Katanga` source mode): configure your Geo-11
  install to publish over the shared-memory channel NV3D-Glass listens on
  by setting the following in `d3dxdm.ini`:

  ```ini
  direct_mode = katanga_vr
  upscaling = 0
  ```

## Building from source

Requires Visual Studio 2022 with the **Desktop development with C++**
workload (v143 toolset, Windows 10 SDK).

```powershell
git clone --recurse-submodules https://github.com/oneup03/NV3D-Glass.git
cd NV3D-Glass
.\tools\build.ps1
```

`tools/build.ps1` locates MSBuild via `vswhere` (no Developer Command
Prompt needed) and builds the default Release-MT|x64. The vcxproj's
PreBuildEvent builds NV3DLib first (lib only, samples skipped). Pass
`-Configuration Debug-MT` / `-Target Rebuild` / etc. for other variants.
Output: `bin\x64\Release-MT\NV3D-Glass.exe`.

**In VS Code**: open the folder, then **Ctrl+Shift+B** runs the default
build task (Release-MT|x64). **F5** launches the built .exe under the
Visual Studio debugger.

## License

LGPL-2.1-or-later. See SPDX headers on individual source files and the
`LICENSE` file at the repo root.

## Acknowledgments

- [NV3DLib](https://github.com/oneup03/NV3D-Lib) - the 3D Vision presenter
- [VRto3D](https://github.com/oneup03/VRto3D) - reference for the FSE
  D3D9Ex + click-through + focus-management patterns
- [SR-Loom](https://github.com/effcol/SR-Loom) - reference structure for
  the standalone-app shell + WGC perf patterns
- [Katanga](https://github.com/bo3b/Katanga) - defines the shared-memory
  producer/consumer protocol our `Katanga` source mode implements
- [Geo-11](https://helixmod.blogspot.com/2022/06/announcing-new-geo-11-3d-driver.html) —
  modern producer for the Katanga protocol; we consume what it publishes
- [VRScreenCap](https://github.com/artumino/VRScreenCap) - prior art for the
  Katanga shared-memory consumer pattern
- [3DVision4All](https://github.com/oneup03/3DVision4All) - reference for the
  folded stereo navigation-arrow cursor shape
- [Dear ImGui](https://github.com/ocornut/imgui) - control panel UI
