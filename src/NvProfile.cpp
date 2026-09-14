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

#include "NvProfile.h"

#include "Logging.h"

#include "nvapi.h"
#include "NvApiDriverSettings.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nv3dg {

namespace {

// Profile we create for ourselves when one doesn't already exist.
constexpr const wchar_t* kSelfProfileName = L"NV3D-Glass";

// Private INI section holding the undo record. Deliberately not the main
// settings section: SaveSettings rewrites that wholesale on every shutdown,
// and this has to survive independently of it.
constexpr const wchar_t* kBackupSection = L"NvProfileBackup";

// Command-line verbs for the elevated child.
constexpr const wchar_t* kCmdFlag   = L"--apply-nvprofile";
constexpr const wchar_t* kCmdSelf   = L"self";
constexpr const wchar_t* kCmdGame   = L"game";
constexpr const wchar_t* kCmdRevert = L"revert";

static_assert(sizeof(wchar_t) == sizeof(NvU16),
              "NvAPI_UnicodeString is UTF-16; this code memcpy's wchar_t into it");

void ToNvStr(NvAPI_UnicodeString& dst, const wchar_t* src) {
    std::memset(dst, 0, sizeof(NvAPI_UnicodeString));
    if (!src) return;
    size_t n = wcslen(src);
    if (n > NVAPI_UNICODE_STRING_MAX - 1) n = NVAPI_UNICODE_STRING_MAX - 1;
    std::memcpy(dst, src, n * sizeof(wchar_t));
}

std::wstring FromNvStr(const NvAPI_UnicodeString& src) {
    const wchar_t* p = reinterpret_cast<const wchar_t*>(src);
    size_t n = 0;
    while (n < NVAPI_UNICODE_STRING_MAX && p[n] != 0) ++n;
    return std::wstring(p, n);
}

std::wstring BaseName(const wchar_t* path) {
    if (!path) return std::wstring();
    std::wstring p(path);
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? p : p.substr(slash + 1);
}

std::wstring SelfExePath() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
}

// ---------------------------------------------------------------------------
// Undo record
// ---------------------------------------------------------------------------
//
// One line per setting we overwrote:
//     b<i>=<profileName>|<settingIdHex>|<had>|<previousValue>
// `had` is 1 when the profile already carried a user value for that setting
// (so Revert writes it back) and 0 when it did not (so Revert clears ours and
// lets the driver default apply again).
//
// Plus the things we created, which Revert removes outright:
//     created_profile=<name or empty>
//     created_app=<appName or empty>
struct BackupEntry {
    std::wstring tag;          // "self" or "game" - see NvProfile::Scope
    // The executable this entry was written for. The profile name alone is
    // not enough: several games can share a profile, and the UI has to answer
    // "is THIS game capped" rather than "is some game capped".
    std::wstring exe;
    std::wstring profile;
    NvU32        setting_id = 0;
    bool         had_value  = false;
    NvU32        value      = 0;
};

// The tag stored in the undo record. Scope::All is not a tag - it is a
// selector meaning "every tag" - so it has no entry here; ScopeName() is the
// one to use for logging.
const wchar_t* ScopeTag(NvProfile::Scope s) {
    return s == NvProfile::Scope::Game ? L"game" : L"self";
}

const wchar_t* ScopeName(NvProfile::Scope s) {
    switch (s) {
        case NvProfile::Scope::Game: return L"game";
        case NvProfile::Scope::Self: return L"self";
        default:                     return L"all";
    }
}

bool ScopeMatches(NvProfile::Scope s, const std::wstring& tag) {
    if (s == NvProfile::Scope::All) return true;
    return tag == ScopeTag(s);
}

// An entry is selected when its scope matches and, for a Game-scope query
// that named an executable, when it belongs to that executable. Entries from
// the older format carry no exe and deliberately do NOT match a named one -
// better to leave such a row for an all-scopes revert than to attribute it to
// a game it may not belong to.
bool EntryMatches(NvProfile::Scope scope, const wchar_t* exe, const BackupEntry& e) {
    if (!ScopeMatches(scope, e.tag)) return false;
    if (!exe || !*exe) return true;
    if (e.tag != ScopeTag(NvProfile::Scope::Game)) return true;
    return !e.exe.empty() && _wcsicmp(e.exe.c_str(), exe) == 0;
}

// Per-scope keys for the things an apply had to create, so reverting one
// scope can't delete a profile the other scope is still using.
std::wstring CreatedProfileKey(const std::wstring& tag) { return L"created_profile." + tag; }
std::wstring CreatedAppKey(const std::wstring& tag)     { return L"created_app." + tag; }

std::wstring IniGet(const wchar_t* key, const wchar_t* def, const wchar_t* path) {
    wchar_t buf[1024]{};
    GetPrivateProfileStringW(kBackupSection, key, def, buf, _countof(buf), path);
    return std::wstring(buf);
}

void IniSet(const wchar_t* key, const wchar_t* value, const wchar_t* path) {
    WritePrivateProfileStringW(kBackupSection, key, value, path);
}

// Flush the OS-side INI write cache. Without this, a value written by the
// elevated child isn't reliably visible to the parent process that spawned it.
void IniFlush(const wchar_t* path) {
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
}

