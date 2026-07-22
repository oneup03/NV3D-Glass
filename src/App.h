/*
 * This file is part of NV3D-Glass.
 *
 * NV3D-Glass is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * NV3D-Glass is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with NV3D-Glass. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "Settings.h"
#include "Renderer.h"
#include "NV3DPresenter.h"
#include "HotkeyManager.h"
#include "TrayIcon.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace nv3dg {

class ICaptureSource;
class Gui;

enum class AppState {
    Idle,            // nothing running
    Running,         // capture + presenter live
    SourceLost,      // we were running, source disappeared
    ErrorPresenter,  // presenter init failed
    ErrorCapture,    // capture create failed
};

class App {
public:
    App();
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    bool  Init(HINSTANCE hInstance);
    void  Shutdown();
    int   Run();

    // ---- Actions exposed to Gui / TrayIcon / HotkeyManager ----
    bool  Start();                 // build capture + presenter from current Settings
    void  Stop();                  // tear down capture + presenter
    void  Restart();               // Stop + Start
    void  ToggleEyeSwap();
    void  TogglePanelVisible();
    void  ShowPanel();
    void  HidePanel();
    void  ToggleFseVisible();

private:
    bool  fse_visible_ = true;
    // True between Start and the first successful Katanga frame. While set,
    // the FSE popup is hidden and the test pattern is suppressed so the user
    // just sees their desktop until the producer comes online. The first
    // TryAcquire that yields a frame flips this off and reveals the popup.
    bool  waiting_katanga_first_frame_ = false;
public:
    void  Quit();

    void  RefreshSources();        // re-enumerate windows + monitors (for picker)

    // ---- State accessors for Gui ----
    Settings&             settings()         { return settings_; }
    const Settings&       settings() const   { return settings_; }
    Renderer&             renderer()         { return renderer_; }
    NV3DPresenter&        presenter()        { return presenter_; }
    HotkeyManager&        hotkeys()          { return hotkeys_; }
    AppState              state() const      { return state_; }
    UINT                  src_w() const      { return last_src_w_; }
    UINT                  src_h() const      { return last_src_h_; }
    float                 fps()   const      { return fps_; }
    HWND                  hwnd()  const      { return hwnd_; }
    bool                  panel_visible() const { return panel_visible_; }

    const std::wstring&   status_extra() const { return status_extra_; }
    void                  set_status_extra(std::wstring s) { status_extra_ = std::move(s); }

    bool                  is_capture_running() const { return cap_ != nullptr; }

    // WndProc forwards everything here.
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    bool   CreateControlPanelWindow();
    void   Tick();
    void   PumpHotkeys();
    void   ReregisterHotkeys();
    void   ApplyPanelGeometryFromSettings();
    void   PersistPanelGeometry();
    HWND   ResolveCaptureTarget(SourceKind* out_kind, HWND* out_hwnd, HMONITOR* out_hmon) const;
    HMONITOR ResolveOutputMonitor() const;

    // Cursor lock / 3D cursor. ComputeCaptureContentRect returns the screen-
    // space rectangle of what we're capturing - the ClipCursor region AND the
    // domain the mouse position is normalized against for the 3D cursor. It
    // follows the current foreground window (so alt+tab updates it), falling
    // back to the resolved capture target when the foreground is one of our
    // own windows / the shell. UpdateCursorLock runs every Tick and asserts or
    // releases the clip; ReleaseCursorClip is the teardown path.
    bool   ComputeCaptureContentRect(RECT* out) const;
    void   UpdateCursorLock();
    void   ReleaseCursorClip();

    // Capture idle-floor mitigation. When the mouse is idle and our fullscreen
    // popup occludes the game, both WGC and Katanga capture collapse to a few
    // fps - WGC because DWM stops compositing the occluded source (its frame
    // pool starves), Katanga because the OS/engine throttles the occluded
    // game's own rendering (the shared texture stops updating). Drives the
    // source hands-off every tick (~120Hz). Always opts the game process out of
    // power throttling, then applies whichever of two opt-in wake methods the
    // user enabled: force_full_capture_hz (net-zero SendInput cursor jiggle —
    // forces DWM composition) and/or force_capture_hz_postmsg (targeted
    // WM_MOUSEMOVE to the game window - keeps an input-driven loop pumping).
    // Self-gates on state / WGC-or-Katanga source / fse_visible_.
    void   DriveIdleCaptureHz();

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);

    HINSTANCE                          hinstance_     = nullptr;
    HWND                               hwnd_          = nullptr;
    ATOM                               wndclass_atom_ = 0;
    bool                               running_       = false;
    bool                               panel_visible_ = true;
    AppState                           state_         = AppState::Idle;

    Settings                           settings_;
    std::wstring                       ini_path_;

    Renderer                           renderer_;
    NV3DPresenter                      presenter_;
    // Crop region applied to each captured frame before it lands in staging
    // - used to strip the title bar / borders from window-source captures.
    // Empty (zero-area) rect means "no crop, copy the whole frame".
    RECT                               capture_src_region_{0, 0, 0, 0};
    HotkeyManager                      hotkeys_;
    TrayIcon                           tray_;
    std::unique_ptr<Gui>               gui_;
    std::unique_ptr<ICaptureSource>    cap_;

    // Captured game's PID, cached so Ctrl+F8 / "Show 3D Window" can re-spawn
    // the ForceFocus watcher on every show transition. 0 = no tracked game.
    DWORD                              tracked_pid_ = 0;

    // Katanga: a SYNCHRONIZE handle to the producer game's process, opened
    // at the reveal step. Polled with WaitForSingleObject each Tick so we
    // can fold the UI back to waiting state when the game exits (Geo-11
    // doesn't zero the mapping slot on death, so the slot-grace-timer
    // wouldn't catch this).
    HANDLE                             tracked_process_handle_ = nullptr;

    // Focus watcher state. The thread is JOINABLE (not detached) so we can
    // guarantee it has fully exited - and therefore released any
    // AttachThreadInput pair it was holding - before our process moves on.
    //
    // A detached watcher is the root cause of the "whole-display freeze on
    // quit, cursor frozen, reboot required" symptom: if the process exits
    // (or Shutdown advances) while the watcher is mid-ForceFocus, the OS-
    // wide input chain is left attached to a dying thread and wedges
    // system-wide. Joining caps the wedge window at one watcher iteration.
    // VRto3D's NvStereoDx9Presenter has the same detached pattern at line
    // ~1303 and the same symptom; the fix ports cleanly.
    std::thread                        focus_watcher_thread_;
    std::shared_ptr<std::atomic<bool>> focus_watcher_stop_;

    void StartForceFocusWatcher(DWORD pid);
    void StopForceFocusWatcher();

    // Transition back to Katanga's "waiting for producer" UI. Fully
    // RELEASES the FSE D3D9 session (a lingering fullscreen-exclusive
    // stereo device wedges the next game's display takeover); the reveal
    // path in Tick re-Inits the presenter when a producer publishes again.
    // Keeps the tracked producer handle while that process is still alive
    // (res-change reconnects are the same process, and the handle is the
    // only Katanga producer-death signal that exists). Caller is
    // responsible for calling ResetForReconnect() on the CaptureKatanga so
    // the next TryAcquire can detect a new producer.
    void EnterKatangaWaitingMode();

    // Diagnostic frame counter - every N ticks we log the live D3D11 device
    // state, gated on NV3D_GLASS_D3D11_DEBUG=1. Used to catch a
    // partially-removed device before the explicit TDR-detection paths
    // (CaptureKatanga IsLost, pre-SubmitFrame check) fire.
    int                                diag_periodic_counter_ = 0;
    // Any-state device-removed watchdog cadence (see top of Tick).
    int                                dev_check_counter_ = 0;
    // Late producer-adoption probe (Katanga): when no producer process was
    // identified at reveal, Tick periodically samples the foreground window
    // and adopts a stable non-self pid so death detection works. Counter is
    // the probe cadence; candidate is the pid seen on the previous probe.
    int                                adopt_check_counter_ = 0;
    DWORD                              adopt_candidate_pid_ = 0;
    // Last foreground process that isn't us and isn't the shell, sampled
    // continuously in Tick. The Katanga reveal falls back to this when the
    // foreground at first-frame is our own control panel - the COMMON case,
    // since the user just clicked Start on us while the game was already
    // running. Without it the focus hand-off targeted nothing and the
    // restored popup kept activation on our process instead of the game.
    int                                fg_sample_counter_ = 0;
    DWORD                              last_external_fg_pid_ = 0;

    // Cursor-lock state. cursor_clipped_ tracks whether we currently hold a
    // ClipCursor so we only call ClipCursor(nullptr) when we actually set one.
    // last_content_rect_ is the capture content rect computed once per Tick
    // (in UpdateCursorLock) and reused by the 3D-cursor draw so we don't
    // resolve the foreground window twice.
    bool                               cursor_clipped_    = false;
    bool                               last_content_valid_ = false;
    RECT                               last_content_rect_ { 0, 0, 0, 0 };
    // Presenter-death watchdog. Windows can context-reset JUST our D3D9 FSE
    // device (D3DERR_DEVICEHUNG, no system-wide TDR) - the D3D11 device
    // stays healthy so the device-removed watchdogs never fire, NV3DLib
    // marks itself dead and fast-fails every Present, and the 3D window
    // freezes forever. Tick counts consecutive failing SubmitFrame HRESULTs
    // and bounces the FSE session when the streak says the presenter is
    // gone. The timestamp throttles back-to-back recoveries so a genuinely
    // broken driver state degrades to a Stop instead of an FSE-bounce loop.
    int                                submit_fail_streak_ = 0;
    std::chrono::steady_clock::time_point last_presenter_dead_recovery_{};
    // Producer-death grace. When the Katanga producer process exits we do
    // NOT Stop() on the same tick: the driver is still reclaiming the dead
    // game's GPU context, and releasing our live FSE stereo device into
    // that window strained the driver badly enough (D3D9 release stalled
    // 1.8s) that the eventual process exit hard-froze the display. Arm a
    // ~1s deadline instead and Stop when it lapses. Zero = unarmed.
    std::chrono::steady_clock::time_point producer_death_stop_at_{};

    // Telemetry / status
    UINT                               last_src_w_ = 0;
    UINT                               last_src_h_ = 0;
    UINT                               last_src_fmt_ = 0;
    std::chrono::steady_clock::time_point fps_t0_{};
    int                                fps_frames_ = 0;
    float                              fps_       = 0.0f;
    uint32_t                           test_pattern_frame_ = 0;
    std::chrono::steady_clock::time_point last_present_ts_{};
    // Producer-gap telemetry: timestamp of the last tick that yielded a new
    // capture frame, and whether an over-1s gap warning is currently open.
    // Correlated against NV3DLib's fence-stall log line to tell a
    // device-wide GPU stall apart from a game-side hitch.
    std::chrono::steady_clock::time_point last_capture_frame_ts_{};
    bool                               producer_gap_logged_ = false;

    // Idle-capture-drive state (see DriveIdleCaptureHz). The wake fires every
    // tick (~120Hz), so there's no rate-throttle timestamp. dwm_poke_active_
    // edge-triggers the on/off log. game_process_tweaked_pid_ is the pid we've
    // applied the power-throttle opt-out to (one-shot per process);
    // cached_game_hwnd_ is that game's resolved top-level window for the
    // targeted WM_MOUSEMOVE. Both reset on Stop and on a pid change.
    bool                               dwm_poke_active_ = false;
    DWORD                              game_process_tweaked_pid_ = 0;
    HWND                               cached_game_hwnd_ = nullptr;

    std::wstring                       status_extra_;
};

}  // namespace nv3dg
