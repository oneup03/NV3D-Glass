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
#include <string>
#include <vector>

namespace nv3dg {

struct WindowEntry {
    HWND         hwnd = nullptr;
    std::wstring title;
    std::wstring process_basename;   // "Notepad.exe"
    // Stable identifier used for INI persistence: "Notepad.exe|Untitled - Notepad".
    std::wstring stable_id() const { return process_basename + L"|" + title; }
};

struct MonitorEntry {
    HMONITOR     hmon = nullptr;
    std::wstring device_name;   // "\\.\DISPLAY1"
    std::wstring friendly_name; // "Generic PnP Monitor (2560x1440 @ 144Hz)" (best-effort)
    RECT         rect{};
    bool         is_primary = false;
};

std::vector<WindowEntry>  EnumerateWindows();
std::vector<MonitorEntry> EnumerateMonitors();

// Try to find a window by stable_id (process_basename + "|" + title).
// Returns null if not found.
HWND FindWindowByStableId(const std::wstring& stable_id);

// Resolve "\\.\DISPLAY1" → HMONITOR. Returns null if not found.
HMONITOR FindMonitorByDeviceName(const std::wstring& device_name);

}
