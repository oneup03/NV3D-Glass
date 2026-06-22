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

    // Dimensions WGC will deliver for the captured item — known at session
    // construction time (item.Size()). Callers use this to size their staging
    // texture exactly so CopyResource hits the fast path instead of falling
    // through to a scaler shader pass on every frame.
    UINT InitialWidth()  const;
    UINT InitialHeight() const;

    // For a window source, the sub-rect of the captured frame that
    // corresponds to the window's client area — i.e. excluding the title
    // bar, borders, and the invisible DWM shadow. Returns the full frame
    // rect for monitor sources / when client-area detection fails. Callers
    // use this to crop the title bar out of the stereo output via
    // CopySubresourceRegion at staging-write time.
    RECT ClientAreaInCapture() const;

    // Public so file-scope helpers in Capture.cpp can name the type. The
    // pointer itself stays owned by us.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nv3dg
