#include "App.h"

#include "Capture.h"
#include "CaptureDXGI.h"
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
    ShowWindow(target, SW_RESTORE);
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
    Stop();
    PersistPanelGeometry();
    if (!ini_path_.empty()) SaveSettings(settings_, ini_path_.c_str());

    tray_.Remove();
    hotkeys_.Clear();
    gui_.reset();
    renderer_.Shutdown();

    if (hwnd_)          { DestroyWindow(hwnd_);                 hwnd_          = nullptr; }
    if (wndclass_atom_) { UnregisterClassW(kWndClass, hinstance_); wndclass_atom_ = 0;     }
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
        } else {
            cap_ = CaptureWGC::CreateForMonitor(renderer_.Device(), src_hmon);
            if (!cap_) {
                cap_ = CaptureDXGI::CreateForMonitor(renderer_.Device(), src_hmon);
                if (!cap_) capture_warning = L"WGC + DXGI both refused this monitor — showing test pattern";
            }
        }
    }

    UINT staging_w = 0, staging_h = 0;
    if (auto* wgc = dynamic_cast<CaptureWGC*>(cap_.get())) {
        staging_w = wgc->InitialWidth();
        staging_h = wgc->InitialHeight();
    }
    if (staging_w == 0 || staging_h == 0) {
        // Either test-pattern mode (no cap_) or DXGI fallback (no Initial* getter).
        // Default to a sensible SBS test-pattern size.
        staging_w = 2560;
        staging_h = 720;
    }

    if (!renderer_.CreateFixedStaging(staging_w, staging_h)) {
        if (cap_) { cap_->Stop(); cap_.reset(); }
        state_ = AppState::ErrorPresenter;
        status_extra_ = L"failed to allocate staging texture";
        return false;
    }
    renderer_.FillTestPattern(0);   // known-good stereo content for first frame

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
    fse_visible_   = true;              // NV3DLib's want_visible_ defaults to true; match
    fps_t0_        = std::chrono::steady_clock::now();
    fps_frames_    = 0;
    fps_           = 0.0f;
    test_pattern_frame_ = 0;
    tray_.SetStartStopLabel(L"Stop");

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
    if (kind != SourceKind::None) {
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

        if (game_hwnd && tracked_pid != 0 && tracked_pid != GetCurrentProcessId()) {
            // VRto3D's exact pattern (vrto3d/src/presenter/nvstereo_dx9_presenter.cpp:1303-1314):
            // the ForceFocus loop runs on its OWN detached thread, never on
            // the main thread. AttachThreadInput inside ForceFocus uses the
            // calling thread's id; running it on the main thread (with the
            // GUI message pump and existing input-thread state) is what made
            // the first SetForegroundWindow just "highlight" the game
            // without delivering input.
            const DWORD game_pid = tracked_pid;
            std::thread([game_pid]() {
                for (int i = 0; i < 16; ++i) {
                    HWND target = nullptr;
                    struct Ctx { DWORD pid; HWND result; } c{ game_pid, nullptr };
                    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                        auto* cc = reinterpret_cast<Ctx*>(lp);
                        DWORD pid = 0;
                        GetWindowThreadProcessId(h, &pid);
                        if (pid == cc->pid && IsWindowVisible(h) && GetWindowTextLengthW(h) > 0) {
                            cc->result = h;
                            return FALSE;
                        }
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&c));
                    target = c.result;
                    if (target && GetForegroundWindow() != target) {
                        ForceFocusToGame(target);
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }).detach();
        }
    }

    return true;
}

void App::Stop() {
    if (cap_) { cap_->Stop(); cap_.reset(); }
    presenter_.Shutdown();
    renderer_.ReleaseStaging();
    if (state_ == AppState::Running) state_ = AppState::Idle;
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
    // Mirrors VRto3D's Ctrl+F8 → flip is_on_top_/man_on_top_ pattern. We just
    // flip an atomic NV3DLib reads on its window thread; that thread handles
    // suppress_minimize_ sequencing + SW_MINIMIZE/SW_RESTORE itself. Doing
    // ShowWindow cross-thread on a FSE D3D9Ex window wedges DWM and leaves
    // our control panel non-responsive — which is why the older Ctrl+Alt+H
    // path went white-screen.
    if (state_ != AppState::Running) return;
    fse_visible_ = !fse_visible_;
    presenter_.SetVisible(fse_visible_);
    status_extra_ = fse_visible_ ? L"" : L"FSE minimized (Ctrl+F8 to restore)";
}

void App::Quit() {
    running_ = false;
    PostQuitMessage(0);
}

void App::RefreshSources() {
    if (gui_) gui_->RefreshSources();
}

void App::ReregisterHotkeys() {
    hotkeys_.Clear();
    settings_.hk_eyeswap.registered =
        hotkeys_.Register(kHKEyeSwap, settings_.hk_eyeswap.mods, settings_.hk_eyeswap.vk,
                          [this] { ToggleEyeSwap(); });
    settings_.hk_toggle_panel.registered =
        hotkeys_.Register(kHKTogglePanel, settings_.hk_toggle_panel.mods, settings_.hk_toggle_panel.vk,
                          [this] { TogglePanelVisible(); });
    settings_.hk_toggle_fse.registered =
        hotkeys_.Register(kHKToggleFse, settings_.hk_toggle_fse.mods, settings_.hk_toggle_fse.vk,
                          [this] { ToggleFseVisible(); });
    settings_.hk_quit.registered =
        hotkeys_.Register(kHKQuit, settings_.hk_quit.mods, settings_.hk_quit.vk,
                          [this] { Quit(); });
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
                renderer_.CopyCaptureToStaging(src);
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
    // For 30fps WGC capture that contention is enough to drag the source's
    // render rate down into "slow motion" territory.
    if (panel_visible_) {
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
            // Hide to tray rather than quit, per plan.
            HidePanel();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace nv3dg