// out_unparsed (optional) receives the number of rows the record claims to
// hold but that we could not read. A caller about to REWRITE the record
// must treat that as fatal: overwriting rows we failed to parse destroys
// the only undo information for driver settings that are still applied,
// making them permanent. (Learned the hard way: a record written in an
// older row format parsed as zero entries and was silently truncated.)
std::vector<BackupEntry> LoadBackup(const wchar_t* ini_path,
                                    int* out_unparsed = nullptr) {
    std::vector<BackupEntry> out;
    if (out_unparsed) *out_unparsed = 0;
    if (!ini_path || !*ini_path) return out;
    const int count = GetPrivateProfileIntW(kBackupSection, L"count", 0, ini_path);
    for (int i = 0; i < count; ++i) {
        wchar_t key[32]{};
        _snwprintf_s(key, _TRUNCATE, L"b%d", i);
        const std::wstring line = IniGet(key, L"", ini_path);
        if (line.empty()) { if (out_unparsed) ++*out_unparsed; continue; }

        // tag|profile|settingIdHex|had|value
        std::vector<std::wstring> f;
        size_t start = 0;
        for (;;) {
            const size_t bar = line.find(L'|', start);
            if (bar == std::wstring::npos) { f.push_back(line.substr(start)); break; }
            f.push_back(line.substr(start, bar - start));
            start = bar + 1;
        }
        if (f.size() == 5) {
            // Legacy row: tag|profile|settingIdHex|had|value, no exe.
            f.insert(f.begin() + 1, std::wstring());
        }
        if (f.size() != 6) {
            Log(NV3D::LogLevel::Warning,
                L"NvProfile: undo record row %s is unreadable ('%s') - refusing to "
                L"overwrite the record", key, line.c_str());
            if (out_unparsed) ++*out_unparsed;
            continue;
        }

        BackupEntry e;
        e.tag        = f[0];
        e.exe        = f[1];
        e.profile    = f[2];
        e.setting_id = (NvU32)wcstoul(f[3].c_str(), nullptr, 16);
        e.had_value  = (f[4] == L"1");
        e.value      = (NvU32)wcstoul(f[5].c_str(), nullptr, 10);
        out.push_back(std::move(e));
    }
    // Deliberately not logged on success: the UI polls this a few times a
    // second, so a routine line here floods the log. Unreadable rows warn
    // above, which is the case that actually matters.
    return out;
}

void SaveBackup(const std::vector<BackupEntry>& entries,
                const std::wstring& tag,
                const std::wstring& created_profile,
                const std::wstring& created_app,
                const wchar_t* ini_path) {
    if (!ini_path || !*ini_path) return;

    // Clear any stale rows past the new count so a shorter record can't leave
    // orphans behind that a later Revert would try to apply.
    const int old_count = GetPrivateProfileIntW(kBackupSection, L"count", 0, ini_path);
    for (int i = (int)entries.size(); i < old_count; ++i) {
        wchar_t key[32]{};
        _snwprintf_s(key, _TRUNCATE, L"b%d", i);
        IniSet(key, nullptr, ini_path);
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        wchar_t key[32]{};
        _snwprintf_s(key, _TRUNCATE, L"b%zu", i);
        wchar_t line[1024]{};
        _snwprintf_s(line, _TRUNCATE, L"%s|%s|%s|%08X|%d|%u",
                     entries[i].tag.c_str(), entries[i].exe.c_str(),
                     entries[i].profile.c_str(), entries[i].setting_id,
                     entries[i].had_value ? 1 : 0, entries[i].value);
        IniSet(key, line, ini_path);
    }
    wchar_t cbuf[32]{};
    _snwprintf_s(cbuf, _TRUNCATE, L"%zu", entries.size());
    IniSet(L"count", cbuf, ini_path);
    if (!tag.empty()) {
        if (!created_profile.empty()) {
            IniSet(CreatedProfileKey(tag).c_str(), created_profile.c_str(), ini_path);
        }
        if (!created_app.empty()) {
            IniSet(CreatedAppKey(tag).c_str(), created_app.c_str(), ini_path);
        }
    }
    IniFlush(ini_path);
}

void ClearBackup(const wchar_t* ini_path) {
    if (!ini_path || !*ini_path) return;
    // Deleting the whole section drops every b<i>, the count and the
    // created_* markers in one call.
    WritePrivateProfileStringW(kBackupSection, nullptr, nullptr, ini_path);
    IniFlush(ini_path);
}

// ---------------------------------------------------------------------------
// DRS session wrapper
// ---------------------------------------------------------------------------
class DrsSession {
public:
    ~DrsSession() {
        if (session_) NvAPI_DRS_DestroySession(session_);
    }

    // NVAPI_OK, or a failure code the caller maps to a Result.
    NvAPI_Status Open() {
        NvAPI_Status s = NvAPI_Initialize();
        if (s != NVAPI_OK) return s;
        s = NvAPI_DRS_CreateSession(&session_);
        if (s != NVAPI_OK) return s;
        return NvAPI_DRS_LoadSettings(session_);
    }

    NvDRSSessionHandle get() const { return session_; }

    NvAPI_Status Save() { return NvAPI_DRS_SaveSettings(session_); }

private:
    NvDRSSessionHandle session_ = nullptr;
};

