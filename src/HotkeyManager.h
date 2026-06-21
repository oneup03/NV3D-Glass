#pragma once

#include <Windows.h>
#include <functional>
#include <unordered_map>

namespace nv3dg {

class HotkeyManager {
public:
    using Callback = std::function<void()>;

    HotkeyManager() = default;
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&)            = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // hwnd: target window that will receive WM_HOTKEY.
    void Attach(HWND hwnd);

    // Returns true on successful RegisterHotKey.
    bool Register(int id, UINT mods, UINT vk, Callback cb);
    void Unregister(int id);

    // Call from the message pump for WM_HOTKEY. wParam = id.
    void Dispatch(WPARAM wparam);

    void Clear();

private:
    HWND                                       hwnd_ = nullptr;
    std::unordered_map<int, Callback>          callbacks_;
};

}
