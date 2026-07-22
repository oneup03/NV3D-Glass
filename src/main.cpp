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

// NV3D-Glass - capture a side-by-side stereo source and re-present it as
// NVIDIA 3D Vision via NV3DLib's FSE click-through window.
//
// Process layout:
//   - Main thread: control panel HWND + ImGui + WGC/DXGI capture poll +
//     NV3DPresenter::SubmitFrame.
//   - NV3DLib internal thread: owns the FSE click-through window on the
//     selected 3D Vision display, runs focus-suppression + minimize-suppression.

#include "App.h"
#include "Logging.h"

#include <Windows.h>

// C++/WinRT requires apartment init exactly once per thread before any winrt
// API call. We do it here so WGC capture's first call (in App::Start) is
// already on a valid apartment.
#include <winrt/base.h>

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    nv3dg::App app;
    int rc = 0;
    // Last-resort handler. Nothing in the message loop (WndProc → App::Start →
    // WGC) sat under a try/catch, so a thrown winrt::hresult_error unwound
    // straight to std::terminate and the process vanished with no log line past
    // the point of the throw. Catch-and-log here converts any future escape
    // into a diagnosable Error line instead of a silent death; the specific WGC
    // throw sites that caused this are now guarded at their source too.
    try {
        if (!app.Init(hInstance)) return 1;
        rc = app.Run();
        app.Shutdown();
    } catch (winrt::hresult_error const& e) {
        nv3dg::Log(NV3D::LogLevel::Error,
                   L"main: FATAL uncaught winrt::hresult_error hr=0x%08X (%s)",
                   static_cast<unsigned>(e.code()), e.message().c_str());
        rc = 2;
    } catch (...) {
        nv3dg::Log(NV3D::LogLevel::Error, L"main: FATAL uncaught exception");
        rc = 2;
    }

    // Log file stays open here on purpose - App::Shutdown used to call
    // ShutdownFileLog() at the very end, which meant nothing past that point
    // could be logged. We deferred the close to here so the WinRT teardown
    // and ~App double-Shutdown are observable. The remaining freeze surface
    // (post "control panel destroyed") is one of these steps; the next log
    // line that DOESN'T appear is the one that blocked.
    nv3dg::Log(NV3D::LogLevel::Info, L"main: before winrt::uninit_apartment");
    winrt::uninit_apartment();
    nv3dg::Log(NV3D::LogLevel::Info, L"main: after winrt::uninit_apartment");

    nv3dg::ShutdownFileLog();

    // Skip CRT static destructors and DLL_PROCESS_DETACH entirely. A hard
    // display freeze (reboot required) was observed AFTER the last logged
    // line above - i.e. inside process teardown, where nvd3dumx.dll /
    // nvapi64.dll / d3d9.dll run their detach code against driver stereo
    // state our FSE teardown had just strained (that session's D3D9 release
    // stalled 1.8s while the driver reclaimed a dying game's GPU context).
    // Nothing past this point matters: settings are persisted, every
    // subsystem is explicitly Shutdown, the log is flushed and closed.
    // ExitProcess would still run DLL_PROCESS_DETACH; TerminateProcess does
    // not, and never returns on success.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
    return rc;  // unreachable
}