// Read a setting's CURRENT user value on a profile. out_has is false when the
// profile carries no user value for it (the driver default is in force), which
// is the case Revert has to restore by clearing rather than rewriting.
bool ReadSetting(NvDRSSessionHandle h, NvDRSProfileHandle prof, NvU32 id,
                 bool* out_has, NvU32* out_value) {
    NVDRS_SETTING s{};
    s.version = NVDRS_SETTING_VER;
    const NvAPI_Status st = NvAPI_DRS_GetSetting(h, prof, id, &s);
    if (st != NVAPI_OK) {
        *out_has = false;
        *out_value = 0;
        // NVAPI_SETTING_NOT_FOUND just means "no user value here", which is
        // information, not an error.
        return st == NVAPI_SETTING_NOT_FOUND;
    }
    // settingLocation tells us whether the value actually lives on this
    // profile or is being inherited from the global/base/default profile.
    // Only a value stored on THIS profile is one we are overwriting and must
    // write back verbatim on Revert; an inherited one is restored correctly
    // by simply deleting our override, which is what had_value=false means.
    //
    // isCurrentPredefined additionally distinguishes NVIDIA's own shipped
    // value for a predefined profile from a user edit of it.
    *out_has = (s.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION) &&
               (s.isCurrentPredefined == 0);
    *out_value = s.u32CurrentValue;
    Log(NV3D::LogLevel::Info,
        L"NvProfile: read 0x%08X value=%u location=%d predefined=%d -> %s",
        id, s.u32CurrentValue, (int)s.settingLocation, (int)s.isCurrentPredefined,
        *out_has ? L"profile-local (restore on revert)"
                 : L"inherited/default (clear on revert)");
    return true;
}

// Read-only audit of what we would touch, for --apply-nvprofile inspect.
void InspectSettings(NvDRSSessionHandle h, NvDRSProfileHandle prof,
                     const std::wstring& prof_name) {
    static const NvU32 ids[] = {
        VSYNCMODE_ID, PRERENDERLIMIT_ID, PREFERRED_PSTATE_ID,
        VSYNC_BEHAVIOR_FLAGS_ID, FRL_FPS_ID,
    };
    Log(NV3D::LogLevel::Info, L"NvProfile: inspecting profile '%s'", prof_name.c_str());
    for (NvU32 id : ids) {
        bool had = false;
        NvU32 v = 0;
        ReadSetting(h, prof, id, &had, &v);
    }
}

// Returns the NvAPI status rather than a bool: NVAPI_ACCESS_DENIED here has
// to reach the caller so it can offer elevation.
//
// Elevation is decided PER SETTING, not per session. Measured on one machine:
// the general 3D settings this module writes (VSYNCMODE, PRERENDERLIMIT,
// PREFERRED_PSTATE, FRL_FPS) save fine as a plain user, while the stereo
// block - StereoProfile 0x701EB457 and StereoHiddenProfile 0x70E46F20, as
// written by 3DVision4All's EnableWindowed3D tool - returns ACCESS_DENIED on
// that same machine. So a successful SaveSettings proves nothing on its own:
// SetSetting can refuse individual settings and leave the save to succeed
// having written nothing, which would otherwise be reported as success.
NvAPI_Status WriteSetting(NvDRSSessionHandle h, NvDRSProfileHandle prof,
                          NvU32 id, NvU32 value) {
    NVDRS_SETTING s{};
    s.version         = NVDRS_SETTING_VER;
    s.settingId       = id;
    s.settingType     = NVDRS_DWORD_TYPE;
    s.u32CurrentValue = value;
    const NvAPI_Status st = NvAPI_DRS_SetSetting(h, prof, &s);
    if (st != NVAPI_OK) {
        Log(NV3D::LogLevel::Warning,
            L"NvProfile: SetSetting(0x%08X = %u) failed code=%d%s",
            id, value, (int)st,
            st == NVAPI_ACCESS_DENIED ? L" (needs elevation)" : L"");
    }
    return st;
}

std::wstring ProfileName(NvDRSSessionHandle h, NvDRSProfileHandle prof) {
    NVDRS_PROFILE info{};
    info.version = NVDRS_PROFILE_VER;
    if (NvAPI_DRS_GetProfileInfo(h, prof, &info) != NVAPI_OK) return std::wstring();
    return FromNvStr(info.profileName);
}

// Apply `settings` to `prof`, recording each previous value into `backup`.
// Returns NVAPI_OK only if every setting was written. On failure returns the
// first failing status, so an ACCESS_DENIED on any single setting surfaces as
// NeedsElevation instead of a false success.
NvAPI_Status ApplyRecorded(NvDRSSessionHandle h, NvDRSProfileHandle prof,
                           const std::wstring& prof_name,
                           const std::wstring& tag,
                           const std::wstring& exe,
                           const std::vector<std::pair<NvU32, NvU32>>& settings,
                           std::vector<BackupEntry>& backup) {
    NvAPI_Status first_failure = NVAPI_OK;
    for (const auto& kv : settings) {
        // Never record a second undo entry for a setting we have already
        // overwritten. Applying twice (easy to do now that selecting "Vsync
        // off" can apply automatically) would otherwise capture OUR value as
        // the "previous" one, and Revert would faithfully restore Fast Sync
        // instead of clearing it. The first entry holds the true original.
        const bool already_recorded =
            std::any_of(backup.begin(), backup.end(), [&](const BackupEntry& b) {
                return b.setting_id == kv.first && b.profile == prof_name &&
                       b.tag == tag && _wcsicmp(b.exe.c_str(), exe.c_str()) == 0;
            });

        BackupEntry e;
        e.tag        = tag;
        e.exe        = exe;
        e.profile    = prof_name;
        e.setting_id = kv.first;
        ReadSetting(h, prof, kv.first, &e.had_value, &e.value);

        // Second guard, independent of the undo record: never record a
        // "previous" value that is already the value we are about to write.
        // That can only mean this setting is already applied, and recording
        // it would make Revert restore OUR value instead of clearing it -
        // i.e. the change would become permanent. The `already_recorded`
        // check above normally catches a re-apply, but it relies on reading
        // back the INI, and a re-apply from a second process moments later
        // can read a stale copy. This check needs no state at all.
        //
        // Cost of the false positive: if the user genuinely had this exact
        // value set themselves before we ever ran, Revert clears it to
        // inherited rather than restoring it explicitly. That is a far
        // smaller harm than an un-revertable change.
        const bool value_already_ours = e.had_value && (e.value == kv.second);

        const NvAPI_Status st = WriteSetting(h, prof, kv.first, kv.second);
        if (st == NVAPI_OK) {
            if (!already_recorded && !value_already_ours) {
                backup.push_back(std::move(e));
            }
        } else if (first_failure == NVAPI_OK) {
            first_failure = st;
        }
    }
    return first_failure;
}

