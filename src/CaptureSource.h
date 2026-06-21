#pragma once

#include <Windows.h>
#include <dxgiformat.h>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace nv3dg {

// Common shape for both WGC and DXGI duplication capture backends.
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    // Returns false if the session ended (source closed, ACCESS_LOST, etc.).
    // Caller checks IsLost() to differentiate "no new frame yet" vs "broken".
    // On true: *out is AddRef'd by the implementation; caller releases.
    virtual bool TryAcquire(ID3D11Texture2D** out,
                            UINT* w, UINT* h,
                            DXGI_FORMAT* fmt) = 0;

    virtual bool IsLost() const = 0;

    // Stop the underlying capture session and release backend resources.
    virtual void Stop() = 0;
};

}  // namespace nv3dg
