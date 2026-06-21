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