NvProfile::Result MapOpenFailure(NvAPI_Status st, std::wstring* message) {
    if (st == NVAPI_ACCESS_DENIED) {
        if (message) *message = L"the driver refused access - run as administrator";
        return NvProfile::Result::NeedsElevation;
    }
    if (message) {
        wchar_t buf[160]{};
        _snwprintf_s(buf, _TRUNCATE,
                     L"NVIDIA driver settings unavailable (NvAPI code %d)", (int)st);
        *message = buf;
    }
    return NvProfile::Result::Unavailable;
}

NvProfile::Result MapSaveFailure(NvAPI_Status st, std::wstring* message) {
    if (st == NVAPI_ACCESS_DENIED) {
        if (message) {
            *message = L"saving driver settings needs administrator rights";
        }
        return NvProfile::Result::NeedsElevation;
    }
    if (message) {
        wchar_t buf[160]{};
        _snwprintf_s(buf, _TRUNCATE, L"could not save driver settings (NvAPI code %d)",
                     (int)st);
        *message = buf;
    }
    return NvProfile::Result::Failed;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------

bool NvProfile::IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size) != 0;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

NvProfile::Result NvProfile::ApplySelf(const Options& opt, const wchar_t* ini_path,
                                       std::wstring* message) {
    DrsSession sess;
    if (const NvAPI_Status st = sess.Open(); st != NVAPI_OK) {
        return MapOpenFailure(st, message);
    }

    const std::wstring exe = BaseName(SelfExePath().c_str());
    if (exe.empty()) {
        if (message) *message = L"could not determine our own executable name";
        return Result::Failed;
    }

    // Find the profile our exe already belongs to; only create one if it has
    // none. Same reasoning as ApplyGame - never move an application between
    // profiles behind the user's back.
    NvDRSProfileHandle prof = nullptr;
    std::wstring created_profile, created_app;
    {
        NvAPI_UnicodeString app_name{};
        ToNvStr(app_name, exe.c_str());
        NVDRS_APPLICATION app{};
        app.version = NVDRS_APPLICATION_VER;
        if (NvAPI_DRS_FindApplicationByName(sess.get(), app_name, &prof, &app) != NVAPI_OK) {
            prof = nullptr;
        }
    }
    if (!prof) {
        NvAPI_UnicodeString pname{};
        ToNvStr(pname, kSelfProfileName);
        if (NvAPI_DRS_FindProfileByName(sess.get(), pname, &prof) != NVAPI_OK) {
            NVDRS_PROFILE pi{};
            pi.version = NVDRS_PROFILE_VER;
            ToNvStr(pi.profileName, kSelfProfileName);
            if (NvAPI_DRS_CreateProfile(sess.get(), &pi, &prof) != NVAPI_OK || !prof) {
                if (message) *message = L"could not create the NV3D-Glass driver profile";
                return Result::Failed;
            }
            created_profile = kSelfProfileName;
        }
        NVDRS_APPLICATION app{};
        app.version      = NVDRS_APPLICATION_VER;
        app.isPredefined = 0;
        ToNvStr(app.appName,          exe.c_str());
        ToNvStr(app.userFriendlyName, L"NV3D-Glass");
        if (NvAPI_DRS_CreateApplication(sess.get(), prof, &app) == NVAPI_OK) {
            created_app = exe;
        }
    }

    const std::wstring prof_name = ProfileName(sess.get(), prof);

    // Fast Sync (VSYNCMODE_VIRTUAL) is the headline: unthrottled rendering
    // with only the newest completed frame shown at each vblank, so no
    // tearing and none of hard vsync's rate halving. It is not exposed in the
    // standard Control Panel - writing the profile value is the only way to
    // get it short of NVIDIA Inspector. It only takes effect for an app that
    // is NOT vsync-limited itself, hence the "Vsync off" output sync mode.
    std::vector<std::pair<NvU32, NvU32>> settings = {
        { VSYNCMODE_ID,      (NvU32)VSYNCMODE_VIRTUAL },
        // Shortest present queue the driver will keep - latency, and it stops
        // the driver buffering frames we have already superseded.
        { PRERENDERLIMIT_ID, 1u },
        // Keep clocks up. A presenter that does very little GPU work per
        // frame can otherwise sit in a low P-state and miss vblanks.
        { PREFERRED_PSTATE_ID, (NvU32)PREFERRED_PSTATE_PREFER_MAX },
    };
    if (opt.experimental_flip_interval) {
        settings.emplace_back((NvU32)VSYNC_BEHAVIOR_FLAGS_ID,
                              (NvU32)VSYNC_BEHAVIOR_FLAGS_IGNORE_FLIPINTERVAL_MULTIPLE);
    }

    int unparsed = 0;
    std::vector<BackupEntry> backup = LoadBackup(ini_path, &unparsed);
    if (unparsed > 0) {
        // Saving now would drop those rows and strand whatever they describe.
        if (message) {
            *message = L"the undo record is unreadable - revert or remove the "
                       L"[NvProfileBackup] section before applying again";
        }
        return Result::Failed;
    }

    // Turning the optional flag OFF must actively remove it. Leaving it out of
    // `settings` merely stops us rewriting a value that is already on the
    // profile - the checkbox would read "off" while the setting stayed live.
    if (!opt.experimental_flip_interval) {
        for (auto it = backup.begin(); it != backup.end(); ) {
            const bool ours = it->tag == ScopeTag(Scope::Self) &&
                              it->profile == prof_name &&
                              it->setting_id == (NvU32)VSYNC_BEHAVIOR_FLAGS_ID;
            if (!ours) { ++it; continue; }
            if (it->had_value) {
                WriteSetting(sess.get(), prof, it->setting_id, it->value);
            } else {
                NvAPI_Status d = NvAPI_DRS_DeleteProfileSetting(sess.get(), prof,
                                                                it->setting_id);
                if (d != NVAPI_OK) {
                    NvAPI_DRS_RestoreProfileDefaultSetting(sess.get(), prof,
                                                           it->setting_id);
                }
            }
            Log(NV3D::LogLevel::Info,
                L"NvProfile: removed the flip-interval flag from '%s'", prof_name.c_str());
            it = backup.erase(it);
        }
    }
    const std::wstring tag = ScopeTag(NvProfile::Scope::Self);
    if (const NvAPI_Status st = ApplyRecorded(sess.get(), prof, prof_name, tag, exe,
                                              settings, backup);
        st != NVAPI_OK) {
        // Persist whatever DID get written so Revert can still undo it, then
        // report the failure rather than claiming success.
        SaveBackup(backup, tag, created_profile, created_app, ini_path);
        sess.Save();
        return MapSaveFailure(st, message);
    }

    if (const NvAPI_Status st = sess.Save(); st != NVAPI_OK) {
        return MapSaveFailure(st, message);
    }
    SaveBackup(backup, tag, created_profile, created_app, ini_path);

    Log(NV3D::LogLevel::Info,
        L"NvProfile: applied self profile to '%s' (app=%s, fast-sync, prerender=1, "
        L"max-perf%s)",
        prof_name.c_str(), exe.c_str(),
        opt.experimental_flip_interval ? L", flip-interval-flag" : L"");
    if (message) {
        *message = L"Applied to profile '" + prof_name + L"'. Takes effect on the "
                   L"next Start (the 3D device picks it up when it is created).";
    }
    return Result::Ok;
}

