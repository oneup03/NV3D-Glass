#pragma once

#include "CaptureSource.h"

#include <memory>

struct ID3D11Device;

namespace nv3dg {

// Windows.Graphics.Capture (WGC) backend. Captures a target HWND or HMONITOR
// into a Direct3D11CaptureFramePool the main thread polls with TryGetNextFrame.
// No dependence on the source app's renderer; works for any visible window or
// monitor on Windows 10 1903+. Falls short for some exclusive-fullscreen games
// — those need CaptureDXGI.
class CaptureWGC : public ICaptureSource {
public:
    CaptureWGC();
    ~CaptureWGC() override;

    CaptureWGC(const CaptureWGC&)            = delete;
    CaptureWGC& operator=(const CaptureWGC&) = delete;

    static std::unique_ptr<CaptureWGC> CreateForWindow (ID3D11Device* dev, HWND hwnd);
    static std::unique_ptr<CaptureWGC> CreateForMonitor(ID3D11Device* dev, HMONITOR hmon);

    bool TryAcquire(ID3D11Texture2D** out, UINT* w, UINT* h, DXGI_FORMAT* fmt) override;
    bool IsLost() const override;
    void Stop() override;

    // Public so file-scope helpers in Capture.cpp can name the type. The
    // pointer itself stays owned by us.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nv3dg
