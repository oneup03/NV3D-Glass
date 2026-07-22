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
    // D3D9Ex device window wedges DWM). Idempotent - calling with the current
    // state is a no-op.
    void SetVisible(bool visible);

    // Live eye-swap toggle. Updates the NV3D signature row on the next frame
    // - driver picks up the new routing on the following PresentEx with no
    // teardown / re-Init / visible glitch. Safe to call mid-session at any
    // time. AUTOMATIC-mode (NV3D signature) path only - the legacy DIRECT
    // path that used SetActiveEye is no longer in the build.
    void SetEyeSwap(bool enable);

    // Tell NV3DLib the D3D11 device (and therefore the whole adapter) is
    // lost, BEFORE Shutdown(). Without this the library's D3D9 side never
    // observes the failure (a hidden popup means no D3D9 call runs) and its
    // teardown takes the live path - Stereo_DestroyHandle + COM Release into
    // a wedged kernel driver, both of which can block indefinitely.
    void NotifyDeviceLost();

    // Tear down and rebuild against new Settings. Used when init-only params
    // change (eye_swap, target_monitor, on_top, lightboost, suppressor).
    bool RecreateWith(ID3D11Device* dev, const Settings& s, DWORD tracked_game_pid);

    bool IsActive() const { return iface_ != nullptr; }

private:
    NV3D::InterfaceDX11* iface_   = nullptr;
    ID3D11Texture2D*     last_tex_ = nullptr;  // pointer-identity cache, unowned
};

}
