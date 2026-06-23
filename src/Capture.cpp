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

#include "Capture.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// C++/WinRT headers (Win10 SDK).
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>

using Microsoft::WRL::ComPtr;

namespace winrt {
    using namespace Windows::Foundation;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace nv3dg {

struct CaptureWGC::Impl {
    winrt::IDirect3DDevice                  wrt_device{nullptr};
    winrt::GraphicsCaptureItem              item{nullptr};
    winrt::Direct3D11CaptureFramePool       pool{nullptr};
    winrt::GraphicsCaptureSession           session{nullptr};
    winrt::event_token                      closed_token{};
    std::atomic<bool>                       lost{false};
    winrt::SizeInt32                        size{0, 0};
    // Sub-rect of the captured frame that corresponds to the window's client
    // area (i.e. with the title bar / borders / DWM shadow stripped).
    // Empty / full-frame for monitor sources.
    RECT                                    client_area{0, 0, 0, 0};
};

namespace {

winrt::IDirect3DDevice MakeWinRTDevice(ID3D11Device* dev) {
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) return nullptr;
    winrt::com_ptr<::IInspectable> insp;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()))) return nullptr;
    return insp.as<winrt::IDirect3DDevice>();
}

bool BuildSession(CaptureWGC::Impl& impl, ID3D11Device* dev) {
    impl.wrt_device = MakeWinRTDevice(dev);
    if (!impl.wrt_device) return false;
    try {
        impl.pool = winrt::Direct3D11CaptureFramePool::Create(
            impl.wrt_device,
            winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            impl.item.Size());
        impl.size = impl.item.Size();
        impl.closed_token = impl.item.Closed(
            [&impl](winrt::GraphicsCaptureItem const&, winrt::IInspectable const&) {
                impl.lost.store(true);
            });
        impl.session = impl.pool.CreateCaptureSession(impl.item);
        try { impl.session.IsCursorCaptureEnabled(false); } catch (...) {}
        // Win11 22H2+ only — older OS throws, ignore.
        try { impl.session.IsBorderRequired(false);       } catch (...) {}
        // Lift the WGC ~60fps cap. Without an explicit MinUpdateInterval,
        // WGC delivers frames at roughly display-composition rate (60Hz)
        // regardless of the source's actual update rate — which manifests as
        // "slow motion" when capturing higher-rate sources, and as starving
        // 30fps content of GPU time when our consumer keeps polling at 125Hz
        // expecting fresh frames that never come. 1ms tracks 120/144/240Hz
        // sources without saturating the GPU. Same fix used by Sunshine
        // (PR #4424), Apollo (#676), and SR-Loom (Capture.cpp:179).
        try { impl.session.MinUpdateInterval(std::chrono::milliseconds(1)); } catch (...) {}
        impl.session.StartCapture();
    } catch (winrt::hresult_error const&) {
        return false;
    } catch (...) {
        return false;
    }
    return true;
}

}  // anonymous

CaptureWGC::CaptureWGC()  = default;
CaptureWGC::~CaptureWGC() { Stop(); }

std::unique_ptr<CaptureWGC> CaptureWGC::CreateForWindow(ID3D11Device* dev, HWND hwnd) {
    if (!dev || !hwnd || !IsWindow(hwnd)) return nullptr;
    if (!winrt::GraphicsCaptureSession::IsSupported()) return nullptr;
    auto out = std::make_unique<CaptureWGC>();
    out->impl_ = std::make_unique<Impl>();
    auto factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    HRESULT hr = factory->CreateForWindow(
        hwnd, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(out->impl_->item));
    if (FAILED(hr) || !out->impl_->item) return nullptr;
    if (!BuildSession(*out->impl_, dev))  return nullptr;

    // Compute the client area's position within the captured frame so callers
    // can crop out the title bar / borders / DWM shadow at staging-write time.
    // WGC captures the FULL window as DWM sees it (including the shadow on
    // Win10+), so we anchor on the window's outer rect and find the client
    // origin in screen space relative to that.
    RECT outer{};
    if (GetWindowRect(hwnd, &outer)) {
        RECT cr{};
        POINT client_origin{ 0, 0 };
        if (GetClientRect(hwnd, &cr) && ClientToScreen(hwnd, &client_origin)) {
            const LONG ox = client_origin.x - outer.left;
            const LONG oy = client_origin.y - outer.top;
            const LONG cw = cr.right  - cr.left;
            const LONG ch = cr.bottom - cr.top;
            const LONG fw = out->impl_->size.Width;
            const LONG fh = out->impl_->size.Height;
            // Clamp into the captured frame in case WGC's size and
            // GetWindowRect disagree (they can by a pixel or two).
            const LONG l = (ox < 0) ? 0 : (ox > fw ? fw : ox);
            const LONG t = (oy < 0) ? 0 : (oy > fh ? fh : oy);
            const LONG r = ((ox + cw) > fw) ? fw : (ox + cw);
            const LONG b = ((oy + ch) > fh) ? fh : (oy + ch);
            if (r > l && b > t) {
                out->impl_->client_area = RECT{ l, t, r, b };
            }
        }
    }
    return out;
}