NvProfile::Result NvProfile::ApplyGame(const wchar_t* game_exe_path, int fps_cap,
                                       const wchar_t* ini_path, std::wstring* message) {
    const std::wstring exe = BaseName(game_exe_path);
    if (exe.empty()) {
        if (message) *message = L"no game executable known - start a session first";
        return Result::Failed;
    }
    if (fps_cap < 20 || fps_cap > 1000) {
        if (message) *message = L"frame cap out of range";
        return Result::Failed;
    }

    DrsSession sess;
    if (const NvAPI_Status st = sess.Open(); st != NVAPI_OK) {
        return MapOpenFailure(st, message);
    }

    // Write into whatever profile already owns this exe. Creating our own and
    // adding the exe to it would MOVE the application - an app belongs to
    // exactly one profile - and silently discard every other setting the
    // game's existing (often NVIDIA-predefined) profile was applying.
    NvDRSProfileHandle prof = nullptr;
    std::wstring created_profile, created_app;
    {
        NvAPI_UnicodeString app_name{};
        ToNvStr(app_name, exe.c_str());
        NVDRS_APPLICATION app{};
        app.version = NVDRS_APPLICATION_VER;
        if (NvAPI_DRS_FindApplicationByName(sess.get(), app_name, &prof, &app) != NVAPI_OK) {
            prof = nullptr;
        }
    }
    if (!prof) {
        // The game has no profile of its own - make one just for it, named
        // after the exe so it is obvious in Control Panel where it came from.
        const std::wstring pname_s = L"NV3D-Glass: " + exe;
        NvAPI_UnicodeString pname{};
        ToNvStr(pname, pname_s.c_str());
        if (NvAPI_DRS_FindProfileByName(sess.get(), pname, &prof) != NVAPI_OK) {
            NVDRS_PROFILE pi{};
            pi.version = NVDRS_PROFILE_VER;
            ToNvStr(pi.profileName, pname_s.c_str());
            if (NvAPI_DRS_CreateProfile(sess.get(), &pi, &prof) != NVAPI_OK || !prof) {
                if (message) *message = L"could not create a driver profile for the game";
                return Result::Failed;
            }
            created_profile = pname_s;
        }
        NVDRS_APPLICATION app{};
        app.version      = NVDRS_APPLICATION_VER;
        app.isPredefined = 0;
        ToNvStr(app.appName,          exe.c_str());
        ToNvStr(app.userFriendlyName, exe.c_str());
        if (NvAPI_DRS_CreateApplication(sess.get(), prof, &app) == NVAPI_OK) {
            created_app = exe;
        }
    }

    const std::wstring prof_name = ProfileName(sess.get(), prof);

    // Vsync on plus a cap at the panel's stereo rate. This is the pair that
    // matters: the cap stops the game producing frames we can never show, and
    // vsync phase-locks its presents to the same vblank train we present on,
    // so our capture sees one whole new frame per stereo pair instead of a
    // rolling fraction of one.
    const std::vector<std::pair<NvU32, NvU32>> settings = {
        { VSYNCMODE_ID, (NvU32)VSYNCMODE_FORCEON },
        { FRL_FPS_ID,   (NvU32)fps_cap },
    };

    int unparsed = 0;
    std::vector<BackupEntry> backup = LoadBackup(ini_path, &unparsed);
    if (unparsed > 0) {
        // Saving now would drop those rows and strand whatever they describe.
        if (message) {
            *message = L"the undo record is unreadable - revert or remove the "
                       L"[NvProfileBackup] section before applying again";
        }
        return Result::Failed;
    }
    const std::wstring tag = ScopeTag(NvProfile::Scope::Game);
    if (const NvAPI_Status st = ApplyRecorded(sess.get(), prof, prof_name, tag, exe,
                                              settings, backup);
        st != NVAPI_OK) {
        SaveBackup(backup, tag, created_profile, created_app, ini_path);
        sess.Save();
        return MapSaveFailure(st, message);
    }

    if (const NvAPI_Status st = sess.Save(); st != NVAPI_OK) {
        return MapSaveFailure(st, message);
    }
    SaveBackup(backup, tag, created_profile, created_app, ini_path);

    Log(NV3D::LogLevel::Info,
        L"NvProfile: applied game profile to '%s' (app=%s, vsync on, cap=%d)",
        prof_name.c_str(), exe.c_str(), fps_cap);
    if (message) {
        wchar_t buf[512]{};
        _snwprintf_s(buf, _TRUNCATE,
                     L"Applied vsync on + %d fps cap to '%s' (profile '%s'). "
                     L"RESTART THE GAME for it to take effect.",
                     fps_cap, exe.c_str(), prof_name.c_str());
        *message = buf;
    }
    return Result::Ok;
}

