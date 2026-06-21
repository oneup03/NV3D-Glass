#include "TrayIcon.h"

#include <shellapi.h>

namespace nv3dg {

namespace {

constexpr UINT kIconUid = 1;

enum MenuCmd : UINT {
    kCmdShow      = 100,
    kCmdHide      = 101,
    kCmdStartStop = 102,
    kCmdQuit      = 103,
};

}  // anonymous

TrayIcon::~TrayIcon() { Remove(); }

bool TrayIcon::Install(HWND hwnd, const std::wstring& tooltip) {
    if (installed_) return true;
    hwnd_ = hwnd;
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd_;
    nid.uID              = kIconUid;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kCallbackMsg;
    nid.hIcon            = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, tooltip.c_str(), _countof(nid.szTip));
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    installed_ = true;
    return true;
}

void TrayIcon::Remove() {
    if (!installed_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = kIconUid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    installed_ = false;
}

void TrayIcon::SetActions(Action on_show, Action on_hide,
                          Action on_start_stop, Action on_quit) {
    on_show_       = std::move(on_show);
    on_hide_       = std::move(on_hide);
    on_start_stop_ = std::move(on_start_stop);
    on_quit_       = std::move(on_quit);
}

void TrayIcon::OnMessage(WPARAM, LPARAM lparam) {
    const UINT evt = LOWORD(lparam);
    switch (evt) {
        case WM_LBUTTONDBLCLK:
            if (on_show_) on_show_();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowMenu();
            break;
        default: break;
    }
}

void TrayIcon::ShowMenu() {
    if (!hwnd_) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdShow,      L"Show control panel");
    AppendMenuW(menu, MF_STRING, kCmdHide,      L"Hide control panel");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdStartStop, start_stop_label_.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdQuit,      L"Quit");

    SetForegroundWindow(hwnd_);
    POINT pt{}; GetCursorPos(&pt);
    UINT cmd = TrackPopupMenu(menu,
                              TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);  // ensure follow-up clicks dismiss cleanly

    switch (cmd) {
        case kCmdShow:      if (on_show_)       on_show_();       break;
        case kCmdHide:      if (on_hide_)       on_hide_();       break;
        case kCmdStartStop: if (on_start_stop_) on_start_stop_(); break;
        case kCmdQuit:      if (on_quit_)       on_quit_();       break;
        default: break;
    }
}

}  // namespace nv3dg
