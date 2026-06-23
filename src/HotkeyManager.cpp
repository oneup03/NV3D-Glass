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
