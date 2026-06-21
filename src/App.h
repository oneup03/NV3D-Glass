#pragma once

#include "Settings.h"
#include "Renderer.h"
#include "NV3DPresenter.h"
#include "HotkeyManager.h"
#include "TrayIcon.h"

#include <Windows.h>
#include <chrono>
#include <memory>
#include <string>

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
    HotkeyManager                      hotkeys_;
    TrayIcon                           tray_;
    std::unique_ptr<Gui>               gui_;
    std::unique_ptr<ICaptureSource>    cap_;

    // Telemetry / status
    UINT                               last_src_w_ = 0;
    UINT                               last_src_h_ = 0;
    UINT                               last_src_fmt_ = 0;
    std::chrono::steady_clock::time_point fps_t0_{};
    int                                fps_frames_ = 0;
    float                              fps_       = 0.0f;
    uint32_t                           test_pattern_frame_ = 0;
    std::chrono::steady_clock::time_point last_present_ts_{};
    std::wstring                       status_extra_;
};

}  // namespace nv3dg