NvProfile::Result NvProfile::Revert(Scope scope, const wchar_t* ini_path,
                                    std::wstring* message, const wchar_t* exe) {
    const std::vector<BackupEntry> all = LoadBackup(ini_path);

    // Split the record: entries this scope owns get reverted, the rest are
    // written back untouched. Reverting the Fast Sync profile must never
    // disturb a frame cap the user applied to their game.
    std::vector<BackupEntry> mine, keep;
    for (const BackupEntry& e : all) {
        (EntryMatches(scope, exe, e) ? mine : keep).push_back(e);
    }

    // Created-object markers are per scope for the same reason.
    struct CreatedRec { std::wstring tag, profile, app; };
    std::vector<CreatedRec> created;
    for (const wchar_t* tag : { L"self", L"game" }) {
        if (!ScopeMatches(scope, tag)) continue;
        // Narrowing to one executable leaves the scope's other entries in
        // place, so anything the scope created is still in use - only a
        // whole-scope revert may remove it.
        if (exe && *exe) continue;
        CreatedRec c;
        c.tag     = tag;
        c.profile = IniGet(CreatedProfileKey(tag).c_str(), L"", ini_path);
        c.app     = IniGet(CreatedAppKey(tag).c_str(),     L"", ini_path);
        if (!c.profile.empty() || !c.app.empty()) created.push_back(std::move(c));
    }

    if (mine.empty() && created.empty()) {
        if (message) *message = L"nothing to revert";
        return Result::Nothing;
    }

    DrsSession sess;
    if (const NvAPI_Status st = sess.Open(); st != NVAPI_OK) {
        return MapOpenFailure(st, message);
    }

    int restored = 0;
    for (const BackupEntry& e : mine) {
        NvAPI_UnicodeString pname{};
        ToNvStr(pname, e.profile.c_str());
        NvDRSProfileHandle prof = nullptr;
        if (NvAPI_DRS_FindProfileByName(sess.get(), pname, &prof) != NVAPI_OK || !prof) {
            Log(NV3D::LogLevel::Warning,
                L"NvProfile: revert - profile '%s' no longer exists, skipping setting 0x%08X",
                e.profile.c_str(), e.setting_id);
            continue;
        }
        if (e.had_value) {
            if (WriteSetting(sess.get(), prof, e.setting_id, e.value) == NVAPI_OK) {
                ++restored;
            }
        } else {
            // There was no user value here before us: drop ours so the
            // driver default applies again. DeleteProfileSetting is the
            // precise inverse of SetSetting; RestoreProfileDefaultSetting is
            // the fallback for drivers that refuse the delete.
            NvAPI_Status st = NvAPI_DRS_DeleteProfileSetting(sess.get(), prof, e.setting_id);
            if (st != NVAPI_OK) {
                st = NvAPI_DRS_RestoreProfileDefaultSetting(sess.get(), prof, e.setting_id);
            }
            if (st == NVAPI_OK) ++restored;
        }
    }

    // Remove what we created, innermost first.
    for (const CreatedRec& c : created) {
        if (!c.profile.empty()) {
            NvAPI_UnicodeString pname{};
            ToNvStr(pname, c.profile.c_str());
            NvDRSProfileHandle prof = nullptr;
            if (NvAPI_DRS_FindProfileByName(sess.get(), pname, &prof) == NVAPI_OK && prof) {
                if (!c.app.empty()) {
                    NvAPI_UnicodeString aname{};
                    ToNvStr(aname, c.app.c_str());
                    NvAPI_DRS_DeleteApplication(sess.get(), prof, aname);
                }
                NvAPI_DRS_DeleteProfile(sess.get(), prof);
            }
        } else if (!c.app.empty()) {
            // We added the app to a pre-existing profile; take just the app
            // back out and leave the profile alone.
            NvAPI_UnicodeString aname{};
            ToNvStr(aname, c.app.c_str());
            NvDRSProfileHandle prof = nullptr;
            NVDRS_APPLICATION app{};
            app.version = NVDRS_APPLICATION_VER;
            if (NvAPI_DRS_FindApplicationByName(sess.get(), aname, &prof, &app) == NVAPI_OK
                && prof) {
                NvAPI_DRS_DeleteApplication(sess.get(), prof, aname);
            }
        }
    }

    if (const NvAPI_Status st = sess.Save(); st != NVAPI_OK) {
        return MapSaveFailure(st, message);
    }
    // Rewrite the record with only the scopes we did NOT revert. ClearBackup
    // deletes the whole section - which is what we want for the stale b<i>
    // rows, but it also takes the surviving scope's created-object markers
    // with it, so snapshot those first and stamp them back afterwards.
    struct SurvivingCreated { std::wstring tag, profile, app; };
    std::vector<SurvivingCreated> survivors;
    for (const wchar_t* tag : { L"self", L"game" }) {
        if (ScopeMatches(scope, tag)) continue;   // this scope is being reverted
        SurvivingCreated sc;
        sc.tag     = tag;
        sc.profile = IniGet(CreatedProfileKey(tag).c_str(), L"", ini_path);
        sc.app     = IniGet(CreatedAppKey(tag).c_str(),     L"", ini_path);
        if (!sc.profile.empty() || !sc.app.empty()) survivors.push_back(std::move(sc));
    }

    ClearBackup(ini_path);

    if (!keep.empty()) {
        // Empty tag: SaveBackup writes the rows (each already carries its own
        // tag) without stamping any created marker of its own.
        SaveBackup(keep, std::wstring(), std::wstring(), std::wstring(), ini_path);
    }
    for (const SurvivingCreated& sc : survivors) {
        if (!sc.profile.empty()) {
            IniSet(CreatedProfileKey(sc.tag).c_str(), sc.profile.c_str(), ini_path);
        }
        if (!sc.app.empty()) {
            IniSet(CreatedAppKey(sc.tag).c_str(), sc.app.c_str(), ini_path);
        }
    }
    IniFlush(ini_path);

    Log(NV3D::LogLevel::Info, L"NvProfile: reverted %d of %zu recorded settings (scope=%s)",
        restored, mine.size(), ScopeName(scope));
    if (message) {
        wchar_t buf[160]{};
        _snwprintf_s(buf, _TRUNCATE, L"Reverted %d driver setting(s).", restored);
        *message = buf;
    }
    return Result::Ok;
}

