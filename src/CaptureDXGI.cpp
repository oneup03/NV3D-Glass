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

#include "CaptureDXGI.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>

using Microsoft::WRL::ComPtr;

namespace nv3dg {

struct CaptureDXGI::Impl {
    ComPtr<ID3D11Device>           dev;
    ComPtr<IDXGIOutputDuplication> dup;
    HMONITOR                       hmon = nullptr;
    std::atomic<bool>              lost{false};
    bool                           frame_held = false;
};

namespace {

ComPtr<IDXGIOutput1> FindOutputForMonitor(ID3D11Device* dev, HMONITOR hmon) {
    if (!dev || !hmon) return nullptr;
    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) return nullptr;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_dev->GetAdapter(&adapter))) return nullptr;

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> out;
        if (adapter->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC d{};
        if (SUCCEEDED(out->GetDesc(&d)) && d.Monitor == hmon) {
            ComPtr<IDXGIOutput1> out1;
            if (SUCCEEDED(out.As(&out1))) return out1;
        }
    }
    return nullptr;
}

bool StartDuplication(CaptureDXGI::Impl& impl) {
    auto out = FindOutputForMonitor(impl.dev.Get(), impl.hmon);
    if (!out) return false;
    impl.dup.Reset();
    HRESULT hr = out->DuplicateOutput(impl.dev.Get(), &impl.dup);
    return SUCCEEDED(hr) && impl.dup;
}

}  // anonymous

CaptureDXGI::CaptureDXGI()  = default;
CaptureDXGI::~CaptureDXGI() { Stop(); }

std::unique_ptr<CaptureDXGI> CaptureDXGI::CreateForMonitor(ID3D11Device* dev, HMONITOR hmon) {
    if (!dev || !hmon) return nullptr;
    auto out = std::make_unique<CaptureDXGI>();
    out->impl_      = std::make_unique<Impl>();
    out->impl_->dev = dev;
    out->impl_->hmon = hmon;
    if (!StartDuplication(*out->impl_)) return nullptr;
    return out;
}

bool CaptureDXGI::TryAcquire(ID3D11Texture2D** out, UINT* w, UINT* h, DXGI_FORMAT* fmt) {
    if (!impl_ || !impl_->dup || impl_->lost.load()) return false;
    if (impl_->frame_held) {
        impl_->dup->ReleaseFrame();
        impl_->frame_held = false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource>   res;
    HRESULT hr = impl_->dup->AcquireNextFrame(0, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Try to re-duplicate once; if it fails, mark lost.
        if (!StartDuplication(*impl_)) impl_->lost.store(true);
        return false;
    }
    if (FAILED(hr) || !res) {
        impl_->lost.store(true);
        return false;
    }
    impl_->frame_held = true;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex)) || !tex) return false;

    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (w)   *w   = d.Width;
    if (h)   *h   = d.Height;
    if (fmt) *fmt = d.Format;   // typically B8G8R8A8_UNORM
    *out = tex.Detach();
    return true;
}

bool CaptureDXGI::IsLost() const { return !impl_ || impl_->lost.load(); }

void CaptureDXGI::Stop() {
    if (!impl_) return;
    if (impl_->frame_held && impl_->dup) impl_->dup->ReleaseFrame();
    impl_->frame_held = false;
    impl_->dup.Reset();
    impl_->dev.Reset();
    impl_.reset();
}

}  // namespace nv3dg
