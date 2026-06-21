#pragma once

#include <Windows.h>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace NV3D { class InterfaceDX11; }

namespace nv3dg {

struct Settings;

class NV3DPresenter {
public:
    NV3DPresenter() = default;
    ~NV3DPresenter();

    NV3DPresenter(const NV3DPresenter&)            = delete;
    NV3DPresenter& operator=(const NV3DPresenter&) = delete;

    // Build NV3D::InitParams from `s` and call NV3D::CreateInterfaceDX11.
    // The library owns the FSE click-through window (host_hwnd=nullptr) and
    // pumps its own message loop on a private thread.
    //
    // tracked_game_pid: PID of the captured game/app. Non-zero enables
    // NV3DLib's VRto3D-style focus loop (BringToTop + ForceFocus to that PID's
    // main window). Pass 0 for legacy minimize-on-focus-loss behavior.
    bool Init(ID3D11Device* dev, const Settings& s, DWORD tracked_game_pid);

    void Shutdown();

    // Hand the SbS staging texture (2W x H BGRA8 MISC_SHARED) to NV3DLib and
    // present one stereo frame. Returns the HRESULT from NV3DLib::Present.
    HRESULT SubmitFrame(ID3D11Texture2D* sbs);

    // Toggle the FSE popup's visibility. The actual SW_MINIMIZE / SW_RESTORE
    // is done on NV3DLib's window thread (cross-thread ShowWindow on a FSE
    // D3D9Ex device window wedges DWM). Idempotent — calling with the current
    // state is a no-op.
    void SetVisible(bool visible);

    // Tear down and rebuild against new Settings. Used when init-only params
    // change (eye_swap, target_monitor, on_top, lightboost, suppressor).
    bool RecreateWith(ID3D11Device* dev, const Settings& s, DWORD tracked_game_pid);

    bool IsActive() const { return iface_ != nullptr; }

private:
    NV3D::InterfaceDX11* iface_   = nullptr;
    ID3D11Texture2D*     last_tex_ = nullptr;  // pointer-identity cache, unowned
};

}
