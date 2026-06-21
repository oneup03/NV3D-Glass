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
