#include "NV3DPresenter.h"
#include "Logging.h"
#include "Settings.h"

#include "NV3D.hpp"

#include <Windows.h>

namespace nv3dg {

namespace {

// NV3DLib's library-owned FSE popup uses this exact class name. We use it to
// (a) verify the popup actually got the click-through ex-styles after Init,
// and (b) re-assert them ourselves if for some reason NV3DLib didn't (the
// only path that skips ApplyClickThrough is the windowed-fallback when
// CreateDeviceEx FSE refuses — see d3d9_presenter.cpp:261-272).
constexpr const wchar_t* kNV3DLibClass = L"NV3DLib_PresentWindow";

HWND FindNV3DLibFseWindow() {
    return FindWindowW(kNV3DLibClass, nullptr);
}

void DiagnoseClickThrough() {
    HWND hwnd = FindNV3DLibFseWindow();
    if (!hwnd) {
        Log(NV3D::LogLevel::Warning,
            L"NV3DPresenter: could not locate NV3DLib FSE window (class=%s) — "
            L"NV3DLib never created it, or it's on a different desktop",
            kNV3DLibClass);
        return;
    }

    // Only report state; we no longer re-apply WS_EX_LAYERED|WS_EX_TRANSPARENT
    // from here. Those styles break the D3D9Ex FSE scan-out (DWM composites
    // the layered surface, ignoring the FSE back buffer). NV3DLib now does
    // click-through via WM_NCHITTEST→HTTRANSPARENT inside its WndProc subclass.
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    Log(NV3D::LogLevel::Info,
        L"NV3DPresenter: FSE hwnd=%p exstyle=0x%08lX  "
        L"LAYERED=%s TRANSPARENT=%s TOPMOST=%s",
        (void*)hwnd,
        static_cast<unsigned long>(ex),
        (ex & WS_EX_LAYERED)     ? L"yes" : L"no",
        (ex & WS_EX_TRANSPARENT) ? L"yes" : L"no",
        (ex & WS_EX_TOPMOST)     ? L"yes" : L"no");
}

}  // anonymous

NV3DPresenter::~NV3DPresenter() {
    Shutdown();
}

bool NV3DPresenter::Init(ID3D11Device* dev, const Settings& s, DWORD tracked_game_pid) {
    if (iface_) return true;
    if (!dev)   return false;

    NV3D::InitParams p{};
    p.target_monitor          = s.target_monitor;
    p.eye_swap                = s.eye_swap;
    p.on_top                  = s.on_top;
    p.enable_lightboost       = s.enable_lightboost;
    p.enable_suppressor       = s.enable_suppressor;
    p.host_hwnd               = nullptr;   // library-owned FSE click-through window
    p.activation_retry_budget = 60;
    p.tracked_game_pid        = tracked_game_pid;

    Log(NV3D::LogLevel::Info,
        L"NV3DPresenter::Init target_monitor=%p eye_swap=%d on_top=%d "
        L"lightboost=%d suppressor=%d tracked_game_pid=%lu",
        (void*)s.target_monitor,
        (int)s.eye_swap, (int)s.on_top,
        (int)s.enable_lightboost, (int)s.enable_suppressor,
        static_cast<unsigned long>(tracked_game_pid));

    HRESULT hr = NV3D::CreateInterfaceDX11(dev, &p, &iface_);
    if (FAILED(hr) || !iface_) {
        Log(NV3D::LogLevel::Error,
            L"NV3DPresenter: CreateInterfaceDX11 hr=0x%08lX iface=%p",
            static_cast<unsigned long>(hr), (void*)iface_);
        iface_ = nullptr;
        return false;
    }

    DiagnoseClickThrough();
    Log(NV3D::LogLevel::Info,
        L"NV3DPresenter: Init complete  iface=%p IsActive=true", (void*)iface_);

    last_tex_ = nullptr;
    return true;
}

void NV3DPresenter::Shutdown() {
    if (iface_) {
        Log(NV3D::LogLevel::Info,
            L"NV3DPresenter: Shutdown  iface=%p -> nullptr", (void*)iface_);
        iface_->Delete();
        iface_ = nullptr;
    }
    last_tex_ = nullptr;
}

HRESULT NV3DPresenter::SubmitFrame(ID3D11Texture2D* sbs) {
    if (!iface_ || !sbs) return E_FAIL;
    if (sbs != last_tex_) {
        HRESULT hr = iface_->SetInputTexture(sbs);
        if (FAILED(hr)) return hr;
        last_tex_ = sbs;
    }
    return iface_->Present();
}

void NV3DPresenter::SetVisible(bool visible) {
    if (iface_) iface_->SetVisible(visible);
}

bool NV3DPresenter::RecreateWith(ID3D11Device* dev, const Settings& s, DWORD tracked_game_pid) {
    Shutdown();
    return Init(dev, s, tracked_game_pid);
}

}
