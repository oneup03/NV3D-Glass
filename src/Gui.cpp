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

#include "Gui.h"

#include "App.h"
#include "Capture.h"
#include "CaptureDXGI.h"
#include "NvProfile.h"
#include "Logging.h"

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
    if (hk.vk >= 'A' && hk.vk <= 'Z') {
        s += (char)hk.vk;
    } else if (hk.vk >= '0' && hk.vk <= '9') {
        s += (char)hk.vk;
    } else if (hk.vk >= VK_F1 && hk.vk <= VK_F24) {
        char buf[8];
        sprintf_s(buf, "F%u", static_cast<unsigned>(hk.vk - VK_F1 + 1u));
        s += buf;
    } else {
        switch (hk.vk) {
            case VK_SPACE:    s += "Space";     break;
            case VK_RETURN:   s += "Enter";     break;
            case VK_ESCAPE:   s += "Esc";       break;
            case VK_TAB:      s += "Tab";       break;
            case VK_BACK:     s += "Backspace"; break;
            case VK_DELETE:   s += "Del";       break;
            case VK_INSERT:   s += "Ins";       break;
            case VK_HOME:     s += "Home";      break;
            case VK_END:      s += "End";       break;
            case VK_PRIOR:    s += "PgUp";      break;
            case VK_NEXT:     s += "PgDn";      break;
            case VK_LEFT:     s += "Left";      break;
            case VK_RIGHT:    s += "Right";     break;
            case VK_UP:       s += "Up";        break;
            case VK_DOWN:     s += "Down";      break;
            default: {
                char buf[16];
                sprintf_s(buf, "VK_0x%02X", hk.vk);
                s += buf;
                break;
            }
        }
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

    RefreshProfileCache(app);
    ReconcileSyncProfileOnce(app);

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
    const char* kinds[] = { "None", "Window", "Monitor", "Katanga" };
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
    } else if (s.source_kind == SourceKind::Katanga) {
        ImGui::TextDisabled("Listens on Local\\KatangaMappedFile.");
        ImGui::TextDisabled("Producer can launch before or after Start - we'll wait.");

        // The Katanga contract identifies nothing about the producer - just a
        // shared texture handle - so the game process can only be guessed
        // from window focus. That guess is fine for handing focus back; it is
        // not good enough to write driver settings against. Let the user name
        // the process instead. This picks a DRIVER PROFILE TARGET only; it
        // does not change what we capture.
        int current = -1;
        for (size_t i = 0; i < windows_.size(); ++i) {
            if (windows_[i].stable_id() == s.profile_target_id) { current = (int)i; break; }
        }
        if (ImGui::BeginCombo("Game (for driver profile)", current >= 0 ?
                ToUtf8(windows_[current].title + L" [" +
                       windows_[current].process_basename + L"]").c_str() :
                "<select the game>")) {
            for (size_t i = 0; i < windows_.size(); ++i) {
                bool sel = ((int)i == current);
                std::string label = ToUtf8(windows_[i].title) + " [" +
                                    ToUtf8(windows_[i].process_basename) + "]";
                if (ImGui::Selectable(label.c_str(), sel)) {
                    s.profile_target_id = windows_[i].stable_id();
                    // Re-resolve immediately: the applied/not-applied state
                    // below is per executable, so picking a different game
                    // has to re-read it rather than keep showing the previous
                    // game's answer.
                    nvprofile_cache_tick_ = 0;
                    nvprofile_status_.clear();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Which process the frame cap below is written for.\n\n"
                "Capture is unaffected - Katanga always takes whatever the producer\n"
                "publishes. This only names the game for its NVIDIA driver profile,\n"
                "because the Katanga protocol carries no way to identify it.\n\n"
                "There is no auto-detect: the process could only be guessed from\n"
                "window focus, and a wrong guess would write driver settings for\n"
                "some other application.");
        }
    }

    if (ImGui::Button("Refresh sources")) RefreshSources();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-reacquire on disconnect", &s.auto_reacquire);

    if (s.source_kind != SourceKind::None || nvprofile_has_game_) {
        DrawGameProfileControl(app);
    }

    // Applies to any real capture source (WGC window/monitor AND Katanga) —
    // both floor when the mouse is idle and our popup occludes the game. Two
    // wake methods so users can try whichever works on their machine/game (or
    // both); each also opts the game out of Windows power throttling.
    if (s.source_kind == SourceKind::Window  ||
        s.source_kind == SourceKind::Monitor ||
        s.source_kind == SourceKind::Katanga) {
        ImGui::TextDisabled("Force full capture Hz (if idle capture floors):");
        ImGui::Checkbox("Cursor jiggle", &s.force_full_capture_hz);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Nudges the cursor +1/-1px (no net movement) every tick (~120Hz)\n"
                "to force DWM to keep compositing the occluded game, so WGC keeps\n"
                "delivering frames. Also opts the game out of Windows power\n"
                "throttling. This is the one that fixes the WGC floor, but the\n"
                "global cursor events can make a raw-input game's camera shimmer.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Game poke", &s.force_capture_hz_postmsg);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Posts a WM_MOUSEMOVE straight to the game window every tick\n"
                "(~120Hz) to keep its render loop pumping, plus the power-throttle\n"
                "opt-out. Gentler (no global cursor motion) and more portable\n"
                "across machines, but only helps games that idle on lack of input\n"
                "- not the WGC DWM-sampling floor.");
        }
    }
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

    // Eye-swap is a runtime-cheap toggle in AUTOMATIC mode (rewrites the
    // NV3D signature row on the next frame) so it doesn't need to Restart
    // the display. The other two below DO require teardown.
    if (ImGui::Checkbox("Swap eyes", &s.eye_swap)) {
        if (app.state() == AppState::Running) app.presenter().SetEyeSwap(s.eye_swap);
    }
    bool dirty = false;
    dirty |= ImGui::Checkbox("LightBoost timings",   &s.enable_lightboost);
    dirty |= ImGui::Checkbox("3DVision Driver Fix",  &s.enable_suppressor);
    ImGui::TextDisabled("(toggling LightBoost / Driver Fix reconnects the display)");

    ImGui::Spacing();

    // ---- Smoothness / sync ----
    // All three are D3D9 device-creation parameters, so they ride the same
    // `dirty` restart as LightBoost / Driver Fix above.
    {
        const char* sync_items[] = {
            "Vsync on (safe default)",
            "Vsync off (for driver Fast Sync)",
            "Half rate",
        };
        int sync_idx = (int)s.output_sync;
        if (ImGui::Combo("Output sync", &sync_idx, sync_items, IM_ARRAYSIZE(sync_items))) {
            const OutputSync prev = s.output_sync;
            s.output_sync = (sync_idx == 1) ? OutputSync::VsyncOff
                          : (sync_idx == 2) ? OutputSync::Half
                                            : OutputSync::VsyncOn;
            dirty = true;

            // Vsync off exists to hand the syncing to the driver, and does
            // nothing on its own - Fast Sync only engages for an application
            // that is not already vsync-limited. So choosing it IS the
            // request for the profile; apply it rather than leaving the user
            // with a mode that tears until they find a button. Ask once, then
            // act silently. A session is running only if the user changed
            // this mid-session, and the restart below handles that.
            const bool entering_vsync_off =
                (s.output_sync == OutputSync::VsyncOff && prev != OutputSync::VsyncOff);
            const bool leaving_vsync_off =
                (prev == OutputSync::VsyncOff && s.output_sync != OutputSync::VsyncOff);

            // Leaving Vsync off takes the Fast Sync profile back off. The
            // profile exists only to serve this mode, so the dropdown owns
            // both halves - scoped to our own settings, so a frame cap the
            // user applied to their game is untouched.
            if (leaving_vsync_off && s.auto_apply_nvprofile &&
                NvProfile::HasBackup(NvProfile::Scope::Self, app.ini_path().c_str())) {
                const bool session_live = app.presenter().IsActive() ||
                                          app.state() == AppState::Running;
                if (session_live) {
                    syncprofile_ok_ = false;
                    syncprofile_status_ =
                        "Stop the 3D session to remove the Fast Sync profile.";
                } else {
                    std::wstring msg;
                    NvProfile::Result r = NvProfile::Revert(
                        NvProfile::Scope::Self, app.ini_path().c_str(), &msg);
                    if (r == NvProfile::Result::NeedsElevation) {
                        r = NvProfile::RelaunchElevated(
                                NvProfile::RevertArgs(NvProfile::Scope::Self).c_str(), &msg)
                                ? NvProfile::Result::Ok : NvProfile::Result::Failed;
                    }
                    syncprofile_ok_ = (r == NvProfile::Result::Ok ||
                                       r == NvProfile::Result::Nothing);
                    syncprofile_status_ = ToUtf8(msg);
                    nvprofile_cache_tick_ = 0;
                    if (r == NvProfile::Result::Ok) ask_restart_ = true;
                }
            }

            if (entering_vsync_off && s.auto_apply_nvprofile) {
                const bool session_live = app.presenter().IsActive() ||
                                          app.state() == AppState::Running;
                if (session_live) {
                    // Never raise a UAC prompt while the fullscreen stereo
                    // output is up: the consent dialog fights it for
                    // foreground, and this change restarts the session in the
                    // same frame. The profile section disables itself during a
                    // session for the same reason - say so rather than
                    // silently doing nothing.
                    syncprofile_ok_ = false;
                    syncprofile_status_ =
                        "Stop the 3D session, then re-select Vsync off so Fast "
                        "Sync can be applied.";
                } else if (s.nvprofile_auto_confirmed) {
                    ApplySelfProfileNow(app);
                } else {
                    nvprofile_ask_autoapply_ = true;
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "A 3D Vision panel shows one stereo pair per TWO refreshes, so a\n"
                "120Hz panel caps at 60 stereo fps.\n\n"
                "Vsync on: tear-free, but a present that misses its slot waits for\n"
                "the next one - the classic DX9 rate halving.\n\n"
                "Vsync off: never throttled. Tears on its own - use it together with\n"
                "the Fast Sync driver profile below, which only engages for apps that\n"
                "are NOT already vsync-limited.\n\n"
                "Half rate: lock a source that cannot hold 60 to a steady 30 instead\n"
                "of an uneven cadence.");
        }

        ImGui::TextDisabled(
            "Sync/latency/FLIPEX apply on the next Start; the driver profile "
            "needs a restart.");

        if (ImGui::Checkbox("Fast Sync: also set the undocumented flip-interval flag",
                            &nvprofile_flip_interval_)) {
            // Only meaningful as part of the Fast Sync profile. If that
            // profile is already on, re-apply so the change takes effect now
            // rather than at some later, unrelated moment.
            const bool session_live = app.presenter().IsActive() ||
                                      app.state() == AppState::Running;
            if (!session_live && s.output_sync == OutputSync::VsyncOff &&
                NvProfile::HasBackup(NvProfile::Scope::Self, app.ini_path().c_str())) {
                ApplySelfProfileNow(app);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Adds Vsync - Behavior Flags = IGNORE_FLIPINTERVAL_MULTIPLE to the\n"
                "Fast Sync profile. Undocumented. 3D Vision doubles the flip interval\n"
                "to fit a stereo pair into two refreshes, and this is the driver's\n"
                "knob for that multiplication - worth trying, and switching Output\n"
                "sync away from \"Vsync off\" undoes it with the rest of the profile.\n\n"
                "This is a driver profile setting like Fast Sync itself, so it needs\n"
                "NV3D-Glass restarted - unlike the two settings above, which only\n"
                "need a Stop/Start.");
        }

        if (ImGui::SliderInt("Max frame latency", &s.max_frame_latency, 1, 3)) {
            dirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Driver present queue depth. 1 is the shortest and the historical\n"
                "value; 2-3 costs a frame of latency but absorbs a hitch that would\n"
                "otherwise drop a whole stereo pair.\n\n"
                "Applied when the 3D device is created, so a Stop/Start is enough -\n"
                "no need to restart NV3D-Glass.");
        }

        if (ImGui::Checkbox("FLIPEX swap chain (experimental)", &s.use_flipex)) {
            dirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Creates the output swap chain with D3DSWAPEFFECT_FLIPEX instead of\n"
                "DISCARD, normally a lower-latency present model.\n\n"
                "Unproven with 3D Vision: the driver demuxes left/right by SCANNING\n"
                "the presented surface, and this changes the present model underneath\n"
                "that scanner. If the image goes flat/mono, turn this back off.\n\n"
                "Applied when the 3D device is created, so a Stop/Start is enough -\n"
                "no need to restart NV3D-Glass.");
        }

        // Pacing is a live setting - the main loop reads it every iteration,
        // so it deliberately does NOT set `dirty`.
        int pacing_idx = (s.pacing_mode == PacingMode::Legacy) ? 1 : 0;
        const char* pacing_items[] = { "Present-clocked (default)", "Legacy fixed tick" };
        if (ImGui::Combo("Frame pacing", &pacing_idx, pacing_items, IM_ARRAYSIZE(pacing_items))) {
            s.pacing_mode = (pacing_idx == 1) ? PacingMode::Legacy : PacingMode::PresentClocked;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Present-clocked: capture one frame per completed 3D present, so the\n"
                "panel sets the pace. Legacy: the original free-running tick, which\n"
                "captured and submitted regardless of the display and beat against it.\n"
                "Applies immediately - no restart needed.");
        }

        // The Fast Sync driver profile is owned by the Output sync setting
        // above, so its state and any result message belong here rather than
        // in a section of their own. Values come from RefreshProfileCache.
        if (nvprofile_has_self_) {
            ImGui::TextDisabled("Fast Sync driver profile: applied");
        } else if (s.output_sync == OutputSync::VsyncOff) {
            // This combination tears: the mode hands syncing to the driver,
            // and the driver has nothing to sync with.
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.35f, 1.0f),
                "Fast Sync driver profile: NOT applied - this mode tears without it\n"
                "(switch Output sync away and back to apply it)");
        } else {
            ImGui::TextDisabled("Fast Sync driver profile: not applied");
        }
        if (!syncprofile_status_.empty()) {
            ImGui::TextColored(syncprofile_ok_ ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)
                                               : ImVec4(0.95f, 0.6f, 0.35f, 1.0f),
                               "%s", syncprofile_status_.c_str());
        }
    }


    if (dirty && app.state() == AppState::Running) app.Restart();

    ImGui::Spacing();

    // Cursor lock + 3D cursor. Both only take effect while the 3D output is
    // showing; the clip follows whichever external window has focus, so
    // alt+tabbing updates the lock region (and focusing our panel / the
    // desktop frees the pointer).
    ImGui::Checkbox("Lock cursor to captured window", &s.lock_cursor);

    ImGui::Checkbox("Draw 3D cursor", &s.draw_3d_cursor);
    if (s.draw_3d_cursor) {
        ImGui::SliderFloat("Cursor depth", &s.cursor_parallax, -0.05f, 0.05f, "%.3f");
        ImGui::SliderInt("Cursor size", &s.cursor_size, 12, 96, "%d px");
        ImGui::TextDisabled("(0 depth = screen plane; the OS cursor never appears in the 3D view)");
    }

    ImGui::Spacing();

    // Gate on whether the 3D output window (NV3DLib's FSE popup) is alive.
    // Belt-and-braces: presenter.IsActive() is the primary signal, but we
    // also accept state==Running as a fallback in case the two desync (the
    // earlier "Start stuck after launch" symptom). Either one being true
    // means we have a 3D window the user can Stop / Show.
    const bool fse_alive = app.presenter().IsActive()
                        || app.state() == AppState::Running;

    // One Start/Stop toggle button - label changes with state.
    if (ImGui::Button(fse_alive ? "Stop" : "Start", ImVec2(120, 32))) {
        if (fse_alive) app.Stop();
        else           app.Start();
    }
    ImGui::SameLine();
    // Mirrors Ctrl+F8 - re-show the FSE popup (and re-spawn the ForceFocus
    // watcher so the game gets foreground back). Toggles: press again or
    // Ctrl+F8 to hide.
    if (!fse_alive) ImGui::BeginDisabled();
    if (ImGui::Button("Show 3D Window", ImVec2(140, 32))) app.ToggleFseVisible();
    if (!fse_alive) ImGui::EndDisabled();

    if (!fse_alive && (s.source_kind == SourceKind::None || s.source_id.empty())) {
        ImGui::TextDisabled("(no source picked = test pattern)");
    }

    // One-time consent for the automatic apply, raised by the Output sync
    // combo above. OpenPopup and BeginPopupModal must sit at the same ImGui
    // ID stack level, which is why it is drawn here rather than where it is
    // triggered from.
    if (nvprofile_ask_autoapply_) {
        ImGui::OpenPopup("Apply Fast Sync profile?");
        nvprofile_ask_autoapply_ = false;
    }
    if (ImGui::BeginPopupModal("Apply Fast Sync profile?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "\"Vsync off\" hands the syncing to the NVIDIA driver, which needs\n"
            "Fast Sync enabled in NV3D-Glass's own driver profile - otherwise\n"
            "this mode just tears.\n\n"
            "Apply it now? Only this application's profile is touched, and\n"
            "switching Output sync back undoes it. Windows may ask for\n"
            "administrator approval.\n\n"
            "NV3D-Glass has to restart afterwards - the driver binds a profile\n"
            "while a process starts up, so this one cannot pick it up.");
        ImGui::Spacing();
        if (ImGui::Button("Apply", ImVec2(110, 0))) {
            s.nvprofile_auto_confirmed = true;   // don't ask again
            ApplySelfProfileNow(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Not now", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Never ask", ImVec2(110, 0))) {
            s.auto_apply_nvprofile = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The driver reads an application's profile as that process initialises
    // its graphics DLL. We create a D3D11 device during startup, so a profile
    // written now cannot affect the running process no matter how many times
    // the 3D session is stopped and started - only a new process can pick it
    // up. Offer that rather than leaving the setting looking applied but inert.
    if (ask_restart_) {
        ImGui::OpenPopup("Restart NV3D-Glass?");
        ask_restart_ = false;
    }
    if (ImGui::BeginPopupModal("Restart NV3D-Glass?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "The NVIDIA driver applies a profile while an application is\n"
            "starting up, so this change cannot take effect until NV3D-Glass\n"
            "is restarted. Stopping and starting the 3D session is not enough.\n\n"
            "Restart now? Your settings are saved first, and the 3D session is\n"
            "not running.");
        ImGui::Spacing();
        if (ImGui::Button("Restart now", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            app.RestartSelf();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Gui::ApplySelfProfileNow(App& app) {
    NvProfile::Options opt;
    opt.experimental_flip_interval = nvprofile_flip_interval_;
    std::wstring msg;
    // In-process first. Elevation is only needed when the DRS database's own
    // ACL refuses a plain user, which varies by machine - on many it never
    // does, and prompting unconditionally would be pure friction.
    NvProfile::Result r = NvProfile::ApplySelf(opt, app.ini_path().c_str(), &msg);
    if (r == NvProfile::Result::NeedsElevation) {
        r = NvProfile::RelaunchElevated(NvProfile::SelfArgs(opt).c_str(), &msg)
                ? NvProfile::Result::Ok : NvProfile::Result::Failed;
    }
    syncprofile_ok_ = (r == NvProfile::Result::Ok);
    syncprofile_status_ = ToUtf8(msg);
    nvprofile_cache_tick_ = 0;   // an undo record may now exist
    if (syncprofile_ok_) ask_restart_ = true;

}

std::wstring Gui::ResolveProfileTargetExe(App& app) const {
    const Settings& s = app.settings();

    // An explicit pick always wins - it is the whole point of the picker.
    if (!s.profile_target_id.empty()) {
        for (const WindowEntry& w : windows_) {
            if (w.stable_id() == s.profile_target_id) return w.process_basename;
        }
        // The window is gone (game closed, or renamed its title). The stable
        // id still carries the executable name, which is all DRS needs.
        const size_t bar = s.profile_target_id.find(L'|');
        if (bar != std::wstring::npos) return s.profile_target_id.substr(0, bar);
    }

    // Katanga deliberately has no fallback. The only other source of a
    // process here is inference from window focus, which is fine for handing
    // focus back but not for writing driver settings - a wrong guess would
    // modify some unrelated application's profile. Nothing picked, nothing
    // written.
    if (s.source_kind == SourceKind::Katanga) return std::wstring();

    // Window capture knows the game exactly: we resolved its HWND at Start.
    return app.TrackedGameExePath();
}

// Resolving the profile target can mean an OpenProcess and the backup checks
// are INI reads, so they run at ~2Hz rather than per frame. Refreshed from
// Draw() so both the capture and output sections see the same values - if the
// capture section owned the timer, the output section (which runs after it in
// the same frame) would always find the interval unexpired and never update.
void Gui::RefreshProfileCache(App& app) {
    const DWORD now_tick = GetTickCount();
    if (nvprofile_cache_tick_ != 0 && now_tick - nvprofile_cache_tick_ < 500) return;
    nvprofile_cache_tick_ = now_tick;
    const std::wstring& ini = app.ini_path();
    nvprofile_game_exe_ = ResolveProfileTargetExe(app);
    // Per executable, not "is any game capped" - otherwise switching games
    // leaves the button claiming the new one is already applied.
    nvprofile_has_game_ = !nvprofile_game_exe_.empty() &&
        NvProfile::HasBackup(NvProfile::Scope::Game, ini.c_str(),
                             nvprofile_game_exe_.c_str());
    nvprofile_has_self_ = NvProfile::HasBackup(NvProfile::Scope::Self, ini.c_str());
}

void Gui::ReconcileSyncProfileOnce(App& app) {
    if (profile_reconciled_) return;
    profile_reconciled_ = true;

    const Settings& s = app.settings();
    // Only when the user has already chosen this mode AND already agreed to
    // the automatic apply. This re-asserts a decision they made; it never
    // makes one for them.
    const bool want   = (s.output_sync == OutputSync::VsyncOff);
    const bool agreed = s.auto_apply_nvprofile && s.nvprofile_auto_confirmed;
    const bool idle   = !app.presenter().IsActive() && app.state() != AppState::Running;
    Log(NV3D::LogLevel::Info,
        L"Gui: sync-profile reconcile - vsync_off=%d agreed=%d already_applied=%d idle=%d",
        (int)want, (int)agreed, (int)nvprofile_has_self_, (int)idle);
    if (!want || !agreed || nvprofile_has_self_ || !idle) return;

    // In-process only: no elevation prompt. Raising UAC on the first frame,
    // unbidden, would be obnoxious - and on machines where these settings
    // need admin the warning under the dropdown stays visible instead.
    NvProfile::Options opt;
    opt.experimental_flip_interval = nvprofile_flip_interval_;
    std::wstring msg;
    const NvProfile::Result r = NvProfile::ApplySelf(opt, app.ini_path().c_str(), &msg);
    if (r == NvProfile::Result::Ok) {
        nvprofile_cache_tick_ = 0;
        Log(NV3D::LogLevel::Info,
            L"Gui: re-applied the Fast Sync profile for the persisted Vsync-off mode");
    } else {
        syncprofile_ok_ = false;
        syncprofile_status_ = ToUtf8(msg);
        Log(NV3D::LogLevel::Warning,
            L"Gui: sync-profile reconcile failed (%d) - %s", (int)r, msg.c_str());
    }
}

void Gui::DrawGameProfileControl(App& app) {
    const std::wstring& ini = app.ini_path();
    const std::wstring& game_exe = nvprofile_game_exe_;
    const int  stereo_hz     = app.StereoRateHz();
    const bool game_applied  = nvprofile_has_game_;

    // Writing driver settings can raise a UAC prompt. A consent dialog
    // stealing foreground from the fullscreen stereo output is exactly the
    // kind of focus fight that destabilises it, and the wait would stall the
    // present loop - so this is only live when no session is running.
    const bool session_live = app.presenter().IsActive() ||
                              app.state() == AppState::Running;
    if (session_live) ImGui::BeginDisabled();

    // One toggle rather than an apply button plus a global revert: undoing
    // the game cap must not also undo the Fast Sync profile that the Output
    // sync dropdown owns, and vice versa.
    char game_label[160]{};
    if (game_applied) {
        _snprintf_s(game_label, _TRUNCATE, "Remove game vsync + fps cap");
    } else {
        _snprintf_s(game_label, _TRUNCATE, "Apply vsync + %d fps cap to game", stereo_hz);
    }
    const bool game_disabled = !game_applied && game_exe.empty();
    if (game_disabled) ImGui::BeginDisabled();
    if (ImGui::Button(game_label, ImVec2(260, 0))) {
        std::wstring msg;
        NvProfile::Result r;
        if (game_applied) {
            r = NvProfile::Revert(NvProfile::Scope::Game, ini.c_str(), &msg,
                                  game_exe.c_str());
            if (r == NvProfile::Result::NeedsElevation) {
                r = NvProfile::RelaunchElevated(
                        NvProfile::RevertArgs(NvProfile::Scope::Game,
                                              game_exe.c_str()).c_str(), &msg)
                        ? NvProfile::Result::Ok : NvProfile::Result::Failed;
            }
        } else {
            r = NvProfile::ApplyGame(game_exe.c_str(), stereo_hz, ini.c_str(), &msg);
            if (r == NvProfile::Result::NeedsElevation) {
                r = NvProfile::RelaunchElevated(
                        NvProfile::GameArgs(game_exe.c_str(), stereo_hz).c_str(), &msg)
                        ? NvProfile::Result::Ok : NvProfile::Result::Failed;
            }
        }
        nvprofile_ok_ = (r == NvProfile::Result::Ok || r == NvProfile::Result::Nothing);
        nvprofile_status_ = ToUtf8(msg);
        nvprofile_cache_tick_ = 0;
    }
    if (game_disabled) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Caps the game to the panel's stereo rate (half its refresh) and"
            "forces vsync on for it.\n\n"
            "The single most effective setting for smoothness: the cap stops the\n"
            "game drawing frames that can never be shown, and vsync locks its\n"
            "presents to the same vblank train we present on, so each stereo pair\n"
            "carries one whole new frame instead of part of one.\n\n"
            "Written into whatever profile the game already belongs to, so its\n"
            "other settings are preserved. The same button removes it again.\n\n"
            "The driver applies a profile when the game creates its device, so\n"
            "the game must be RESTARTED before this changes anything.");
    }

    ImGui::SameLine();
    if (game_exe.empty()) {
        ImGui::TextDisabled(app.settings().source_kind == SourceKind::Katanga
                                ? "(pick the game above)"
                                : "(no game identified yet)");
    } else {
        ImGui::TextDisabled("%s", ToUtf8(game_exe).c_str());
    }

    if (session_live) ImGui::EndDisabled();
    if (session_live) {
        ImGui::TextDisabled("(stop the 3D session to change driver settings)");
    }
    // NVIDIA reads a profile when the application creates its device, so a
    // game that is already running keeps whatever it started with.
    ImGui::TextDisabled("Driver profile changes need the game restarted to take effect.");

    if (!nvprofile_status_.empty()) {
        ImGui::TextColored(nvprofile_ok_ ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)
                                         : ImVec4(0.95f, 0.6f, 0.35f, 1.0f),
                           "%s", nvprofile_status_.c_str());
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
    row("Show/hide 3D output",     4, s.hk_toggle_fse,   any_change);

    if (any_change) {
        app.hotkeys().Clear();
        app.settings().hk_eyeswap.registered    = app.hotkeys().Register(1, s.hk_eyeswap.mods,    s.hk_eyeswap.vk,    [&app](){ app.ToggleEyeSwap();    });
        app.settings().hk_toggle_fse.registered = app.hotkeys().Register(4, s.hk_toggle_fse.mods, s.hk_toggle_fse.vk, [&app](){ app.ToggleFseVisible(); });
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
