#include "Gui.h"

#include "App.h"
#include "Capture.h"
#include "CaptureDXGI.h"

#include "imgui.h"

#include <Windows.h>
#include <cwchar>
#include <string>

namespace nv3dg {

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string HotkeyLabel(const HotkeyDef& hk) {
    std::string s;
    if (hk.mods & MOD_CONTROL) s += "Ctrl+";
    if (hk.mods & MOD_ALT)     s += "Alt+";
    if (hk.mods & MOD_SHIFT)   s += "Shift+";
    if (hk.mods & MOD_WIN)     s += "Win+";
    if (hk.vk >= 'A' && hk.vk <= 'Z') s += (char)hk.vk;
    else if (hk.vk >= '0' && hk.vk <= '9') s += (char)hk.vk;
    else {
        char buf[16]; sprintf_s(buf, "VK_0x%02X", hk.vk); s += buf;
    }
    return s;
}

const char* AppStateLabel(AppState s) {
    switch (s) {
        case AppState::Idle:           return "Idle";
        case AppState::Running:        return "Running";
        case AppState::SourceLost:     return "Source lost";
        case AppState::ErrorPresenter: return "Presenter error";
        case AppState::ErrorCapture:   return "Capture error";
    }
    return "?";
}

bool CaptureChordOnce(HotkeyDef& out) {
    ImGuiIO& io = ImGui::GetIO();
    UINT mods = 0;
    if (io.KeyCtrl)  mods |= MOD_CONTROL;
    if (io.KeyAlt)   mods |= MOD_ALT;
    if (io.KeyShift) mods |= MOD_SHIFT;
    if (io.KeySuper) mods |= MOD_WIN;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (!ImGui::IsKeyPressed((ImGuiKey)k, false)) continue;
        // Skip modifier-only keys.
        if (k == ImGuiKey_LeftCtrl  || k == ImGuiKey_RightCtrl ||
            k == ImGuiKey_LeftAlt   || k == ImGuiKey_RightAlt  ||
            k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift||
            k == ImGuiKey_LeftSuper || k == ImGuiKey_RightSuper) continue;
        // Map ImGuiKey → VK. ImGui exposes the mapping table on Windows.
        // ImGui_ImplWin32 keeps the original VK in io.KeysData, but the safer
        // approach is ImGui::GetKeyChordName + a lookup. For our purposes,
        // a small range suffices for letters / digits / function keys.
        if (k >= ImGuiKey_A && k <= ImGuiKey_Z) {
            out.mods = mods;
            out.vk   = 'A' + (k - ImGuiKey_A);
            return true;
        }
        if (k >= ImGuiKey_0 && k <= ImGuiKey_9) {
            out.mods = mods;
            out.vk   = '0' + (k - ImGuiKey_0);
            return true;
        }
        if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) {
            out.mods = mods;
            out.vk   = VK_F1 + (k - ImGuiKey_F1);
            return true;
        }
    }
    return false;
}

}  // anonymous

void Gui::RefreshSources() {
    windows_  = EnumerateWindows();
    monitors_ = EnumerateMonitors();
}

