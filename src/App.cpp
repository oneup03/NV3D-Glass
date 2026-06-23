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

#include "App.h"

#include "Capture.h"
#include "CaptureDXGI.h"
#include "CaptureKatanga.h"
#include "CaptureSource.h"
#include "Gui.h"
#include "Logging.h"
#include "SourceEnumerator.h"

#include "imgui.h"
#include "imgui_impl_win32.h"

#include <Windows.h>
#include <d3d11.h>
#include <chrono>
#include <thread>

// ImGui's Win32 backend intentionally hides this declaration behind `#if 0`
// (see imgui_impl_win32.h:34-36) so the header doesn't pull in <windows.h>.
// The backend's documentation tells consumers to forward-declare it manually.
extern IMGUI_IMPL_API LRESULT
ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace nv3dg {

namespace {

constexpr wchar_t kWndClass[] = L"NV3DGlass_ControlPanel";

constexpr int kHKEyeSwap     = 1;
constexpr int kHKTogglePanel = 2;
constexpr int kHKQuit        = 3;
constexpr int kHKToggleFse   = 4;

// VRto3D ForceFocus port (verbatim from vrto3dlib/win32_helper.hpp). Pulls
// the captured game's HWND into the actual foreground so keyboard/mouse go
// there instead of getting eaten by our control panel.
void ForceFocusToGame(HWND target) {
    if (!target || !IsWindow(target)) return;

    // Dummy Alt down/up wakes Win32's focus-lock logic.
    INPUT input{};
    input.type   = INPUT_KEYBOARD;
    input.ki.wVk = VK_MENU;
    SendInput(1, &input, sizeof(INPUT));
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
    Sleep(50);

    DWORD my_tid     = GetCurrentThreadId();
    DWORD target_tid = GetWindowThreadProcessId(target, nullptr);
    if (!target_tid) return;
    AttachThreadInput(my_tid, target_tid, TRUE);
    // SW_RESTORE only when the target is actually minimized — calling
    // SW_RESTORE on a MAXIMIZED window un-maximizes it (per MSDN), which
    // shrinks the captured source mid-session and produces a picture-in-
    // picture artifact because our staging is sized to the pre-shrink
    // client area. Leave maximized / normal windows in their current state.
    if (IsIconic(target)) {
        ShowWindow(target, SW_RESTORE);
    }
    SetForegroundWindow(target);
    SetFocus(target);
    SetActiveWindow(target);
    BringWindowToTop(target);
    AttachThreadInput(my_tid, target_tid, FALSE);
    Sleep(50);
}

}  // anonymous

App::App()  = default;
App::~App() { Shutdown(); }

bool App::Init(HINSTANCE hInstance) {
    hinstance_ = hInstance;
    ini_path_  = DefaultIniPath();
    LoadSettings(settings_, ini_path_.c_str());
    InitFileLog(DefaultLogPath().c_str());

    if (!CreateControlPanelWindow()) return false;
    if (!renderer_.Init(hwnd_))      return false;

    gui_ = std::make_unique<Gui>();
    gui_->RefreshSources();

    hotkeys_.Attach(hwnd_);
    ReregisterHotkeys();

    tray_.Install(hwnd_, L"NV3D-Glass — capture & 3D Vision");
    tray_.SetActions(
        [this] { ShowPanel(); },
        [this] { HidePanel(); },
        [this] {
            if (state_ == AppState::Running) Stop();
            else                              Start();
        },
        [this] { Quit(); });

    panel_visible_ = true;
    ShowWindow(hwnd_, SW_SHOW);

    // Don't autostart on launch — the saved source may be stale and the user
    // hasn't asked us to do anything yet. Wait for them to hit Start.

    return true;
}

void App::Shutdown() {
    // Step-by-step logs so a freeze in any one step pinpoints itself in the
    // log. The freeze symptom we're hunting (whole-display freeze on quit
    // after a session) goes silent somewhere in this chain — without these
    // checkpoints we can't tell whether it's tray removal, ImGui DX11
    // shutdown, swap chain release, the control-panel DestroyWindow, etc.
    Log(NV3D::LogLevel::Info, L"App::Shutdown  begin");
    Stop();
    Log(NV3D::LogLevel::Info, L"App::Shutdown  after Stop");
    PersistPanelGeometry();
    if (!ini_path_.empty()) SaveSettings(settings_, ini_path_.c_str());
    Log(NV3D::LogLevel::Info, L"App::Shutdown  settings persisted");

    tray_.Remove();
    Log(NV3D::LogLevel::Info, L"App::Shutdown  tray removed");
    hotkeys_.Clear();
    Log(NV3D::LogLevel::Info, L"App::Shutdown  hotkeys cleared");
    gui_.reset();
    Log(NV3D::LogLevel::Info, L"App::Shutdown  gui_ reset");
    renderer_.Shutdown();
    Log(NV3D::LogLevel::Info, L"App::Shutdown  renderer_ shutdown done");

    if (hwnd_)          { DestroyWindow(hwnd_);                 hwnd_          = nullptr; }
    if (wndclass_atom_) { UnregisterClassW(kWndClass, hinstance_); wndclass_atom_ = 0;     }
    Log(NV3D::LogLevel::Info, L"App::Shutdown  control panel destroyed");
    ShutdownFileLog();
}

int App::Run() {
    running_ = true;
    auto next = std::chrono::steady_clock::now();
    fps_t0_   = next;
    fps_frames_ = 0;

    while (running_) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running_ = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running_) break;

        Tick();

        next += std::chrono::milliseconds(8);
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else            next = now;
    }
    return 0;
}

