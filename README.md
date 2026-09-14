# NV3D-Glass

Capture a side-by-side stereo source (game, video player, Katanga producer) and
re-present it as true frame-sequential **NVIDIA 3D Vision** output via
NV3DLib. No DLL injection into the captured app; runs as a standalone .exe with an ImGui control panel.

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
  katanga (full-SbS) producer (see the Geo-11 example below)

## Install

Download the latest pre-release from the
<a href="https://github.com/oneup03/NV3D-Glass/releases">Releases</a> page, unzip, and run `NV3D-Glass.exe`. The
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
  frame pacing / output sync settings, cursor lock / 3D cursor settings,
  hotkey bindings, panel geometry. Also holds the `[NvProfileBackup]` undo
  record for any NVIDIA driver settings applied from the control panel -
  deleting that section means those changes can no longer be undone from
  the control panel.
- `NV3D-Glass.log` (next to the .exe) - all `[NV3D]` messages from the
  presenter + suppressor + LightBoost subsystems, plus app-level
  start/stop/shutdown checkpoints. First file to attach when reporting
  an issue.

## Smoothness & sync

A 3D Vision panel is frame-sequential: one stereo pair occupies **two**
display refreshes, so a 120 Hz panel can show at most **60 stereo frames per
second**. A source running at anything other than that rate has to be
resampled to it, and an uneven resample is what stutter looks like. The
controls below exist to get every clock in the chain onto the same beat.

- **Frame pacing** (`pacing_mode`, default *Present-clocked*): captures one
  frame per completed 3D present, so the panel sets the pace. The previous
  behaviour (*Legacy fixed tick*) ran on a free-running 8 ms timer and
  captured, copied and submitted regardless of what the display or the source
  were doing - most of that work was then thrown away, and the leftover
  competed with the captured game for the GPU. Applies immediately; switch to
  Legacy only if a machine misbehaves under the paced loop.
- **Output sync** (`output_sync`, default *Vsync on*): the presentation
  interval of the 3D output swap chain.
    - *Vsync on* - tear-free and the safe default. A present that misses its
      slot waits for the next one, which is the classic "DX9 frame rate
      halves" behaviour.
    - *Vsync off* - presents are never throttled. This tears on its own; its
      purpose is to let the **driver** do the syncing, because NVIDIA's Fast
      Sync only engages for applications that are not already vsync-limited.
      Selecting this mode writes the Fast Sync driver profile for you (see
      below); switching away from it takes the profile back off.
    - *Half rate* - deliberately halve the stereo rate (30 fps on a 120 Hz
      panel) to give a source that cannot hold 60 an even cadence instead of
      an uneven one.
- **Max frame latency** (`max_frame_latency`, default 1): driver present queue
  depth. 1 is the shortest; 2-3 costs a frame of latency but absorbs a hitch
  that would otherwise drop a whole stereo pair.

- **FLIPEX swap chain** (`use_flipex`, default off): **experimental**. Creates
  the output swap chain with `D3DSWAPEFFECT_FLIPEX`, normally a lower-latency
  present model. Unproven with 3D Vision, because the driver demuxes left/right
  by *scanning* the presented surface and this changes the present model
  underneath that scanner. If the image goes flat/mono, turn it back off. If
  the driver refuses it outright, NV3D-Glass falls back to the standard swap
  chain on its own and says so in the log.

**Which restart does a setting need?** Two different things are being set here,
and they bind at different moments:

| Setting | Takes effect |
| --- | --- |
| Output sync interval, Max frame latency, FLIPEX | next **Start** - NV3DLib reads them while creating the 3D device |
| Fast Sync profile, flip-interval flag | **restart of NV3D-Glass** - the driver binds a profile as a process starts up |

Output sync spans both: the presentation interval changes on the next Start,
but the Fast Sync profile it turns on or off needs the restart, which is why
changing it offers one.

### NVIDIA driver profile

NV3D-Glass can write NVIDIA driver settings for you, so you don't have to
reproduce them by hand in NVIDIA Control Panel or NVIDIA Inspector. Everything
it overwrites is recorded first so it can be put back exactly, including
deleting a profile or application entry it had to create.

**The Fast Sync profile is owned by the Output sync setting**, not by a button.
Selecting *Vsync off* applies it (**Fast Sync**, 1 pre-rendered frame,
maximum-performance power mode) and selecting anything else removes it again -
the profile is meaningless without that mode, and having two places to set one
thing is how the two get out of step. It asks for confirmation the first time
and is silent afterwards; set `auto_apply_nvprofile=0` in the INI to disable
the coupling entirely.

Fast Sync is not exposed in the standard Control Panel at all: it renders
unthrottled and shows only the newest finished frame at each vblank, so you get
tear-free output without hard vsync halving the rate when a frame is late.
Nothing outside NV3D-Glass's own profile is touched. A checkbox beside the
dropdown adds the undocumented
`Vsync - Behavior Flags = IGNORE_FLIPINTERVAL_MULTIPLE` to that profile - 3D
Vision doubles the flip interval to fit a stereo pair into two refreshes, and
this is the driver's knob for that multiplication.

