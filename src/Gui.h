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
    void DrawHotkeySection(App& app);
    void DrawStatusFooter(App& app);

    std::vector<WindowEntry>  windows_;
    std::vector<MonitorEntry> monitors_;
    bool                      capturing_chord_for_ = false;
    int                       chord_id_ = -1;
};

}  // namespace nv3dg
