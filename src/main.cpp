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

// NV3D-Glass — capture a side-by-side stereo source and re-present it as
// NVIDIA 3D Vision via NV3DLib's FSE click-through window.
//
// Process layout:
//   - Main thread: control panel HWND + ImGui + WGC/DXGI capture poll +
//     NV3DPresenter::SubmitFrame.
//   - NV3DLib internal thread: owns the FSE click-through window on the
//     selected 3D Vision display, runs focus-suppression + minimize-suppression.

#include "App.h"

#include <Windows.h>

// C++/WinRT requires apartment init exactly once per thread before any winrt
// API call. We do it here so WGC capture's first call (in App::Start) is
// already on a valid apartment.
#include <winrt/base.h>

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    nv3dg::App app;
    if (!app.Init(hInstance)) return 1;
    const int rc = app.Run();
    app.Shutdown();

    winrt::uninit_apartment();
    return rc;
}