**Apply vsync + N fps cap to game** is a separate, opt-in toggle, and the
single most effective setting for smoothness. It caps the captured game to the
panel's stereo rate (half its refresh) and forces vsync on for it: the cap
stops the game drawing frames that can never be shown, and vsync locks its
presents to the same vblank train we present on, so each stereo pair carries
one whole new frame instead of part of one. It is written into whatever profile
the game **already** belongs to, so that profile's other settings are
preserved, and the same button removes it again. The two are undone
independently - switching Output sync away from *Vsync off* never disturbs a
game's frame cap.

The control sits in the **Capture Source** section, under *Refresh sources*,
with the target executable named beside it. In Window capture mode that target
is exact - the game's process was resolved at Start. In **Katanga** mode the
protocol carries a texture handle and nothing else, so there is nothing to
resolve: pick the process from the **Game (for driver profile)** dropdown.
There is deliberately no auto-detect option - the only thing available to guess
from is window focus, and a wrong guess would write driver settings for an
unrelated application. Nothing is written until you pick one. The dropdown
affects only which profile is written; capture still takes whatever the
producer publishes.

> **A profile change only takes effect in a freshly started process.** The
> NVIDIA driver reads an application's profile while that application is
> initialising its graphics DLL, so a game that is already running keeps
> whatever it started with - **restart the game**.
>
> This applies to NV3D-Glass itself too, and stopping and restarting the 3D
> session is *not* enough: a D3D11 device is created during startup, so our
> profile is already bound by the time you can change anything. Switching
> **Output sync** into or out of *Vsync off* therefore offers to restart
> NV3D-Glass, which saves your settings, launches a replacement that waits for
> this one to exit, and quits.

The applied/removed state is tracked **per executable**, so capping one game
never makes another appear capped, and removing one leaves the others alone.

Both are disabled while a 3D session is live: an elevation prompt stealing
foreground from the fullscreen stereo output is exactly the kind of focus
fight that destabilises it.

If *Vsync off* is still selected the next time NV3D-Glass starts, it re-applies
the Fast Sync profile at launch (without prompting for elevation) so the mode
isn't left silently inert. If it can't - because that machine needs admin for
it - the Output sync section says so rather than pretending the mode works.

Whether saving needs administrator rights is decided **per setting**, not per
session: the general 3D settings written here go through as a plain user on
machines where the 3D Vision *stereo* settings (`StereoProfile` and friends,
as written by 3DVision4All's `EnableWindowed3D`) are refused outright. So
NV3D-Glass always tries in-process first and only re-runs that one change in
an elevated helper when the driver actually returns access-denied - no prompt
when none is needed. If you would rather not let it near your driver profile
at all, the
equivalent by hand is: cap the game to half your panel's refresh and turn
vsync on for it, and leave **Output sync** on *Vsync on*.

To measure any of this, set `NV3D_GLASS_PACING=1` and NV3D-Glass logs a
once-per-second line with the real tick period, how many ticks captured a
frame, and the present worker's accepted/dropped/duration counters.

### Driver profile from the command line

The same profile operations the control panel uses are reachable directly,
which is handy for support and for undoing things without launching the UI.
Each runs one driver transaction and exits without creating a window:

```
NV3D-Glass.exe --apply-nvprofile inspect [game.exe]   report what the driver
                                                      currently has (read-only)
NV3D-Glass.exe --apply-nvprofile revert               undo everything on record
NV3D-Glass.exe --apply-nvprofile revert self          undo just the Fast Sync profile
NV3D-Glass.exe --apply-nvprofile revert game game.exe undo just that game's cap
```

`revert` with no scope is the way out if the undo record is ever in a state the
control panel can't act on. These write to `NV3D-Glass-nvprofile.log` rather
than the main log, so they don't disturb a running instance.

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
  sets its height in pixels.
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

- <a href="https://github.com/oneup03/NV3D-Lib">NV3DLib</a> - the 3D Vision presenter
- <a href="https://github.com/effcol/SR-Loom">SR-Loom</a> - reference structure for
  the standalone-app shell + WGC perf patterns
- <a href="https://github.com/bo3b/Katanga">Katanga</a> - defines the shared-memory
  producer/consumer protocol our `Katanga` source mode implements
- <a href="https://helixmod.blogspot.com/2022/06/announcing-new-geo-11-3d-driver.html">Geo-11</a> —
  modern producer for the Katanga protocol; we consume what it publishes
- <a href="https://github.com/artumino/VRScreenCap">VRScreenCap</a> - prior art for the
  Katanga shared-memory consumer pattern
- <a href="https://github.com/ocornut/imgui">Dear ImGui</a> - control panel UI
