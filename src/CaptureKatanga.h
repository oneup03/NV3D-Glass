#pragma once

#include "CaptureSource.h"

#include <memory>

struct ID3D11Device;

namespace nv3dg {

// Katanga / Geo-11 / 3DVision4All shared-texture receiver.
//
// The producer publishes a 2W x H side-by-side stereo frame as a shared D3D
// texture and writes the texture's KMT shared handle (a single pointer-sized
// value) into the named file mapping "Local\KatangaMappedFile". Any process
// that opens that mapping reads the handle and imports the texture into its
// own D3D11 device via ID3D11Device::OpenSharedResource.
//
// The handle is API-agnostic — D3D9Ex (pSharedHandle) and D3D11
// (D3D11_RESOURCE_MISC_SHARED) both yield session-global KMT handles that
// OpenSharedResource accepts transparently. NT-handle producers
// (D3D11_RESOURCE_MISC_SHARED_NTHANDLE) cannot publish through this contract
// because NT handles are process-local.
//
// Contract is lock-free: producer atomically rewrites the 8-byte handle slot
// when it rebuilds the staging texture (resolution change, restart, ...). We
// re-read it every TryAcquire and re-import on change.
class CaptureKatanga : public ICaptureSource {
public:
    CaptureKatanga();
    ~CaptureKatanga() override;

    CaptureKatanga(const CaptureKatanga&)            = delete;
    CaptureKatanga& operator=(const CaptureKatanga&) = delete;

    static std::unique_ptr<CaptureKatanga> Create(ID3D11Device* dev);

    bool TryAcquire(ID3D11Texture2D** out, UINT* w, UINT* h, DXGI_FORMAT* fmt) override;
    bool IsLost() const override;
    bool IsDisconnected() const override;
    void Stop() override;

    // Clear soft-disconnect state so the next TryAcquire can detect a new
    // producer without tearing down the file mapping. Safe no-op if the
    // source is in any state other than disconnected. Does NOT recover from
    // IsLost — that's a fatal device-removed condition.
    void ResetForReconnect();

    // Dimensions of the shared texture at first open — used by App::Start to
    // size the local staging texture so CopyResource hits the fast path.
    UINT InitialWidth()  const;
    UINT InitialHeight() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nv3dg
