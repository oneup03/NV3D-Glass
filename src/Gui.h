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
