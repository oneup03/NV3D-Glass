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

    // Soft-recoverable failure: the source went away in a way the host can
    // wait through (e.g. Katanga producer process exited but our shared
    // mapping is still open). Hosts may transition to a "waiting" state and
    // call ResetForReconnect() rather than tearing the session down. Default
    // false — only Katanga distinguishes disconnect from lost today.
    virtual bool IsDisconnected() const { return false; }

    // Stop the underlying capture session and release backend resources.
    virtual void Stop() = 0;
};

}  // namespace nv3dg
