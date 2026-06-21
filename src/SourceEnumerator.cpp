#include "SourceEnumerator.h"

#include <Windows.h>
#include <Psapi.h>
#include <Shlwapi.h>
#include <cstdio>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace nv3dg {

namespace {

std::wstring ProcessBasenameForHwnd(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH]{};
    DWORD cap = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &cap)) {
        PathStripPathW(buf);
        out = buf;
    }
    CloseHandle(h);
    return out;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<WindowEntry>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (IsIconic(hwnd))         return TRUE;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW)  return TRUE;
    // Skip our own (avoid capturing the control panel)
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    std::wstring title(len + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(wcslen(title.c_str()));
    WindowEntry e;
    e.hwnd  = hwnd;
    e.title = title;
    e.process_basename = ProcessBasenameForHwnd(hwnd);
    list->push_back(std::move(e));
    return TRUE;
}

struct MonEnumState {
    std::vector<MonitorEntry>* list;
};

BOOL CALLBACK EnumMonProc(HMONITOR hmon, HDC, LPRECT lprc, LPARAM lp) {
    auto* st = reinterpret_cast<MonEnumState*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) return TRUE;
    MonitorEntry e;
    e.hmon        = hmon;
    e.device_name = mi.szDevice;
    e.rect        = mi.rcMonitor;
    e.is_primary  = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    UINT hz = 60;
    if (EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        hz = dm.dmDisplayFrequency;
    }
    wchar_t buf[256];
    _snwprintf_s(buf, _TRUNCATE, L"%s (%ldx%ld @ %uHz)%s",
                 mi.szDevice,
                 lprc ? (lprc->right - lprc->left) : 0,
                 lprc ? (lprc->bottom - lprc->top) : 0,
                 hz, e.is_primary ? L" *PRIMARY*" : L"");
    e.friendly_name = buf;
    st->list->push_back(std::move(e));
    return TRUE;
}

}  // anonymous

std::vector<WindowEntry> EnumerateWindows() {
    std::vector<WindowEntry> out;
    out.reserve(64);
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&out));
    return out;
}

std::vector<MonitorEntry> EnumerateMonitors() {
    std::vector<MonitorEntry> out;
    out.reserve(4);
    MonEnumState st{ &out };
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&st));
    return out;
}

HWND FindWindowByStableId(const std::wstring& stable_id) {
    auto list = EnumerateWindows();
    for (const auto& e : list) {
        if (e.stable_id() == stable_id) return e.hwnd;
    }
    return nullptr;
}

HMONITOR FindMonitorByDeviceName(const std::wstring& device_name) {
    if (device_name.empty()) return nullptr;
    auto list = EnumerateMonitors();
    for (const auto& e : list) {
        if (_wcsicmp(e.device_name.c_str(), device_name.c_str()) == 0) return e.hmon;
    }
    return nullptr;
}

}  // namespace nv3dg