bool App::CreateControlPanelWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &App::StaticWndProc;
    wc.hInstance     = hinstance_;
    wc.lpszClassName = kWndClass;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wndclass_atom_   = RegisterClassExW(&wc);
    if (!wndclass_atom_) return false;

    int x = settings_.panel_x, y = settings_.panel_y;
    int w = settings_.panel_w > 0 ? settings_.panel_w : 480;
    int h = settings_.panel_h > 0 ? settings_.panel_h : 600;
    if (x == CW_USEDEFAULT || y == CW_USEDEFAULT) { x = 120; y = 120; }

    hwnd_ = CreateWindowExW(
        0, kWndClass, L"NV3D-Glass",
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        nullptr, nullptr, hinstance_, this);
    return hwnd_ != nullptr;
}

void App::ApplyPanelGeometryFromSettings() {
    if (!hwnd_) return;
    int x = settings_.panel_x, y = settings_.panel_y;
    int w = settings_.panel_w > 0 ? settings_.panel_w : 480;
    int h = settings_.panel_h > 0 ? settings_.panel_h : 600;
    if (x == CW_USEDEFAULT || y == CW_USEDEFAULT) { x = 120; y = 120; }
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void App::PersistPanelGeometry() {
    if (!hwnd_) return;
    RECT rc{};
    if (GetWindowRect(hwnd_, &rc)) {
        settings_.panel_x = rc.left;
        settings_.panel_y = rc.top;
        settings_.panel_w = rc.right  - rc.left;
        settings_.panel_h = rc.bottom - rc.top;
    }
}

HWND App::ResolveCaptureTarget(SourceKind* out_kind, HWND* out_hwnd, HMONITOR* out_hmon) const {
    if (out_kind) *out_kind = settings_.source_kind;
    if (out_hwnd) *out_hwnd = nullptr;
    if (out_hmon) *out_hmon = nullptr;
    if (settings_.source_kind == SourceKind::Window) {
        HWND h = FindWindowByStableId(settings_.source_id);
        if (out_hwnd) *out_hwnd = h;
        return h;
    }
    if (settings_.source_kind == SourceKind::Monitor) {
        HMONITOR m = FindMonitorByDeviceName(settings_.source_id);
        if (out_hmon) *out_hmon = m;
        return reinterpret_cast<HWND>(m);  // sentinel; caller checks kind
    }
    return nullptr;
}

HMONITOR App::ResolveOutputMonitor() const {
    if (settings_.output_monitor_id.empty()) return nullptr;  // auto
    return FindMonitorByDeviceName(settings_.output_monitor_id);
}

bool App::Start() {
    Stop();

    SourceKind kind{}; HWND src_hwnd{}; HMONITOR src_hmon{};
    ResolveCaptureTarget(&kind, &src_hwnd, &src_hmon);

    // Try to start the capture session FIRST so we can size the staging
    // texture to exactly what WGC will deliver. CopyResource on dim-match is
    // an order of magnitude cheaper than the scaler shader pass we'd
    // otherwise need every frame — and the scaler's extra GPU work was
    // contending with the captured source's own rendering, dragging the
    // observed source rate down to "slow motion" levels.
    //
    // Fall through to test-pattern mode (2560x720, no scaler ever) if the
    // user didn't pick a source or capture refused.
    std::wstring capture_warning;
    if (kind == SourceKind::Window) {
        if (!src_hwnd) {
            capture_warning = L"selected window not running — showing test pattern";
        } else {
            cap_ = CaptureWGC::CreateForWindow(renderer_.Device(), src_hwnd);
            if (!cap_) capture_warning = L"WGC refused this window — showing test pattern";
        }
    } else if (kind == SourceKind::Monitor) {
        if (!src_hmon) {
            capture_warning = L"selected monitor not connected — showing test pattern";
        } else if (HMONITOR out_hmon = ResolveOutputMonitor();
                   out_hmon && out_hmon == src_hmon) {
            // Both WGC monitor mode and DXGI Output Duplication scan out the
            // composited desktop including our FSE popup. Capturing the same
            // monitor we're presenting on recurses (every frame contains the
            // previous frame's output). Refuse before we open the session.
            capture_warning = L"capture and output monitor are the same — pick a different display to avoid a feedback loop";
        } else {
            cap_ = CaptureWGC::CreateForMonitor(renderer_.Device(), src_hmon);
            if (!cap_) {
                cap_ = CaptureDXGI::CreateForMonitor(renderer_.Device(), src_hmon);
                if (!cap_) capture_warning = L"WGC + DXGI both refused this monitor — showing test pattern";
            }
        }
    } else if (kind == SourceKind::Katanga) {
        // Create succeeds even if no producer is running yet: we open or
        // create Local\KatangaMappedFile and sit in "waiting" state until a
        // producer publishes a handle. The test pattern Renderer paints below
        // is what the user sees until then.
        cap_ = CaptureKatanga::Create(renderer_.Device());
        if (!cap_) capture_warning = L"Katanga mapping setup failed — showing test pattern";
    }

    UINT staging_w = 0, staging_h = 0;
    capture_src_region_ = RECT{0, 0, 0, 0};
    if (auto* wgc = dynamic_cast<CaptureWGC*>(cap_.get())) {
        // Crop to the window's client area — strips the title bar, borders,
        // and DWM shadow so they don't get sliced as part of the SbS layout.
        // For monitor sources / borderless-fullscreen windows ClientAreaInCapture
        // returns the full frame, so this stays a no-op.
        const RECT client = wgc->ClientAreaInCapture();
        const LONG cw = client.right - client.left;
        const LONG ch = client.bottom - client.top;
        if (cw > 0 && ch > 0) {
            staging_w = static_cast<UINT>(cw);
            staging_h = static_cast<UINT>(ch);
            capture_src_region_ = client;
        } else {
            staging_w = wgc->InitialWidth();
            staging_h = wgc->InitialHeight();
        }
    }
    // Katanga deliberately falls through to the monitor-based default below
    // even when the producer is already up: pinning staging to a stable size
    // (output_monitor × 2) lets the scaler handle every source size — both
    // mid-session producer-side resolution changes and upscale-when-smaller —
    // without ever resizing the staging texture or restarting the presenter.
    if (staging_w == 0 || staging_h == 0) {
        // Test-pattern mode (no cap_), DXGI fallback (no Initial* getter), or
        // Katanga waiting (producer hasn't published yet). Size the staging
        // to the output monitor's resolution × 2 in width — that matches the
        // SbS layout NV3DLib expects and lines up with the panel a typical
        // producer is rendering at, so the post-Restart staging will already
        // be the right size in the common case.
        HMONITOR fallback_mon = ResolveOutputMonitor();
        if (!fallback_mon) {
            fallback_mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        }
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (fallback_mon && GetMonitorInfoW(fallback_mon, &mi)) {
            staging_w = static_cast<UINT>(mi.rcMonitor.right  - mi.rcMonitor.left) * 2;
            staging_h = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
        } else {
            // Ultimate fallback — no monitor info available at all.
            staging_w = 2560;
            staging_h = 720;
        }
    }

    if (!renderer_.CreateFixedStaging(staging_w, staging_h)) {
        if (cap_) { cap_->Stop(); cap_.reset(); }
        state_ = AppState::ErrorPresenter;
        status_extra_ = L"failed to allocate staging texture";
        return false;
    }

    // For Katanga we always treat the first Tick as the "reveal moment" —
    // even when the producer is already running, the first TryAcquire is what
    // flips fse_visible_ on. Suppress the test pattern in this mode so the
    // user sees their desktop, not stereo bars, while waiting.
    const bool katanga_mode = (kind == SourceKind::Katanga);
    waiting_katanga_first_frame_ = katanga_mode;
    if (!katanga_mode) {
        renderer_.FillTestPattern(0);   // known-good stereo content for first frame
    }

    settings_.target_monitor = ResolveOutputMonitor();

    DWORD tracked_pid = 0;
    if (kind == SourceKind::Window && src_hwnd) {
        GetWindowThreadProcessId(src_hwnd, &tracked_pid);
    } else if (kind == SourceKind::Monitor && src_hmon) {
        HWND fg = GetForegroundWindow();
        if (fg && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == src_hmon) {
            GetWindowThreadProcessId(fg, &tracked_pid);
        }
    }
    // For "test pattern only" runs (no source), pass our own PID so NV3DLib's
    // VRto3D loop keeps the popup pinned and exits cleanly when we exit.
    if (tracked_pid == 0) tracked_pid = GetCurrentProcessId();

    if (!presenter_.Init(renderer_.Device(), settings_, tracked_pid)) {
        if (cap_) { cap_->Stop(); cap_.reset(); }
        renderer_.ReleaseStaging();
        state_ = AppState::ErrorPresenter;
        status_extra_ = L"presenter init failed (NV3DLib log has details)";
        return false;
    }

    state_         = AppState::Running;
    status_extra_  = capture_warning;   // empty on the happy path
    fse_visible_   = !katanga_mode;     // hide popup until first Katanga frame; otherwise NV3DLib's default
    fps_t0_        = std::chrono::steady_clock::now();
    fps_frames_    = 0;
    fps_           = 0.0f;
    test_pattern_frame_ = 0;
    tray_.SetStartStopLabel(L"Stop");
    if (katanga_mode) {
        presenter_.SetVisible(false);
        status_extra_ = L"waiting for Katanga producer";
    }

    // VRto3D-style hand-off. Order matters here:
    //   1. Hide the control panel FIRST so it stops being the foreground
    //      window. If we ForceFocus before hiding, the subsequent SW_HIDE
    //      makes Windows pick a new foreground (usually our FSE popup) and
    //      undoes the focus we just set.
    //   2. Pump messages so the OS finishes the focus transition from the
    //      panel hide before we override it.
    //   3. ForceFocus to the game.
    //   4. Spawn a detached watcher thread that re-asserts ForceFocus once
    //      per second for 15s — VRto3D's exact pattern (their
    //      nvstereo_dx9_presenter.cpp:1303-1314). The first
    //      SetForegroundWindow is fragile; the watcher catches anything that
    //      steals focus back in the first few seconds (the game's own
    //      splash-screen window-creation, anti-cheat shims, etc.).
    // Skip the panel-hide + force-focus dance for Katanga: there's no game
    // window owned by us to yank into focus (the producer's game window lives
    // in another process and we don't know which one), and HidePanel'ing
    // ourselves while waiting for a producer would just disappear the only UI
    // the user has.
    if (kind != SourceKind::None && kind != SourceKind::Katanga) {
        HWND game_hwnd = src_hwnd;
        if (!game_hwnd && tracked_pid != 0 && tracked_pid != GetCurrentProcessId()) {
            struct Ctx { DWORD pid; HWND result; } ctx{ tracked_pid, nullptr };
            EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid == c->pid && IsWindowVisible(h) && GetWindowTextLengthW(h) > 0) {
                    c->result = h;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&ctx));
            game_hwnd = ctx.result;
        }

        HidePanel();
        // Pump our queue so WM_ACTIVATE / WM_KILLFOCUS from the panel hide
        // drain before the watcher thread starts grabbing foreground.
        MSG msg;
        for (int i = 0; i < 3; ++i) {
            Sleep(10);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        tracked_pid_ = tracked_pid;
        StartForceFocusWatcher(tracked_pid_);
    } else {
        tracked_pid_ = 0;
    }

    return true;
}

