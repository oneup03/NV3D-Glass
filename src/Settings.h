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
#include <cstdint>

namespace nv3dg {

enum class SourceKind : int { None = 0, Window = 1, Monitor = 2, Katanga = 3 };

struct HotkeyDef {
    UINT mods = 0;     // MOD_CONTROL|MOD_ALT etc.
    UINT vk   = 0;     // virtual-key code
    bool registered = false;  // runtime status, not persisted
};

struct Settings {
    // ---- 3D Vision output / NV3DPresenter init params ----
    HMONITOR     target_monitor    = nullptr;  // null = auto (let NV3DLib pick primary 3DVision display)
    std::wstring output_monitor_id;             // "\\.\DISPLAY1" — for persistence; resolves to HMONITOR at startup
    bool         eye_swap          = false;
    bool         on_top            = true;
    bool         enable_lightboost = true;
    bool         enable_suppressor = true;

    // ---- Capture source ----
    SourceKind   source_kind = SourceKind::None;
    // For Window: process_basename + L"|" + window_title (re-matched at startup since HWNDs aren't stable).
    // For Monitor: GDI device name "\\.\DISPLAY1".
    std::wstring source_id;

    bool         auto_reacquire = true;

    // ---- Cursor lock / 3D cursor ----
    // Clip the mouse to the captured window/monitor while the 3D output is
    // showing (so mouse-look games don't let the pointer wander off onto the
    // desktop / other monitors). The clip region follows the current
    // foreground window, so alt+tabbing updates it. Only active while the FSE
    // popup is visible; released on every teardown path.
    bool         lock_cursor = false;
    // Draw a software reticle into the SbS output at the mouse's position
    // within the captured content. The captured frame never contains the OS
    // cursor (WGC cursor capture is off), so this is the only pointer the 3D
    // view can show.
    bool         draw_3d_cursor = false;
    // Right/left-eye horizontal split for the 3D cursor as a fraction of the
    // per-eye width. 0 = screen plane. Small +/- values push the reticle
    // behind / in front of the screen. Kept tiny — a few % is a lot of depth.
    float        cursor_parallax = 0.0f;

    // ---- Hotkeys (persisted) ----
    HotkeyDef hk_eyeswap     { MOD_CONTROL, VK_F9 };
    HotkeyDef hk_toggle_panel{ MOD_CONTROL | MOD_ALT, 'S' };
    HotkeyDef hk_toggle_fse  { MOD_CONTROL, VK_F8 };
    HotkeyDef hk_quit        { MOD_CONTROL | MOD_ALT, 'Q' };

    // ---- Control panel window position/size ----
    int panel_x = CW_USEDEFAULT;
    int panel_y = CW_USEDEFAULT;
    int panel_w = 480;
    int panel_h = 600;
};

void   LoadSettings(Settings& s, const wchar_t* path);
void   SaveSettings(const Settings& s, const wchar_t* path);
std::wstring DefaultIniPath();   // returns "<exe-dir>\NV3D-Glass.ini"
std::wstring DefaultLogPath();   // returns "<exe-dir>\NV3D-Glass.log"

}
