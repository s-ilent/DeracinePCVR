// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "core/unreal/guest_memory.h"
#include "core/unreal/inspector.h"

namespace Core::Devtools::Widget {

// Read-only view of the Unreal objects of the running game, plus the escape hatch that writes a
// ps4-unreal-dumper memory snapshot for offline analysis. Discovery only runs when asked: nothing
// scans guest memory during ordinary gameplay.
class UnrealObjectsViewer {
public:
    bool open = false;

    void Draw();

private:
    void Discover();
    void Capture();
    void DrawDiscovery();
    void DrawObjectList();

    Unreal::LiveGuestMemory live_memory;
    std::unique_ptr<Unreal::Inspector> inspector;
    Unreal::ObjectInfo selected;
    Unreal::DiscoveryResult discovery;
    std::string last_error;
    std::string snapshot_path;
    std::string snapshot_status;

    char search[128]{};
    std::vector<Unreal::ObjectInfo> results;
    u32 limit = 200;
    bool discovered = false;
};

} // namespace Core::Devtools::Widget