void App::Stop() {
    Log(NV3D::LogLevel::Info,
        L"App::Stop  cap_=%p presenter.IsActive=%s state=%d",
        (void*)cap_.get(),
        presenter_.IsActive() ? L"true" : L"false",
        (int)state_);
    StopForceFocusWatcher();
    if (cap_) { cap_->Stop(); cap_.reset(); }
    presenter_.Shutdown();
    renderer_.ReleaseStaging();
    if (state_ == AppState::Running) state_ = AppState::Idle;
    tracked_pid_ = 0;
    if (tracked_process_handle_) {
        CloseHandle(tracked_process_handle_);
        tracked_process_handle_ = nullptr;
    }
    waiting_katanga_first_frame_ = false;
    tray_.SetStartStopLabel(L"Start");
    // Refresh the source picker so the user gets up-to-date windows /
    // monitors when they pick something different before clicking Start
    // again. Cheap and only fires when they intentionally tear down.
    if (gui_) gui_->RefreshSources();
}

void App::Restart() {
    if (state_ == AppState::Running) {
        Stop();
        Start();
    }
}

void App::ToggleEyeSwap() {
    settings_.eye_swap = !settings_.eye_swap;
    if (state_ == AppState::Running) Restart();
}

void App::TogglePanelVisible() {
    if (panel_visible_) HidePanel();
    else                ShowPanel();
}

