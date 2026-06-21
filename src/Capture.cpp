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
