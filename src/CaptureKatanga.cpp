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

#include "CaptureKatanga.h"

#include "Logging.h"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace nv3dg {

namespace {

// Established by Katanga's legacy DeviarePlugin host; reused by Geo-11 and the
// 3DVision4All proxy. VRScreenCap consumes the same name (see
// ../VRScreenCap-main/src/loaders/katanga_loader.rs:42).
constexpr const char* kMappingName = "Local\\KatangaMappedFile";

}  // anonymous

struct CaptureKatanga::Impl {
    ComPtr<ID3D11Device>            device;
    ComPtr<ID3D11Texture2D>         shared_tex;
    HANDLE                          mapping_file = nullptr;
    void*                           mapping_view = nullptr;
    uintptr_t                       current_handle = 0;
    UINT                            width  = 0;
    UINT                            height = 0;
    DXGI_FORMAT                     format = DXGI_FORMAT_UNKNOWN;
    std::atomic<bool>               lost{false};          // unrecoverable: D3D11 device removed
    std::atomic<bool>               disconnected{false};  // recoverable: producer exited / grace expired
    // Grace tracking for transient producer-side rebuilds. A resolution change
    // in Geo-11 (and others) can briefly zero the handle slot or publish a
    // handle that OpenSharedResource rejects while the new texture is still
    // being initialized. Don't tip into "disconnected" on a single bad poll;
    // only do so if the bad state persists past kBadStateGrace.
    std::chrono::steady_clock::time_point bad_state_since{};
};