void App::ShowPanel() {
    if (!hwnd_) return;
    // SW_RESTORE handles both the SW_MINIMIZE (HidePanel) case and the
    // initial-hidden case.
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    panel_visible_ = true;
}

void App::HidePanel() {
    if (!hwnd_) return;
    // SW_MINIMIZE rather than SW_HIDE so the taskbar icon stays — the user
    // can click it to restore the control panel without needing the hotkey.
    ShowWindow(hwnd_, SW_MINIMIZE);
    panel_visible_ = false;
}

void App::ToggleFseVisible() {
    Log(NV3D::LogLevel::Info,
        L"App::ToggleFseVisible  state=%d fse_visible_=%d presenter.IsActive=%s",
        (int)state_, (int)fse_visible_,
        presenter_.IsActive() ? L"true" : L"false");
    // Mirrors VRto3D's Ctrl+F8 → flip is_on_top_/man_on_top_ pattern. We just
    // flip an atomic NV3DLib reads on its window thread; that thread handles
    // suppress_minimize_ sequencing + SW_MINIMIZE/SW_RESTORE itself.
    //
    // Show transition also (re)spawns the ForceFocus watcher so the
    // captured game gets foreground back. Hide transition kills the watcher
    // so it stops yanking focus while the user is trying to interact with
    // the desktop.
    if (state_ != AppState::Running) return;
    fse_visible_ = !fse_visible_;
    presenter_.SetVisible(fse_visible_);
    if (fse_visible_) {
        StartForceFocusWatcher(tracked_pid_);
        status_extra_.clear();
    } else {
        StopForceFocusWatcher();
        status_extra_ = L"FSE minimized (Ctrl+F8 to restore)";
    }
}