void Gui::Draw(App& app) {
    if (windows_.empty() && monitors_.empty()) RefreshSources();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("NV3D-Glass", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawCaptureSourceSection(app);
    DrawOutputSection(app);
    DrawHotkeySection(app);
    DrawStatusFooter(app);

    ImGui::End();
}

void Gui::DrawCaptureSourceSection(App& app) {
    ImGui::SeparatorText("Capture Source");

    Settings& s = app.settings();
    int kind_idx = static_cast<int>(s.source_kind);
    const char* kinds[] = { "None", "Window", "Monitor" };
    if (ImGui::Combo("Source kind", &kind_idx, kinds, IM_ARRAYSIZE(kinds))) {
        s.source_kind = static_cast<SourceKind>(kind_idx);
        s.source_id.clear();
    }

    if (s.source_kind == SourceKind::Window) {
        // Build display strings on the fly to avoid lifetime headaches.
        int current = -1;
        for (size_t i = 0; i < windows_.size(); ++i) {
            if (windows_[i].stable_id() == s.source_id) { current = (int)i; break; }
        }
        if (ImGui::BeginCombo("Window", current >= 0 ?
                ToUtf8(windows_[current].title + L" [" + windows_[current].process_basename + L"]").c_str() :
                "<select a window>")) {
            for (size_t i = 0; i < windows_.size(); ++i) {
                bool sel = ((int)i == current);
                std::string label = ToUtf8(windows_[i].title) + " [" + ToUtf8(windows_[i].process_basename) + "]";
                if (ImGui::Selectable(label.c_str(), sel)) {
                    s.source_kind = SourceKind::Window;
                    s.source_id   = windows_[i].stable_id();
                }
            }
            ImGui::EndCombo();
        }
    } else if (s.source_kind == SourceKind::Monitor) {
        int current = -1;
        for (size_t i = 0; i < monitors_.size(); ++i) {
            if (_wcsicmp(monitors_[i].device_name.c_str(), s.source_id.c_str()) == 0) {
                current = (int)i; break;
            }
        }
        if (ImGui::BeginCombo("Monitor (capture)", current >= 0 ?
                ToUtf8(monitors_[current].friendly_name).c_str() : "<select a monitor>")) {
            for (size_t i = 0; i < monitors_.size(); ++i) {
                bool sel = ((int)i == current);
                if (ImGui::Selectable(ToUtf8(monitors_[i].friendly_name).c_str(), sel)) {
                    s.source_kind = SourceKind::Monitor;
                    s.source_id   = monitors_[i].device_name;
                }
            }
            ImGui::EndCombo();
        }
    }

    if (ImGui::Button("Refresh sources")) RefreshSources();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-reacquire on disconnect", &s.auto_reacquire);
}

void Gui::DrawOutputSection(App& app) {
    ImGui::SeparatorText("3D Vision Output");

    Settings& s = app.settings();

    int current = -1;
    for (size_t i = 0; i < monitors_.size(); ++i) {
        if (_wcsicmp(monitors_[i].device_name.c_str(), s.output_monitor_id.c_str()) == 0) {
            current = (int)i; break;
        }
    }
    if (ImGui::BeginCombo("Output monitor", current >= 0 ?
            ToUtf8(monitors_[current].friendly_name).c_str() : "<auto>")) {
        if (ImGui::Selectable("<auto>", current < 0)) {
            s.output_monitor_id.clear();
        }
        for (size_t i = 0; i < monitors_.size(); ++i) {
            bool sel = ((int)i == current);
            if (ImGui::Selectable(ToUtf8(monitors_[i].friendly_name).c_str(), sel)) {
                s.output_monitor_id = monitors_[i].device_name;
            }
        }
        ImGui::EndCombo();
    }

    bool dirty = false;
    dirty |= ImGui::Checkbox("Swap eyes",          &s.eye_swap);
    dirty |= ImGui::Checkbox("Stay on top",        &s.on_top);
    dirty |= ImGui::Checkbox("LightBoost timings", &s.enable_lightboost);
    dirty |= ImGui::Checkbox("3DVision suppressor", &s.enable_suppressor);
    ImGui::TextDisabled("(toggling any of the above reconnects the display)");

    if (dirty && app.state() == AppState::Running) app.Restart();

    ImGui::Spacing();
    const bool running = app.state() == AppState::Running;
    if (!running) {
        if (ImGui::Button("Start", ImVec2(120, 32))) app.Start();
        if (s.source_kind == SourceKind::None || s.source_id.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(no source = stereo test pattern only)");
        }
    } else {
        if (ImGui::Button("Stop", ImVec2(120, 32))) app.Stop();
    }
}

void Gui::DrawHotkeySection(App& app) {
    ImGui::SeparatorText("Hotkeys");

    Settings& s = app.settings();
    auto row = [&](const char* label, int id, HotkeyDef& hk, bool& dirty) {
        ImGui::PushID(id);
        ImGui::Text("%s", label);
        ImGui::SameLine(180);
        ImGui::Text("%s", HotkeyLabel(hk).c_str());
        ImGui::SameLine();
        if (!hk.registered) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(in use by another app)");
            ImGui::SameLine();
        }
        if (capturing_chord_for_ && chord_id_ == id) {
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "press chord...");
            HotkeyDef captured{};
            if (CaptureChordOnce(captured) && captured.vk != 0) {
                hk = captured;
                hk.registered = false;
                capturing_chord_for_ = false;
                chord_id_ = -1;
                dirty = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                capturing_chord_for_ = false;
                chord_id_ = -1;
            }
        } else {
            if (ImGui::Button("Rebind")) {
                capturing_chord_for_ = true;
                chord_id_ = id;
            }
        }
        ImGui::PopID();
    };

    bool any_change = false;
    row("Toggle eye-swap",         1, s.hk_eyeswap,      any_change);
    row("Show/hide control panel", 2, s.hk_toggle_panel, any_change);
    row("Show/hide 3D output",     4, s.hk_toggle_fse,   any_change);
    row("Quit",                    3, s.hk_quit,         any_change);

    if (any_change) {
        app.hotkeys().Clear();
        app.settings().hk_eyeswap.registered      = app.hotkeys().Register(1, s.hk_eyeswap.mods, s.hk_eyeswap.vk, [&app](){ app.ToggleEyeSwap(); });
        app.settings().hk_toggle_panel.registered = app.hotkeys().Register(2, s.hk_toggle_panel.mods, s.hk_toggle_panel.vk, [&app](){ app.TogglePanelVisible(); });
        app.settings().hk_quit.registered         = app.hotkeys().Register(3, s.hk_quit.mods, s.hk_quit.vk, [&app](){ app.Quit(); });
        app.settings().hk_toggle_fse.registered   = app.hotkeys().Register(4, s.hk_toggle_fse.mods, s.hk_toggle_fse.vk, [&app](){ app.ToggleFseVisible(); });
    }
}

void Gui::DrawStatusFooter(App& app) {
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();
    Settings& s = app.settings();
    char status[256];
    if (app.state() == AppState::Running) {
        sprintf_s(status, "Capturing %ux%u BGRA8 @ %.1f fps",
                  app.src_w(), app.src_h(), app.fps());
    } else {
        sprintf_s(status, "%s", AppStateLabel(app.state()));
    }
    ImGui::Text("%s", status);
    if (!app.status_extra().empty()) {
        ImGui::SameLine(); ImGui::TextDisabled("(%s)", ToUtf8(app.status_extra()).c_str());
    }
    ImGui::Text("Presenter: %s", app.presenter().IsActive() ? "active" : "idle");
    ImGui::TextDisabled("settings persist to NV3D-Glass.ini next to the .exe");
    (void)s;
}

}  // namespace nv3dg