namespace {

constexpr auto kBadStateGrace = std::chrono::seconds(3);

// Reads the handle value the producer last wrote. The slot is 8 bytes aligned
// on x64, so the load is naturally atomic; no locking needed.
uintptr_t ReadMappingHandle(void* view) {
    return *static_cast<volatile uintptr_t*>(view);
}

bool BadStateExpired(const CaptureKatanga::Impl& impl) {
    if (impl.bad_state_since == std::chrono::steady_clock::time_point{}) return false;
    return (std::chrono::steady_clock::now() - impl.bad_state_since) > kBadStateGrace;
}

void MarkBadState(CaptureKatanga::Impl& impl) {
    if (impl.bad_state_since == std::chrono::steady_clock::time_point{}) {
        impl.bad_state_since = std::chrono::steady_clock::now();
    }
}

void ClearBadState(CaptureKatanga::Impl& impl) {
    impl.bad_state_since = {};
}

// CPU-side wait until the GPU has consumed every command submitted so far.
// We need this before releasing an old shared import on handle rotation —
// the producer's KMT-shared texture has no cross-process refcount, so it
// may free the underlying GPU memory at any moment after writing a new
// handle. If our previously queued CopyResource(staging, old) is still
// executing when that memory disappears, we get a TDR that takes down our
// D3D11 device (and NV3DLib's D3D9 view of our staging with it).
void DrainGpu(ID3D11Device* dev) {
    if (!dev) return;
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> q;
    if (FAILED(dev->CreateQuery(&qd, &q))) return;
    ctx->End(q.Get());
    ctx->Flush();   // kick the work, including the queued CopyResource we want to land
    for (int i = 0; i < 100; ++i) {        // bounded: ~100ms ceiling
        BOOL done = FALSE;
        HRESULT hr = ctx->GetData(q.Get(), &done, sizeof(done), 0);
        if (hr == S_OK && done) return;
        Sleep(1);
    }
    Log(NV3D::LogLevel::Warning,
        L"CaptureKatanga: DrainGpu timeout after 100ms — proceeding anyway");
}

bool ImportHandle(CaptureKatanga::Impl& impl, uintptr_t handle_value) {
    if (handle_value == 0) return false;
    ComPtr<ID3D11Resource> res;
    HRESULT hr = impl.device->OpenSharedResource(
        reinterpret_cast<HANDLE>(handle_value),
        IID_PPV_ARGS(&res));
    if (FAILED(hr) || !res) {
        Log(NV3D::LogLevel::Warning,
            L"CaptureKatanga: OpenSharedResource failed hr=0x%08X handle=%p",
            (unsigned)hr, reinterpret_cast<void*>(handle_value));
        return false;
    }
    ComPtr<ID3D11Texture2D> tex;
    hr = res.As(&tex);
    if (FAILED(hr) || !tex) {
        Log(NV3D::LogLevel::Warning,
            L"CaptureKatanga: shared resource is not ID3D11Texture2D hr=0x%08X",
            (unsigned)hr);
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    impl.shared_tex     = std::move(tex);
    impl.current_handle = handle_value;
    impl.width          = desc.Width;
    impl.height         = desc.Height;
    impl.format         = desc.Format;
    Log(NV3D::LogLevel::Info,
        L"CaptureKatanga: imported %ux%u fmt=%u handle=%p",
        desc.Width, desc.Height, (unsigned)desc.Format,
        reinterpret_cast<void*>(handle_value));
    return true;
}

}  // anonymous

CaptureKatanga::CaptureKatanga()  = default;
CaptureKatanga::~CaptureKatanga() { Stop(); }

std::unique_ptr<CaptureKatanga> CaptureKatanga::Create(ID3D11Device* dev) {
    if (!dev) return nullptr;

    // Two real-world contracts use Local\KatangaMappedFile:
    //   * producer-creates: Geo-11 etc. create the mapping when injected; the
    //     consumer (us, VRScreenCap, ...) opens it. We must wait for the
    //     producer if it isn't running yet.
    //   * consumer-creates: the legacy Katanga / Unity host created the
    //     mapping at startup; the Deviare-injected game opened it and wrote
    //     the handle when it had one.
    // Support both: try to open first; if that fails, create our own zeroed
    // mapping so a producer that arrives later can open and populate it.
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
    bool we_created = false;
    if (!map) {
        DWORD open_err = GetLastError();
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = FALSE;
        sa.lpSecurityDescriptor = nullptr;   // default DACL — same-session producers see it
        map = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                 0, sizeof(uintptr_t), kMappingName);
        if (!map) {
            Log(NV3D::LogLevel::Warning,
                L"CaptureKatanga: open failed err=%lu and create failed err=%lu",
                open_err, GetLastError());
            return nullptr;
        }
        we_created = true;
        Log(NV3D::LogLevel::Info,
            L"CaptureKatanga: open failed err=%lu — created mapping %S, waiting for producer",
            open_err, kMappingName);
    }
    void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(uintptr_t));
    if (!view) {
        Log(NV3D::LogLevel::Warning,
            L"CaptureKatanga: MapViewOfFile failed err=%lu", GetLastError());
        CloseHandle(map);
        return nullptr;
    }
    if (we_created) {
        *static_cast<volatile uintptr_t*>(view) = 0;   // explicit zero so the slot starts clean
    }

    auto out = std::make_unique<CaptureKatanga>();
    out->impl_ = std::make_unique<Impl>();
    out->impl_->device       = dev;
    out->impl_->mapping_file = map;
    out->impl_->mapping_view = view;

    // Best-effort initial import. If the slot is empty or the producer hasn't
    // published a usable handle yet, stay in "waiting" state — TryAcquire will
    // poll and import as soon as a non-zero handle appears.
    uintptr_t initial = ReadMappingHandle(view);
    if (initial != 0) {
        if (!ImportHandle(*out->impl_, initial)) {
            Log(NV3D::LogLevel::Info,
                L"CaptureKatanga: initial import deferred (handle=%p will retry)",
                reinterpret_cast<void*>(initial));
        }
    } else {
        Log(NV3D::LogLevel::Info,
            L"CaptureKatanga: mapping open, slot empty — waiting for producer");
    }

    return out;
}

