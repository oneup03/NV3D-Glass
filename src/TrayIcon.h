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
#include <functional>
#include <string>

namespace nv3dg {

// Tray icon: posts WM_APP+1 to the target HWND with notification events.
// The host's WndProc forwards WM_APP+1 messages to OnMessage().
class TrayIcon {
public:
    static constexpr UINT kCallbackMsg = WM_APP + 1;

    using Action = std::function<void()>;

    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&)            = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // hwnd: window that owns the icon and will receive kCallbackMsg.
    // tooltip is shown on hover.
    bool Install(HWND hwnd, const std::wstring& tooltip);
    void Remove();

    // Menu actions:
    void SetActions(Action on_show, Action on_hide,
                    Action on_start_stop, Action on_quit);
    void SetStartStopLabel(const std::wstring& label) { start_stop_label_ = label; }

    // Forward kCallbackMsg from the host's WndProc.
    void OnMessage(WPARAM wparam, LPARAM lparam);

private:
    void ShowMenu();

    HWND          hwnd_     = nullptr;
    bool          installed_ = false;
    Action        on_show_, on_hide_, on_start_stop_, on_quit_;
    std::wstring  start_stop_label_ = L"Start";
};

}
