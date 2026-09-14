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
#include "NvProfile.h"
#include "Settings.h"

#include <Windows.h>

#include <string>

// C++/WinRT requires apartment init exactly once per thread before any winrt
// API call. We do it here so WGC capture's first call (in App::Start) is
// already on a valid apartment.
#include <winrt/base.h>

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // Elevated helper mode. The UI can't write NVIDIA driver settings without
    // admin rights, so it re-launches this same exe elevated with
    // --apply-nvprofile; that instance performs exactly one DRS transaction
    // and exits without ever creating a window, a device or a capture.
    // Checked before anything else so the helper stays as small and as
    // side-effect-free as possible.
    if (wcsstr(GetCommandLineW(), L"--apply-nvprofile")) {
        // Its own log file, not NV3D-Glass.log: the parent process is very
        // likely running and holding that one open exclusively, and
        // InitFileLog truncates. A separate file means the helper's
        // diagnostics survive and the parent's log is untouched.
        const std::wstring ini = nv3dg::DefaultIniPath();
        std::wstring helper_log = nv3dg::DefaultLogPath();
        const size_t dot = helper_log.find_last_of(L'.');
        helper_log = (dot == std::wstring::npos ? helper_log : helper_log.substr(0, dot)) +
                     L"-nvprofile.log";
        nv3dg::InitFileLog(helper_log.c_str());

        int profile_rc = 0;
        const bool handled = nv3dg::NvProfile::RunCommandLine(
            GetCommandLineW(), ini.c_str(), &profile_rc);
        nv3dg::ShutdownFileLog();
        if (handled) return profile_rc;
        // Fall through only if the flag was present but unparseable - better
        // to start normally than to exit silently.
    }

    // A restarting instance hands us the outgoing process id. Wait for it to
    // actually be gone before touching anything global - the tray icon, the
    // global hotkey registrations and the Katanga mapping are all things two
    // live instances would fight over. Bounded so a wedged predecessor can't
    // stop the replacement coming up at all.
    if (const wchar_t* p = wcsstr(GetCommandLineW(), L"--wait-for-pid")) {
        const DWORD pid = static_cast<DWORD>(_wtoi(p + wcslen(L"--wait-for-pid")));
        if (pid != 0) {
            if (HANDLE prev = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
                WaitForSingleObject(prev, 10000);
                CloseHandle(prev);
            }
        }
    }

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
