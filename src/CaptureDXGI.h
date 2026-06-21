#pragma once

#include "CaptureSource.h"

#include <memory>

struct ID3D11Device;

namespace nv3dg {

// DXGI Output Duplication fallback. Captures a monitor's desktop image even
// while an exclusive-fullscreen app is presenting on it. Cannot target an
// individual window; the WGC path is preferred when it works.
class CaptureDXGI : public ICaptureSource {
public:
    CaptureDXGI();
    ~CaptureDXGI() override;

    CaptureDXGI(const CaptureDXGI&)            = delete;
    CaptureDXGI& operator=(const CaptureDXGI&) = delete;

    static std::unique_ptr<CaptureDXGI> CreateForMonitor(ID3D11Device* dev, HMONITOR hmon);

    bool TryAcquire(ID3D11Texture2D** out, UINT* w, UINT* h, DXGI_FORMAT* fmt) override;
    bool IsLost() const override;
    void Stop() override;

    // Public so file-scope helpers in CaptureDXGI.cpp can name the type.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nv3dg
