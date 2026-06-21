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
