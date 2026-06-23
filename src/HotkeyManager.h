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