bool NvProfile::HasBackup(Scope scope, const wchar_t* ini_path, const wchar_t* exe) {
    if (!ini_path || !*ini_path) return false;
    for (const BackupEntry& e : LoadBackup(ini_path)) {
        if (EntryMatches(scope, exe, e)) return true;
    }
    if (exe && *exe) return false;   // created markers are not per executable
    for (const wchar_t* tag : { L"self", L"game" }) {
        if (!ScopeMatches(scope, tag)) continue;
        if (!IniGet(CreatedProfileKey(tag).c_str(), L"", ini_path).empty()) return true;
        if (!IniGet(CreatedAppKey(tag).c_str(),     L"", ini_path).empty()) return true;
    }
    return false;
}

std::wstring NvProfile::DescribeBackup(Scope scope, const wchar_t* ini_path,
                                       const wchar_t* exe) {
    std::wstring profiles;
    size_t n = 0;
    for (const BackupEntry& e : LoadBackup(ini_path)) {
        if (!EntryMatches(scope, exe, e)) continue;
        ++n;
        if (profiles.find(e.profile) == std::wstring::npos) {
            if (!profiles.empty()) profiles += L", ";
            profiles += e.profile;
        }
    }
    if (n == 0) return std::wstring();
    wchar_t buf[512]{};
    _snwprintf_s(buf, _TRUNCATE, L"%zu setting(s) on: %s", n, profiles.c_str());
    return buf;
}

// ---------------------------------------------------------------------------
// Elevation plumbing
// ---------------------------------------------------------------------------

std::wstring NvProfile::SelfArgs(const Options& opt) {
    std::wstring s = std::wstring(kCmdFlag) + L" " + kCmdSelf;
    if (opt.experimental_flip_interval) s += L" flip";
    return s;
}

std::wstring NvProfile::GameArgs(const wchar_t* game_exe_path, int fps_cap) {
    wchar_t buf[1024]{};
    _snwprintf_s(buf, _TRUNCATE, L"%s %s %d \"%s\"",
                 kCmdFlag, kCmdGame, fps_cap, game_exe_path ? game_exe_path : L"");
    return buf;
}

