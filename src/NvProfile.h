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

namespace nv3dg {

// Writes NVIDIA driver profile settings (NvAPI DRS) so the user doesn't have
// to reproduce them by hand in NVIDIA Control Panel / NVIDIA Inspector.
//
// Two distinct operations, because they carry very different risk:
//
//   ApplySelf - settings on OUR OWN executable's profile. Nothing else on the
//               machine is affected, so this is always safe to offer. The
//               headline setting is Fast Sync, which is not exposed in the
//               standard Control Panel at all; combined with the "Vsync off"
//               output sync mode it gives unthrottled, tear-free presents
//               without hard vsync's rate halving.
//
//   ApplyGame - settings on the CAPTURED GAME's profile: vsync on plus a
//               frame cap at the panel's stereo rate. This is the
//               configuration users have been discovering by hand, and it is
//               the single most effective thing for smoothness, because it
//               phase-locks the source to the same clock we present on. It
//               touches a profile the user may care about, so it is opt-in,
//               and it deliberately writes into whatever profile the exe
//               ALREADY belongs to rather than moving it into ours - moving
//               an application between profiles would silently drop every
//               other setting that profile applied to it.
//
// Every value we overwrite is backed up first (into a private section of the
// app's INI) so Revert can put the machine back exactly as it was, including
// deleting a profile or application entry that we created.
//
// Saving driver settings normally requires elevation: the driver returns
// NVAPI_ACCESS_DENIED to a non-elevated caller. That surfaces as
// NeedsElevation, and RelaunchElevated re-runs a single operation in an
// elevated copy of this exe that applies and exits.
class NvProfile {
public:
    enum class Result {
        Ok,              // applied and saved
        NeedsElevation,  // the driver refused the save without admin rights
        Unavailable,     // no NVIDIA driver / NvAPI unavailable on this machine
        Nothing,         // nothing to do (Revert with no backup)
        Failed,          // see the message
    };

    // Which set of changes an operation covers. The undo record tags every
    // entry, because the two operations are independent: leaving "Vsync off"
    // must undo the Fast Sync profile WITHOUT touching a frame cap the user
    // applied to their game, and vice versa.
    enum class Scope { Self, Game, All };

    struct Options {
        // Also write VSYNC_BEHAVIOR_FLAGS = IGNORE_FLIPINTERVAL_MULTIPLE.
        // Undocumented, and the one most likely to interact with the way the
        // 3D Vision driver doubles the flip interval for frame-sequential
        // output - which is exactly why it is worth having, and exactly why
        // it is behind its own opt-in.
        bool experimental_flip_interval = false;
    };

    // Apply the recommended profile to NV3D-Glass.exe.
    static Result ApplySelf(const Options& opt, const wchar_t* ini_path,
                            std::wstring* message);

    // Apply "vsync on + cap to fps_cap" to the profile owning game_exe_path.
    // fps_cap should be the panel's stereo rate (half the refresh).
    static Result ApplyGame(const wchar_t* game_exe_path, int fps_cap,
                            const wchar_t* ini_path, std::wstring* message);

    // Restore the values recorded for `scope` and drop those entries from the
    // record, leaving any other scope's entries intact.
    //
    // `exe` narrows a Game-scope operation to one executable. That matters
    // because the record can hold caps for several games at once: reverting
    // one must not disturb the others, and the UI needs to know whether THIS
    // game is capped rather than whether any game is. Null means every
    // executable in the scope. Ignored for Scope::Self.
    static Result Revert(Scope scope, const wchar_t* ini_path, std::wstring* message,
                         const wchar_t* exe = nullptr);

    // True when unreverted changes are on record for `scope` (and `exe`).
    static bool HasBackup(Scope scope, const wchar_t* ini_path,
                          const wchar_t* exe = nullptr);

    // Human-readable summary of what is currently backed up, for the UI.
    static std::wstring DescribeBackup(Scope scope, const wchar_t* ini_path,
                                       const wchar_t* exe = nullptr);

    // Is this process already elevated?
    static bool IsElevated();

    // Re-launch this executable elevated with `args`, wait for it to finish,
    // and report. Blocks the caller for as long as the UAC prompt is up, so
    // never call it while a 3D session is live. Returns false with a message
    // if the user declined the prompt or the child failed.
    static bool RelaunchElevated(const wchar_t* args, std::wstring* message);

    // Command-line entry point for the elevated child. Returns true if `argv`
    // contained a profile command (in which case the process should perform
    // it and exit without starting the UI). The performed command's result is
    // written to the log.
    static bool RunCommandLine(const wchar_t* command_line,
                               const wchar_t* ini_path, int* exit_code);

    // The argument strings RelaunchElevated expects; also what RunCommandLine
    // parses. Exposed so the UI can build them without duplicating literals.
    static std::wstring SelfArgs(const Options& opt);
    static std::wstring GameArgs(const wchar_t* game_exe_path, int fps_cap);
    static std::wstring RevertArgs(Scope scope, const wchar_t* exe = nullptr);
};

}  // namespace nv3dg