void App::Quit() {
    Log(NV3D::LogLevel::Info, L"App::Quit  state=%d", (int)state_);
    running_ = false;
    PostQuitMessage(0);
}

void App::RefreshSources() {
    if (gui_) gui_->RefreshSources();
}

void App::StartForceFocusWatcher(DWORD pid) {
    // Always drop any previous watcher first. Stop also JOINS, so we know the
    // old thread has released any AttachThreadInput pair before we spawn the
    // new one — no two-watcher-at-once window.
    StopForceFocusWatcher();
    if (pid == 0 || pid == GetCurrentProcessId()) return;

    focus_watcher_stop_ = std::make_shared<std::atomic<bool>>(false);
    auto stop = focus_watcher_stop_;
    focus_watcher_thread_ = std::thread([pid, stop]() {
        for (int i = 0; i < 16; ++i) {
            if (stop->load(std::memory_order_relaxed)) return;
            struct Ctx { DWORD pid; HWND result; } c{ pid, nullptr };
            EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                auto* cc = reinterpret_cast<Ctx*>(lp);
                DWORD wpid = 0;
                GetWindowThreadProcessId(h, &wpid);
                if (wpid == cc->pid && IsWindowVisible(h) && GetWindowTextLengthW(h) > 0) {
                    cc->result = h;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&c));
            if (c.result && GetForegroundWindow() != c.result) {
                ForceFocusToGame(c.result);
            }
            // 1s tick split into 10×100ms so the stop flag is checked
            // promptly — Ctrl+F8 to hide should silence the watcher within
            // a tenth of a second, not after a full second of yanking.
            for (int j = 0; j < 10; ++j) {
                if (stop->load(std::memory_order_relaxed)) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void App::StopForceFocusWatcher() {
    // Set the stop flag THEN join. The watcher polls the flag between every
    // 100 ms sleep and around each ForceFocus iteration, so worst-case join
    // wait is bounded by one in-flight ForceFocusToGame call (~100 ms total:
    // two 50 ms sleeps plus the Win32 calls between AttachThreadInput TRUE
    // and FALSE). Joining is critical for shutdown safety — see App.h for the
    // freeze-on-quit failure mode this prevents.
    if (focus_watcher_stop_) {
        focus_watcher_stop_->store(true, std::memory_order_relaxed);
    }
    if (focus_watcher_thread_.joinable()) {
        focus_watcher_thread_.join();
    }
    focus_watcher_stop_.reset();
}

void App::EnterKatangaWaitingMode() {
    // Reverse the first-frame reveal: minimize the FSE popup, restore the
    // control panel, drop the focus watcher targeting the (now gone) game.
    // Leaves cap_, presenter_, and renderer_ alive so we can resume in place
    // when another producer publishes.
    presenter_.SetVisible(false);
    fse_visible_ = false;
    StopForceFocusWatcher();
    tracked_pid_ = 0;
    if (tracked_process_handle_) {
        CloseHandle(tracked_process_handle_);
        tracked_process_handle_ = nullptr;
    }
    ShowPanel();
    waiting_katanga_first_frame_ = true;
    status_extra_ = L"waiting for Katanga producer";
}

void App::ReregisterHotkeys() {
    hotkeys_.Clear();
    // Only the two user-facing hotkeys are bound. Quit + show/hide control
    // panel are still reachable through the tray icon and the X button on
    // the control panel itself — no global hotkey for them.
    settings_.hk_eyeswap.registered =
        hotkeys_.Register(kHKEyeSwap, settings_.hk_eyeswap.mods, settings_.hk_eyeswap.vk,
                          [this] { ToggleEyeSwap(); });
    settings_.hk_toggle_fse.registered =
        hotkeys_.Register(kHKToggleFse, settings_.hk_toggle_fse.mods, settings_.hk_toggle_fse.vk,
                          [this] { ToggleFseVisible(); });
}

void App::Tick() {
    if (state_ == AppState::Running) {
        bool staging_dirty = false;

        if (cap_) {
            // Capture mode. Drain WGC if a fresh frame is available; otherwise
            // leave the staging texture holding the last captured frame.
            ID3D11Texture2D* src = nullptr;
            UINT w = 0, h = 0;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            if (cap_->TryAcquire(&src, &w, &h, &fmt) && src) {
                // First Katanga frame: reveal the FSE popup AND hand focus
                // to the producer's game window. We never resize staging or
                // restart on dim differences — the renderer's scaler handles
                // every size (upscaling when the producer is below display
                // res, downscaling when above).
                //
                // Focus dance: NV3DLib's SW_RESTORE on the popup activates
                // it, which steals focus from whatever was foreground. For
                // WGC/DXGI modes we knew the game's HWND at Start and ran
                // the focus watcher from there; in Katanga the game lives in
                // a process we don't know about up front, so we infer it
                // from GetForegroundWindow at the moment of reveal (the
                // common path: user clicked Start on us, then alt-tabbed /
                // launched the game — game is foreground when its producer
                // first publishes a frame). HidePanel + focus watcher mirrors
                // the WGC path's hand-off so input keeps going to the game.
                if (waiting_katanga_first_frame_) {
                    Log(NV3D::LogLevel::Info,
                        L"App::Tick  first Katanga frame %ux%u (staging %ux%u) — revealing FSE",
                        w, h, renderer_.StagingWidth(), renderer_.StagingHeight());
                    waiting_katanga_first_frame_ = false;

                    HWND  prev_fg     = GetForegroundWindow();
                    DWORD prev_fg_pid = 0;
                    if (prev_fg) GetWindowThreadProcessId(prev_fg, &prev_fg_pid);
                    const bool game_was_foreground =
                        prev_fg && prev_fg_pid != 0 && prev_fg_pid != GetCurrentProcessId();

                    fse_visible_ = true;
                    presenter_.SetVisible(true);
                    status_extra_.clear();

                    if (game_was_foreground) {
                        Log(NV3D::LogLevel::Info,
                            L"App::Tick  Katanga reveal: handing focus to game pid=%lu",
                            (unsigned long)prev_fg_pid);
                        HidePanel();
                        // Pump so the panel hide's WM_KILLFOCUS lands before
                        // the watcher starts yanking foreground around — same
                        // sequencing the WGC path uses.
                        MSG pump;
                        for (int i = 0; i < 3; ++i) {
                            Sleep(10);
                            while (PeekMessageW(&pump, nullptr, 0, 0, PM_REMOVE)) {
                                TranslateMessage(&pump);
                                DispatchMessageW(&pump);
                            }
                        }
                        tracked_pid_ = prev_fg_pid;
                        StartForceFocusWatcher(tracked_pid_);
                        // Open a SYNCHRONIZE handle so Tick can detect game
                        // exit and auto-minimize. Geo-11 leaves the mapping
                        // slot pointed at a dead handle on exit, so the
                        // CaptureKatanga slot-grace timer alone won't catch
                        // an ungraceful producer death.
                        if (tracked_process_handle_) {
                            CloseHandle(tracked_process_handle_);
                            tracked_process_handle_ = nullptr;
                        }
                        tracked_process_handle_ = OpenProcess(SYNCHRONIZE, FALSE, prev_fg_pid);
                        if (!tracked_process_handle_) {
                            Log(NV3D::LogLevel::Warning,
                                L"App::Tick  OpenProcess(SYNCHRONIZE, pid=%lu) failed err=%lu — "
                                L"producer-death detection disabled this session",
                                (unsigned long)prev_fg_pid, GetLastError());
                        }
                    } else {
                        Log(NV3D::LogLevel::Info,
                            L"App::Tick  Katanga reveal: no game foreground detected — "
                            L"user will need to click the game window to give it focus");
                    }
                }
                const LONG rw = capture_src_region_.right  - capture_src_region_.left;
                const LONG rh = capture_src_region_.bottom - capture_src_region_.top;
                if (rw > 0 && rh > 0) {
                    renderer_.CopyCaptureRegionToStaging(src, capture_src_region_);
                } else {
                    renderer_.CopyCaptureToStaging(src);
                }
                last_src_w_   = w;
                last_src_h_   = h;
                last_src_fmt_ = (UINT)fmt;
                staging_dirty = true;
                src->Release();
            }
            if (cap_->IsLost()) {
                Stop();
                state_ = AppState::SourceLost;
                status_extra_ = L"source closed";
            } else if (cap_->IsDisconnected()) {
                // Katanga: producer went quiet past the grace window. Don't
                // tear the session down — fold the UI back to "waiting" and
                // let the same CaptureKatanga re-detect the next producer.
                Log(NV3D::LogLevel::Info,
                    L"App::Tick  Katanga producer disconnected — returning to waiting state");
                EnterKatangaWaitingMode();
                if (auto* kat = dynamic_cast<CaptureKatanga*>(cap_.get())) {
                    kat->ResetForReconnect();
                }
            } else if (tracked_process_handle_ &&
                       WaitForSingleObject(tracked_process_handle_, 0) == WAIT_OBJECT_0) {
                // Producer game process exited. The mapping slot may still
                // hold a stale handle (Geo-11 doesn't clear it on exit), so
                // CaptureKatanga can't detect this on its own — we have to.
                Log(NV3D::LogLevel::Info,
                    L"App::Tick  Katanga producer process exited — returning to waiting state");
                EnterKatangaWaitingMode();
                if (auto* kat = dynamic_cast<CaptureKatanga*>(cap_.get())) {
                    kat->ResetForReconnect();
                }
            }
        } else {
            // No capture session — animate the test pattern so the user can
            // see the present loop is alive and verify per-eye stereo
            // independently of any capture issues.
            renderer_.FillTestPattern(test_pattern_frame_++);
            staging_dirty = true;
        }

        // Present only when:
        //   * the staging texture actually changed this tick, OR
        //   * a heartbeat is due (keeps the 3D Vision driver from deciding
        //     we've stopped and dropping stereo if the captured source is
        //     ever idle for a while)
        //
        // Skip entirely while the FSE popup is hidden via Ctrl+F8 — some
        // drivers block PresentEx on an OCCLUDED device window and wedge
        // the main loop + GUI with it.
        //
        // The reason this matters: NV3DLib's worker takes ~16.67ms per
        // PresentEx (one vsync for the L+R pair at 120Hz frame-sequential).
        // At our 8ms tick rate, presenting every tick spam-submits to a
        // worker that drops anything that can't be queued within 8ms — and
        // each redundant Present is GPU work that competes with the captured
        // 30fps source. That competition starved the source of GPU time so
        // it couldn't hit 30fps, which read as "slow motion" downstream.
        const auto now = std::chrono::steady_clock::now();
        const bool heartbeat_due = (now - last_present_ts_) >= std::chrono::milliseconds(250);
        const bool want_present  = (staging_dirty || heartbeat_due) && fse_visible_;

        if (want_present && renderer_.Staging()) {
            presenter_.SubmitFrame(renderer_.Staging());
            fps_frames_++;
            last_present_ts_ = now;
        }
    }

    // FPS rolling average (1 Hz update)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - fps_t0_).count();
    if (elapsed >= 1.0f) {
        fps_ = fps_frames_ / elapsed;
        fps_t0_ = now;
        fps_frames_ = 0;
    }

    // Skip ImGui entirely while the control panel is minimized. We were
    // running BeginImGuiFrame → widget draws → ImGui_ImplDX11_RenderDrawData
    // → Present on the panel's swap chain every tick (so 120+ times per
    // second), all of which is GPU+CPU work for a window the user can't see.
    //
    // Use IsIconic(hwnd_) — i.e. the real Win32 minimized state — instead
    // of our cached panel_visible_ flag. The cached flag only tracks
    // explicit Show/HidePanel calls; it doesn't update when the user
    // un-minimizes the window from the taskbar, so we'd stay frozen on the
    // pre-HidePanel frame (which is why the button stayed on "Start" even
    // after capture had started and the user had restored the panel).
    const bool panel_minimized = hwnd_ && IsIconic(hwnd_);
    if (!panel_minimized) {
        renderer_.BeginImGuiFrame();
        if (gui_) gui_->Draw(*this);
        renderer_.EndImGuiFrame();
    }
}

LRESULT CALLBACK App::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Let ImGui see input first.
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        // ImGui consumed (typed text into a focused widget, etc.) — fall
        // through anyway for messages we still need to react to.
    }

    switch (msg) {
        case WM_SIZE: {
            UINT w = LOWORD(lp), h = HIWORD(lp);
            renderer_.HandleResize(w, h);
            return 0;
        }
        case WM_HOTKEY:
            hotkeys_.Dispatch(wp);
            return 0;
        case TrayIcon::kCallbackMsg:
            tray_.OnMessage(wp, lp);
            return 0;
        case WM_CLOSE:
            // X-button quits the app (and tears down the FSE popup via the
            // App::Shutdown → NV3DPresenter::Shutdown → NV3DLib teardown
            // chain). Earlier this minimized to the tray, but that left the
            // FSE popup live and surprised users who expected close-is-close.
            Quit();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace nv3dg