std::wstring NvProfile::RevertArgs(Scope scope, const wchar_t* exe) {
    std::wstring a = std::wstring(kCmdFlag) + L" " + kCmdRevert + L" " + ScopeTag(scope);
    if (exe && *exe) a += std::wstring(L" ") + L'"' + exe + L'"';
    return a;
}

bool NvProfile::RelaunchElevated(const wchar_t* args, std::wstring* message) {
    const std::wstring exe = SelfExePath();
    if (exe.empty()) {
        if (message) *message = L"could not locate our own executable";
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb       = L"runas";          // triggers the UAC consent prompt
    sei.lpFile       = exe.c_str();
    sei.lpParameters = args;
    sei.nShow        = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (message) {
            *message = (err == ERROR_CANCELLED)
                           ? L"administrator approval was declined"
                           : L"could not start the elevated helper";
        }
        Log(NV3D::LogLevel::Warning,
            L"NvProfile: elevated relaunch failed err=%lu", err);
        return false;
    }

    if (!sei.hProcess) {
        if (message) *message = L"elevated helper did not start";
        return false;
    }
    // The child does one DRS transaction and exits. The bound is generous
    // because the UAC prompt itself sits inside this wait.
    const DWORD w = WaitForSingleObject(sei.hProcess, 120000);
    DWORD code = (DWORD)-1;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);

    if (w != WAIT_OBJECT_0) {
        if (message) *message = L"elevated helper timed out";
        return false;
    }
    if (code != 0) {
        if (message) *message = L"elevated helper reported a failure - see the log";
        return false;
    }
    if (message) *message = L"applied (elevated)";
    return true;
}

bool NvProfile::RunCommandLine(const wchar_t* command_line, const wchar_t* ini_path,
                               int* exit_code) {
    if (!command_line || !wcsstr(command_line, kCmdFlag)) return false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(command_line, &argc);
    if (!argv) {
        if (exit_code) *exit_code = 1;
        return true;   // it WAS our flag; we just couldn't parse it
    }

    // Locate the flag, then read its verb and any verb arguments.
    int i = 0;
    for (; i < argc; ++i) {
        if (_wcsicmp(argv[i], kCmdFlag) == 0) break;
    }

    std::wstring   msg;
    Result         r    = Result::Failed;
    const wchar_t* verb = (i + 1 < argc) ? argv[i + 1] : L"";

    if (_wcsicmp(verb, kCmdSelf) == 0) {
        Options opt;
        opt.experimental_flip_interval =
            (i + 2 < argc) && _wcsicmp(argv[i + 2], L"flip") == 0;
        r = ApplySelf(opt, ini_path, &msg);
    } else if (_wcsicmp(verb, kCmdGame) == 0 && i + 3 < argc) {
        const int cap = _wtoi(argv[i + 2]);
        r = ApplyGame(argv[i + 3], cap, ini_path, &msg);
    } else if (_wcsicmp(verb, kCmdRevert) == 0) {
        // "revert <scope>"; a bare "revert" stays all-scopes for convenience.
        Scope scope = Scope::All;
        if (i + 2 < argc) {
            if      (_wcsicmp(argv[i + 2], L"self") == 0) scope = Scope::Self;
            else if (_wcsicmp(argv[i + 2], L"game") == 0) scope = Scope::Game;
        }
        // Optional 4th argument narrows a game revert to one executable.
        const wchar_t* rexe = (i + 3 < argc) ? argv[i + 3] : nullptr;
        r = Revert(scope, ini_path, &msg, rexe);
    } else if (_wcsicmp(verb, L"inspect") == 0) {
        // Read-only: logs the current value, storage location and predefined
        // flag of every setting we might write, on the profile that owns our
        // exe. Writes nothing. Exists so the undo record can be audited
        // without having to trust it first.
        DrsSession sess;
        if (const NvAPI_Status st = sess.Open(); st != NVAPI_OK) {
            r = MapOpenFailure(st, &msg);
        } else {
            // "inspect [exe]" - defaults to our own executable. Naming a
            // game's exe reports what the driver actually has for it, which
            // is the only way to confirm a revert really landed.
            const std::wstring exe = (i + 2 < argc && argv[i + 2][0] != 0)
                                         ? BaseName(argv[i + 2])
                                         : BaseName(SelfExePath().c_str());
            NvAPI_UnicodeString app_name{};
            ToNvStr(app_name, exe.c_str());
            NvDRSProfileHandle prof = nullptr;
            NVDRS_APPLICATION app{};
            app.version = NVDRS_APPLICATION_VER;
            if (NvAPI_DRS_FindApplicationByName(sess.get(), app_name, &prof, &app) == NVAPI_OK
                && prof) {
                InspectSettings(sess.get(), prof, ProfileName(sess.get(), prof));
                r   = Result::Ok;
                msg = L"inspected";
            } else {
                msg = L"that exe belongs to no profile";
                r   = Result::Nothing;
            }
        }
    } else {
        msg = L"unrecognised profile command";
    }

    LocalFree(argv);

    Log(r == Result::Ok || r == Result::Nothing ? NV3D::LogLevel::Info
                                                : NV3D::LogLevel::Error,
        L"NvProfile: elevated command '%s' -> %d (%s)",
        verb, (int)r, msg.c_str());

    if (exit_code) *exit_code = (r == Result::Ok || r == Result::Nothing) ? 0 : 1;
    return true;
}

}  // namespace nv3dg
