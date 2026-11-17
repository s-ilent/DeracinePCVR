// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/unreal/inspector.h"
#include "core/unreal/snapshot.h"

namespace Core::Unreal {

struct DiagnosticReportOptions {
    // Bound the single pass over the object array so an export cannot stall a frame for long.
    u32 max_scanned_objects = 200'000;
    u32 top_classes = 20;
    u32 max_candidates = 200;
    // Case-insensitive substrings matched against object and class names, for the sections a hand /
    // animation investigation needs first.
    std::vector<std::string> candidate_patterns = {"hand",  "skeletal", "anim",
                                                  "pawn",  "world",    "player",
                                                  "move",  "controller", "skeleton"};
};

// Compact JSON summary of a successful discovery: where the tables were found, how the object array
// is laid out, a class histogram, and the objects whose name or class matches the candidate patterns.
// Cheap enough to attach to a snapshot capture; never writes to guest memory.
nlohmann::json BuildDiagnosticReport(const Inspector& inspector, const SnapshotMetadata& metadata,
                                     const DiagnosticReportOptions& options = {});

// Writes the report as indented JSON. Returns false and sets `error` when the file cannot be written.
bool WriteDiagnosticReport(const std::filesystem::path& path, const Inspector& inspector,
                           const SnapshotMetadata& metadata,
                           const DiagnosticReportOptions& options = {}, std::string* error = nullptr);

} // namespace Core::Unreal
