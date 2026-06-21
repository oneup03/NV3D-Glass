#include "HotkeyManager.h"

namespace nv3dg {

HotkeyManager::~HotkeyManager() { Clear(); }

void HotkeyManager::Attach(HWND hwnd) { hwnd_ = hwnd; }

bool HotkeyManager::Register(int id, UINT mods, UINT vk, Callback cb) {
    if (!hwnd_) return false;
    UnregisterHotKey(hwnd_, id);
    if (!RegisterHotKey(hwnd_, id, mods | MOD_NOREPEAT, vk)) {
        callbacks_.erase(id);
        return false;
    }
    callbacks_[id] = std::move(cb);
    return true;
}

void HotkeyManager::Unregister(int id) {
    if (hwnd_) UnregisterHotKey(hwnd_, id);
    callbacks_.erase(id);
}

void HotkeyManager::Dispatch(WPARAM wparam) {
    auto it = callbacks_.find((int)wparam);
    if (it != callbacks_.end() && it->second) it->second();
}

void HotkeyManager::Clear() {
    if (hwnd_) {
        for (auto& kv : callbacks_) UnregisterHotKey(hwnd_, kv.first);
    }
    callbacks_.clear();
}

}  // namespace nv3dg