bool CaptureKatanga::TryAcquire(ID3D11Texture2D** out, UINT* w, UINT* h, DXGI_FORMAT* fmt) {
    if (!impl_ || impl_->lost.load() || impl_->disconnected.load()) return false;
    if (!impl_->mapping_view)         return false;

    // Producer process dying after we've imported its texture sometimes
    // takes our D3D11 device with it (the kernel-side KMT object is no
    // longer backed by valid GPU memory; the next Draw / CopyResource trips
    // a TDR). Detect that here and surface as Lost so the host stops
    // submitting frames instead of repainting our control panel white.
    if (impl_->current_handle != 0) {
        HRESULT removed = impl_->device->GetDeviceRemovedReason();
        if (removed != S_OK) {
            Log(NV3D::LogLevel::Warning,
                L"CaptureKatanga: D3D11 device removed (hr=0x%08X) — producer likely died",
                (unsigned)removed);
            impl_->lost.store(true);
            return false;
        }
    }

    uintptr_t now = ReadMappingHandle(impl_->mapping_view);

    if (now == 0) {
        // Slot zeroed:
        //  * never connected yet (waiting): just no frame, not disconnected.
        //  * connected once and producer torn down OR mid-rebuild: start the
        //    grace timer; mark disconnected (soft / recoverable) if the slot
        //    stays zero past kBadStateGrace.
        if (impl_->current_handle != 0) {
            MarkBadState(*impl_);
            if (BadStateExpired(*impl_)) {
                Log(NV3D::LogLevel::Info,
                    L"CaptureKatanga: slot zero for >3s — disconnected");
                impl_->disconnected.store(true);
            }
        }
        return false;
    }

    if (now != impl_->current_handle) {
        // First-ever import OR producer rebuilt its staging.
        // 1. Drain the GPU before releasing the old import so any queued
        //    CopyResource against it lands while the producer's backing
        //    memory is still mapped. Skip the drain on the first import
        //    (no prior shared_tex to protect).
        // 2. Reset and import the new handle.
        // 3. For rotations (not first import) skip emitting a frame this
        //    tick — Geo-11 (and likely others) publish the new handle
        //    *before* finishing init on the new texture, so reading from it
        //    immediately wedges the GPU (manifests as NV3DLib's sync_query
        //    timing out 500ms later). Letting one tick (~8ms) elapse before
        //    we touch the new texture lets the producer finish init.
        const bool is_rotation = (impl_->shared_tex != nullptr);
        if (is_rotation) {
            DrainGpu(impl_->device.Get());
        }
        impl_->shared_tex.Reset();
        if (!ImportHandle(*impl_, now)) {
            // The producer published a new non-zero handle but
            // OpenSharedResource refused it — most likely the new texture
            // isn't fully initialized yet. If we never had a handle, stay in
            // waiting state. If we previously had one (mid-session rebuild),
            // start the grace timer; only give up after kBadStateGrace.
            if (impl_->current_handle != 0) {
                MarkBadState(*impl_);
                if (BadStateExpired(*impl_)) {
                    Log(NV3D::LogLevel::Info,
                        L"CaptureKatanga: import failing for >3s — disconnected");
                    impl_->disconnected.store(true);
                }
            }
            return false;
        }
        ClearBadState(*impl_);
        if (is_rotation) {
            Log(NV3D::LogLevel::Info,
                L"CaptureKatanga: rotation absorbed — skipping one tick to let producer init");
            return false;
        }
    }

    if (!impl_->shared_tex) return false;
    ClearBadState(*impl_);   // we have a current good import

    *out = impl_->shared_tex.Get();
    (*out)->AddRef();
    if (w)   *w   = impl_->width;
    if (h)   *h   = impl_->height;
    if (fmt) *fmt = impl_->format;
    return true;
}

bool CaptureKatanga::IsLost() const {
    return !impl_ || impl_->lost.load();
}

bool CaptureKatanga::IsDisconnected() const {
    return impl_ && impl_->disconnected.load();
}

void CaptureKatanga::ResetForReconnect() {
    if (!impl_) return;
    // Recoverable transition: clear the soft-disconnect state and the cached
    // import so the next TryAcquire treats whatever shows up in the slot as a
    // fresh first import. We deliberately keep the mapping open and don't
    // touch `lost` — that's reserved for unrecoverable TDR / device-removed.
    impl_->shared_tex.Reset();
    impl_->current_handle = 0;
    impl_->bad_state_since = {};
    impl_->disconnected.store(false);
    Log(NV3D::LogLevel::Info, L"CaptureKatanga: reset for reconnect");
}

void CaptureKatanga::Stop() {
    if (!impl_) return;
    impl_->shared_tex.Reset();
    if (impl_->mapping_view) {
        UnmapViewOfFile(impl_->mapping_view);
        impl_->mapping_view = nullptr;
    }
    if (impl_->mapping_file) {
        CloseHandle(impl_->mapping_file);
        impl_->mapping_file = nullptr;
    }
    impl_->device.Reset();
    impl_->current_handle = 0;
    impl_->width = impl_->height = 0;
    impl_->format = DXGI_FORMAT_UNKNOWN;
    impl_->lost.store(true);
}

UINT CaptureKatanga::InitialWidth()  const { return impl_ ? impl_->width  : 0; }
UINT CaptureKatanga::InitialHeight() const { return impl_ ? impl_->height : 0; }

}  // namespace nv3dg
