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
