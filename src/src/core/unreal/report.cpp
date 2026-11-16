// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/unreal/report.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace Core::Unreal {
namespace {

std::string Hex(VAddr address) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(address));
    return text;
}

std::string ToLower(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool MatchesAny(std::string_view text, const std::vector<std::string>& patterns) {
    const auto lower = ToLower(text);
    return std::ranges::any_of(
        patterns, [&](const std::string& pattern) { return lower.find(pattern) != std::string::npos; });
}

nlohmann::json EmptyReport(const SnapshotMetadata& metadata, std::string_view error) {
    return nlohmann::json{{"report", "shadPS4-unreal-inspector/1"},
                          {"generated", metadata.time},
                          {"emulator_commit", metadata.emulator_commit},
                          {"discovery", {{"ok", false}, {"error", error}}},
                          {"objects", {{"live", 0}, {"stale_slots", 0}, {"scanned", 0}}},
                          {"classes", nlohmann::json::array()},
                          {"candidates", nlohmann::json::array()}};
}

} // Anonymous namespace

nlohmann::json BuildDiagnosticReport(const Inspector& inspector, const SnapshotMetadata& metadata,
                                     const DiagnosticReportOptions& options) {
    const auto& discovery = inspector.Result();
    if (!discovery.ok()) {
        return EmptyReport(metadata, discovery.error);
    }

    std::vector<std::string> patterns;
    patterns.reserve(options.candidate_patterns.size());
    for (const auto& pattern : options.candidate_patterns) {
        patterns.push_back(ToLower(pattern));
    }

    std::unordered_map<std::string, u32> class_counts;
    std::vector<nlohmann::json> candidates;
    u32 scanned = 0;
    u32 live = 0;
    u32 stale = 0;

    const u32 count = std::min(inspector.ObjectCount(), options.max_scanned_objects);
    for (u32 index = 0; index < count; ++index) {
        ++scanned;
        const auto info = inspector.ObjectAt(index);
        if (!info) {
            ++stale;
            continue;
        }
        ++live;
        ++class_counts[info->class_name];
        if (candidates.size() < options.max_candidates &&
            (MatchesAny(info->name, patterns) || MatchesAny(info->class_name, patterns))) {
            candidates.push_back({{"name", info->name},
                                  {"class", info->class_name},
                                  {"address", Hex(info->address)},
                                  {"class_address", Hex(info->class_address)},
                                  {"outer", Hex(info->outer_address)},
                                  {"index", info->index},
                                  {"flags", info->flags}});
        }
    }

    std::vector<std::pair<std::string, u32>> sorted_classes{class_counts.begin(),
                                                           class_counts.end()};
    std::ranges::sort(sorted_classes, [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first < b.first;
    });
    nlohmann::json classes = nlohmann::json::array();
    for (u32 i = 0; i < sorted_classes.size() && i < options.top_classes; ++i) {
        classes.push_back({{"class", sorted_classes[i].first}, {"count", sorted_classes[i].second}});
    }

    std::vector<std::string> notes;
    notes.push_back("capture is non-atomic: objects may be freed or recycled while scanning");
    if (count < inspector.ObjectCount()) {
        notes.push_back("scan stopped at max_scanned_objects; counts below are partial");
    }
    std::string joined_patterns;
    for (const auto& pattern : options.candidate_patterns) {
        if (!joined_patterns.empty()) {
            joined_patterns += ',';
        }
        joined_patterns += pattern;
    }
    notes.push_back("candidate patterns: " + joined_patterns);

    return nlohmann::json{
        {"report", "shadPS4-unreal-inspector/1"},
        {"generated", metadata.time},
        {"emulator_commit", metadata.emulator_commit},
        {"title",
         {{"id", metadata.title_id},
          {"name", metadata.title_name},
          {"version", metadata.app_version},
          {"path", metadata.executable_path}}},
        {"discovery",
         {{"ok", true},
          {"name_table", {{"kind", discovery.name_table_kind}, {"address", Hex(discovery.names_address)}}},
          {"object_array",
           {{"kind", discovery.object_array_kind},
            {"address", Hex(discovery.objects_address)},
            {"layout", discovery.object_layout},
            {"item_size", discovery.object_item_size},
            {"item_offset", discovery.object_item_offset},
            {"items_per_chunk", discovery.object_items_per_chunk},
            {"count", discovery.object_count},
            {"capacity", discovery.object_capacity},
            {"chunks", discovery.object_chunks},
            {"max_chunks", discovery.object_max_chunks}}}}},
        {"objects", {{"live", live}, {"stale_slots", stale}, {"scanned", scanned}, {"array_count", inspector.ObjectCount()}}},
        {"classes", std::move(classes)},
        {"candidates", std::move(candidates)},
        {"notes", std::move(notes)}};
}

bool WriteDiagnosticReport(const std::filesystem::path& path, const Inspector& inspector,
                           const SnapshotMetadata& metadata,
                           const DiagnosticReportOptions& options, std::string* error) {
    const auto report = BuildDiagnosticReport(inspector, metadata, options);
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream file{path, std::ios::trunc};
    if (!file) {
        if (error != nullptr) {
            *error = "cannot open " + path.string();
        }
        return false;
    }
    file << report.dump(1) << '\n';
    if (!file) {
        if (error != nullptr) {
            *error = "failed to write " + path.string();
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

} // namespace Core::Unreal
