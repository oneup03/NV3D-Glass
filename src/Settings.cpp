#include "Settings.h"

#include <Windows.h>
#include <Shlwapi.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "Shlwapi.lib")

namespace nv3dg {

namespace {

constexpr const wchar_t* kSection = L"NV3D-Glass";

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

std::wstring ReadStr(const wchar_t* key, const wchar_t* def, const wchar_t* path) {
    wchar_t buf[1024]{};
    GetPrivateProfileStringW(kSection, key, def, buf, _countof(buf), path);
    return buf;
}

int ReadInt(const wchar_t* key, int def, const wchar_t* path) {
    return GetPrivateProfileIntW(kSection, key, def, path);
}

bool ReadBool(const wchar_t* key, bool def, const wchar_t* path) {
    return GetPrivateProfileIntW(kSection, key, def ? 1 : 0, path) != 0;
}

void WriteStr(const wchar_t* key, const wchar_t* val, const wchar_t* path) {
    WritePrivateProfileStringW(kSection, key, val, path);
}

void WriteInt(const wchar_t* key, int val, const wchar_t* path) {
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%d", val);
    WritePrivateProfileStringW(kSection, key, buf, path);
}

const wchar_t* SourceKindStr(SourceKind k) {
    switch (k) {
        case SourceKind::Window:  return L"window";
        case SourceKind::Monitor: return L"monitor";
        default:                  return L"none";
    }
}

SourceKind SourceKindFromStr(const std::wstring& s) {
    if (_wcsicmp(s.c_str(), L"window")  == 0) return SourceKind::Window;
    if (_wcsicmp(s.c_str(), L"monitor") == 0) return SourceKind::Monitor;
    return SourceKind::None;
}

// "MOD_CONTROL|MOD_ALT|E" <-> {MOD_CONTROL|MOD_ALT, 'E'}
std::wstring HotkeyToStr(const HotkeyDef& hk) {
    std::wstring out;
    if (hk.mods & MOD_CONTROL) out += L"MOD_CONTROL|";
    if (hk.mods & MOD_ALT)     out += L"MOD_ALT|";
    if (hk.mods & MOD_SHIFT)   out += L"MOD_SHIFT|";
    if (hk.mods & MOD_WIN)     out += L"MOD_WIN|";
    wchar_t vk[16];
    if (hk.vk >= '0' && hk.vk <= '9') {
        _snwprintf_s(vk, _TRUNCATE, L"%c", (char)hk.vk);
    } else if (hk.vk >= 'A' && hk.vk <= 'Z') {
        _snwprintf_s(vk, _TRUNCATE, L"%c", (char)hk.vk);
    } else {
        _snwprintf_s(vk, _TRUNCATE, L"0x%02X", hk.vk);
    }
    out += vk;
    return out;
}

HotkeyDef HotkeyFromStr(const std::wstring& s, const HotkeyDef& def) {
    HotkeyDef out{};
    std::wstring cur;
    auto flush = [&]() {
        if (cur.empty()) return;
        if (_wcsicmp(cur.c_str(), L"MOD_CONTROL") == 0) { out.mods |= MOD_CONTROL; cur.clear(); return; }
        if (_wcsicmp(cur.c_str(), L"MOD_ALT")     == 0) { out.mods |= MOD_ALT;     cur.clear(); return; }
        if (_wcsicmp(cur.c_str(), L"MOD_SHIFT")   == 0) { out.mods |= MOD_SHIFT;   cur.clear(); return; }
        if (_wcsicmp(cur.c_str(), L"MOD_WIN")     == 0) { out.mods |= MOD_WIN;     cur.clear(); return; }
        // VK: either single char A-Z/0-9 or 0xNN
        if (cur.size() == 1) {
            wchar_t c = (wchar_t)towupper(cur[0]);
            out.vk = (UINT)c;
        } else if (cur.size() > 2 && cur[0] == L'0' && (cur[1] == L'x' || cur[1] == L'X')) {
            out.vk = (UINT)wcstoul(cur.c_str() + 2, nullptr, 16);
        }
        cur.clear();
    };
    for (wchar_t c : s) {
        if (c == L'|') flush();
        else cur += c;
    }
    flush();
    if (out.mods == 0 && out.vk == 0) return def;
    return out;
}

}  // anonymous

std::wstring DefaultIniPath() { return ExeDir() + L"\\NV3D-Glass.ini"; }
std::wstring DefaultLogPath() { return ExeDir() + L"\\NV3D-Glass.log"; }

void LoadSettings(Settings& s, const wchar_t* path) {
    s.source_kind        = SourceKindFromStr(ReadStr(L"last_source_kind", L"none", path));
    s.source_id          = ReadStr(L"last_source_id", L"", path);
    s.output_monitor_id  = ReadStr(L"output_monitor", L"", path);
    s.eye_swap           = ReadBool(L"eye_swap",           s.eye_swap,           path);
    s.on_top             = ReadBool(L"on_top",             s.on_top,             path);
    s.enable_lightboost  = ReadBool(L"enable_lightboost",  s.enable_lightboost,  path);
    s.enable_suppressor  = ReadBool(L"enable_suppressor",  s.enable_suppressor,  path);
    s.auto_reacquire     = ReadBool(L"auto_reacquire",     s.auto_reacquire,     path);
    s.panel_x            = ReadInt (L"panel_x",            s.panel_x,            path);
    s.panel_y            = ReadInt (L"panel_y",            s.panel_y,            path);
    s.panel_w            = ReadInt (L"panel_w",            s.panel_w,            path);
    s.panel_h            = ReadInt (L"panel_h",            s.panel_h,            path);
    s.hk_eyeswap      = HotkeyFromStr(ReadStr(L"hotkey_eyeswap",      HotkeyToStr(s.hk_eyeswap     ).c_str(), path), s.hk_eyeswap);
    s.hk_toggle_panel = HotkeyFromStr(ReadStr(L"hotkey_toggle_panel", HotkeyToStr(s.hk_toggle_panel).c_str(), path), s.hk_toggle_panel);
    s.hk_toggle_fse   = HotkeyFromStr(ReadStr(L"hotkey_toggle_fse",   HotkeyToStr(s.hk_toggle_fse  ).c_str(), path), s.hk_toggle_fse);
    s.hk_quit         = HotkeyFromStr(ReadStr(L"hotkey_quit",         HotkeyToStr(s.hk_quit        ).c_str(), path), s.hk_quit);
}

void SaveSettings(const Settings& s, const wchar_t* path) {
    WriteStr(L"last_source_kind",    SourceKindStr(s.source_kind),       path);
    WriteStr(L"last_source_id",      s.source_id.c_str(),                path);
    WriteStr(L"output_monitor",      s.output_monitor_id.c_str(),        path);
    WriteInt(L"eye_swap",            s.eye_swap          ? 1 : 0,        path);
    WriteInt(L"on_top",              s.on_top            ? 1 : 0,        path);
    WriteInt(L"enable_lightboost",   s.enable_lightboost ? 1 : 0,        path);
    WriteInt(L"enable_suppressor",   s.enable_suppressor ? 1 : 0,        path);
    WriteInt(L"auto_reacquire",      s.auto_reacquire    ? 1 : 0,        path);
    WriteInt(L"panel_x",             s.panel_x,                          path);
    WriteInt(L"panel_y",             s.panel_y,                          path);
    WriteInt(L"panel_w",             s.panel_w,                          path);
    WriteInt(L"panel_h",             s.panel_h,                          path);
    WriteStr(L"hotkey_eyeswap",      HotkeyToStr(s.hk_eyeswap     ).c_str(), path);
    WriteStr(L"hotkey_toggle_panel", HotkeyToStr(s.hk_toggle_panel).c_str(), path);
    WriteStr(L"hotkey_toggle_fse",   HotkeyToStr(s.hk_toggle_fse  ).c_str(), path);
    WriteStr(L"hotkey_quit",         HotkeyToStr(s.hk_quit        ).c_str(), path);
}

}  // namespace nv3dg
