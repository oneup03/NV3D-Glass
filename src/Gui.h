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

#include "SourceEnumerator.h"

#include <Windows.h>
#include <string>
#include <vector>

namespace nv3dg {

class App;

class Gui {
public:
    void Draw(App& app);
    void RefreshSources();

private:
    void DrawCaptureSourceSection(App& app);
    void DrawOutputSection(App& app);
    // The game-side driver profile control, drawn in the capture section
    // because it targets the captured game, not the 3D output.
    void DrawGameProfileControl(App& app);
    // ~2Hz refresh of the cached profile-target / undo-record lookups.
    void RefreshProfileCache(App& app);
    // One-shot startup reconcile: a persisted "Vsync off" needs the Fast Sync
    // profile, but the coupling that applies it only fires on a CHANGE, so a
    // restart would otherwise leave the mode selected and silently inert.
    void ReconcileSyncProfileOnce(App& app);
    // Executable the game-side profile applies to: the explicitly picked
    // process when there is one, otherwise whatever the capture path managed
    // to identify. Empty when neither is known.
    std::wstring ResolveProfileTargetExe(App& app) const;
    // Apply our own Fast Sync profile, falling back to the elevated helper
    // only if the driver refuses. Shared by the button and the automatic
    // apply that rides on selecting Vsync off. Updates nvprofile_status_.
    void ApplySelfProfileNow(App& app);
    void DrawHotkeySection(App& app);
    void DrawStatusFooter(App& app);

    std::vector<WindowEntry>  windows_;
    std::vector<MonitorEntry> monitors_;
    bool                      capturing_chord_for_ = false;
    int                       chord_id_ = -1;

    // Result lines are kept per control, so each message appears next to the
    // thing that triggered it: the game profile lives in the capture section,
    // the Fast Sync profile is driven by the Output sync dropdown.
    std::string               nvprofile_status_;
    bool                      nvprofile_ok_ = true;
    std::string               syncprofile_status_;
    bool                      syncprofile_ok_ = true;
    // Mirrors NvProfile::Options - the one undocumented setting we offer,
    // kept out of the default set.
    bool                      nvprofile_flip_interval_ = false;
    // Cache for the two values this section would otherwise re-derive on
    // every GUI frame: the tracked game's image path (an OpenProcess +
    // QueryFullProcessImageName) and whether an undo record exists (an INI
    // read). Neither changes faster than a person can read, so they refresh
    // on a timer instead of at frame rate.
    // Set when the user picks Vsync off and hasn't yet agreed to the
    // automatic profile apply; drained into a modal on the next frame.
    bool                      nvprofile_ask_autoapply_ = false;
    // Raised after our own driver profile changes - only a new process picks
    // one up, so the user is offered a restart rather than left with a
    // setting that looks applied and does nothing.
    bool                      ask_restart_ = false;
    DWORD                     nvprofile_cache_tick_ = 0;
    std::wstring              nvprofile_game_exe_;
    // Tracked separately because the two are undone independently: the
    // Output sync dropdown owns the self profile, the game button owns its
    // own cap.
    bool                      nvprofile_has_self_ = false;
    bool                      nvprofile_has_game_ = false;
    bool                      profile_reconciled_ = false;
};

}  // namespace nv3dg