std::unique_ptr<CaptureWGC> CaptureWGC::CreateForMonitor(ID3D11Device* dev, HMONITOR hmon) {
    if (!dev || !hmon) return nullptr;
    if (!winrt::GraphicsCaptureSession::IsSupported()) return nullptr;
    auto out = std::make_unique<CaptureWGC>();
    out->impl_ = std::make_unique<Impl>();
    auto factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    HRESULT hr = factory->CreateForMonitor(
        hmon, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(out->impl_->item));
    if (FAILED(hr) || !out->impl_->item) return nullptr;
    if (!BuildSession(*out->impl_, dev))  return nullptr;
    return out;
}

bool CaptureWGC::TryAcquire(ID3D11Texture2D** out, UINT* w, UINT* h, DXGI_FORMAT* fmt) {
    if (!impl_ || !impl_->pool || impl_->lost.load()) return false;
    winrt::Direct3D11CaptureFrame frame{nullptr};
    try {
        frame = impl_->pool.TryGetNextFrame();
    } catch (...) {
        impl_->lost.store(true);
        return false;
    }
    if (!frame) return false;

    // Drain to the NEWEST queued frame so we never present stale content.
    // WGC's FramePool buffers frames the source produces between our polls;
    // if our tick falls behind (GPU contention, scheduler hiccup), older
    // frames stay queued and the consumer sees a growing latency tail —
    // which downstream reads as "slow motion" on real-time sources. Same
    // pattern as SR-Loom (Capture.cpp:208-214).
    try {
        for (;;) {
            auto next = impl_->pool.TryGetNextFrame();
            if (!next) break;
            frame.Close();
            frame = next;
        }
    } catch (...) {
        impl_->lost.store(true);
        return false;
    }

    // Source dim changes (e.g. window resize): recreate the pool at new size.
    auto cs = frame.ContentSize();
    if (cs.Width != impl_->size.Width || cs.Height != impl_->size.Height) {
        impl_->size = cs;
        try {
            impl_->pool.Recreate(
                impl_->wrt_device,
                winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2, cs);
        } catch (...) {
            impl_->lost.store(true);
            return false;
        }
    }

    auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex))) || !tex) {
        return false;
    }
    if (w)   *w   = static_cast<UINT>(cs.Width);
    if (h)   *h   = static_cast<UINT>(cs.Height);
    if (fmt) *fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    *out = tex.Detach();
    return true;
}

bool CaptureWGC::IsLost() const {
    return !impl_ || impl_->lost.load();
}

UINT CaptureWGC::InitialWidth() const {
    return (impl_ ? static_cast<UINT>(impl_->size.Width)  : 0u);
}

UINT CaptureWGC::InitialHeight() const {
    return (impl_ ? static_cast<UINT>(impl_->size.Height) : 0u);
}

RECT CaptureWGC::ClientAreaInCapture() const {
    if (!impl_) return RECT{0, 0, 0, 0};
    const RECT& c = impl_->client_area;
    if (c.right > c.left && c.bottom > c.top) return c;
    // Fall back to the full captured frame (monitor sources, or window
    // sources where client-area detection didn't produce a sane rect).
    return RECT{ 0, 0, impl_->size.Width, impl_->size.Height };
}

void CaptureWGC::Stop() {
    if (!impl_) return;
    try {
        if (impl_->item && impl_->closed_token) impl_->item.Closed(impl_->closed_token);
    } catch (...) {}
    try { if (impl_->session) impl_->session.Close(); } catch (...) {}
    try { if (impl_->pool)    impl_->pool.Close();    } catch (...) {}
    impl_.reset();
}

}  // namespace nv3dg
