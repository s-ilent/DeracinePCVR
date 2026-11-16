// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/devtools/widget/unreal_objects.h"

#include <imgui.h>

#include <array>
#include <cstdio>
#include <ctime>

#include "common/elf_info.h"
#include "common/scm_rev.h"
#include "common/singleton.h"
#include "core/memory.h"
#include "core/unreal/guest_memory.h"
#include "core/unreal/report.h"
#include "core/unreal/snapshot.h"

using namespace ImGui;

namespace Core::Devtools::Widget {
namespace {

// The main module is mapped under its file name; its data segments are where GObjects lives.
constexpr const char* ExecutableVmaName = "eboot.bin";
constexpr const char* ExecutableRegionName = "executable";

bool HasProt(MemoryProt prot, MemoryProt flag) {
    return (static_cast<u32>(prot) & static_cast<u32>(flag)) != 0;
}

std::string Now() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::array<char, 32> text{};
    std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S", &local);
    return text.data();
}

Unreal::ScanRanges LiveRanges(MemoryManager& memory) {
    Unreal::ScanRanges ranges;
    for (const auto& region : memory.EnumerateMappedRegions()) {
        if (!HasProt(region.prot, MemoryProt::CpuRead) || region.size == 0) {
            continue;
        }
        const Unreal::AddressRange range{region.address, region.address + region.size};
        ranges.all.push_back(range);
        if (region.name == ExecutableVmaName && HasProt(region.prot, MemoryProt::CpuWrite)) {
            ranges.writable.push_back(range);
        }
    }
    return ranges;
}

} // Anonymous namespace

void UnrealObjectsViewer::Discover() {
    discovered = false;
    inspector.reset();
    last_error.clear();
    results.clear();

    auto* memory = Core::Memory::Instance();
    if (memory == nullptr) {
        last_error = "guest memory is not initialized";
        return;
    }
    inspector = std::make_unique<Unreal::Inspector>(live_memory, LiveRanges(*memory));
    discovery = inspector->Discover();
    discovered = true;
    if (!discovery.ok()) {
        last_error = discovery.error;
        inspector.reset();
    }
}

void UnrealObjectsViewer::Capture() {
    auto* memory = Core::Memory::Instance();
    if (memory == nullptr) {
        snapshot_status = "guest memory is not initialized";
        return;
    }
    if (snapshot_path.empty()) {
        snapshot_status = "set an output path first";
        return;
    }

    Unreal::SnapshotMetadata metadata;
    metadata.time = Now();
    metadata.emulator_commit = std::string{Common::g_scm_rev};
    if (const auto* elf = Common::Singleton<Common::ElfInfo>::Instance();
        elf != nullptr && elf->Initialized()) {
        metadata.title_id = std::string{elf->GameSerial()};
        metadata.title_name = std::string{elf->Title()};
        metadata.app_version = std::string{elf->AppVer()};
    }

    const auto regions = Unreal::EnumerateLiveRegions(ExecutableVmaName, ExecutableRegionName);
    const auto result = Unreal::WriteSnapshot(snapshot_path, regions, &Unreal::ReadLiveRegion,
                                             metadata);
    if (result.ok) {
        char text[256];
        std::snprintf(text, sizeof(text),
                      "wrote %llu regions, %.1f MiB, %llu holes",
                      static_cast<unsigned long long>(result.regions),
                      static_cast<double>(result.bytes_written) / (1024.0 * 1024.0),
                      static_cast<unsigned long long>(result.holes));
        snapshot_status = text;
    } else {
        snapshot_status = "failed: " + result.error;
    }
}

void UnrealObjectsViewer::ExportReport() {
    if (!inspector || !inspector->Result().ok()) {
        report_status = "discovery has not found a name table and object array yet";
        return;
    }
    std::string path = report_path;
    if (path.empty()) {
        if (snapshot_path.empty()) {
            report_status = "set a report path (or a snapshot path to derive it from)";
            return;
        }
        path = snapshot_path + "_report.json";
    }

    Unreal::SnapshotMetadata metadata;
    metadata.time = Now();
    metadata.emulator_commit = std::string{Common::g_scm_rev};
    if (const auto* elf = Common::Singleton<Common::ElfInfo>::Instance();
        elf != nullptr && elf->Initialized()) {
        metadata.title_id = std::string{elf->GameSerial()};
        metadata.title_name = std::string{elf->Title()};
        metadata.app_version = std::string{elf->AppVer()};
    }

    std::string error;
    if (Unreal::WriteDiagnosticReport(path, *inspector, metadata, {}, &error)) {
        report_status = "wrote " + path;
    } else {
        report_status = "failed: " + error;
    }
}

void UnrealObjectsViewer::DrawDiscovery() {
    if (!discovered) {
        TextUnformatted("Discovery does not run during gameplay.");
        if (Button("Discover Unreal objects")) {
            Discover();
        }
        return;
    }
    if (!discovery.ok()) {
        TextColored({1.0f, 0.6f, 0.4f, 1.0f}, "Not found: %s", last_error.c_str());
        SameLine();
        if (Button("Retry")) {
            Discover();
        }
        return;
    }
    Text("Name table: %s at %016llX", discovery.name_table_kind.c_str(),
         static_cast<unsigned long long>(discovery.names_address));
    Text("Object array: %s at %016llX", discovery.object_array_kind.c_str(),
         static_cast<unsigned long long>(discovery.objects_address));
    Text("Objects: %u live of %u capacity, %u chunks", discovery.object_count,
         discovery.object_capacity, discovery.object_chunks);
    SameLine();
    if (Button("Refresh discovery")) {
        Discover();
    }
}

void UnrealObjectsViewer::DrawObjectList() {
    if (!inspector) {
        return;
    }
    SetNextItemWidth(240.0f);
    InputText("##search", search, sizeof(search));
    SameLine();
    if (Button("By name")) {
        results = inspector->SearchByName(search, limit);
    }
    SameLine();
    if (Button("By class")) {
        results = inspector->SearchByClass(search, limit);
    }
    SameLine();
    if (Button("All")) {
        results.clear();
        for (u32 index = 0; index < inspector->ObjectCount() && results.size() < limit; ++index) {
            if (auto info = inspector->ObjectAt(index)) {
                results.push_back(std::move(*info));
            }
        }
    }

    Text("Showing %zu of %u objects", results.size(), inspector->ObjectCount());
    BeginChild("##objects", {0.0f, 260.0f}, ImGuiChildFlags_Borders);
    for (const auto& object : results) {
        char label[256];
        std::snprintf(label, sizeof(label), "%s : %s##%016llX", object.name.c_str(),
                      object.class_name.c_str(),
                      static_cast<unsigned long long>(object.address));
        if (Selectable(label)) {
            selected = object;
        }
        if (IsItemHovered()) {
            SetTooltip("index %u, address %016llX, flags %08X", object.index,
                       static_cast<unsigned long long>(object.address), object.flags);
        }
    }
    EndChild();

    if (!selected.name.empty()) {
        Separator();
        Text("Name: %s", selected.name.c_str());
        Text("Class: %s at %016llX", selected.class_name.c_str(),
             static_cast<unsigned long long>(selected.class_address));
        Text("Address: %016llX   index: %u   flags: %08X",
             static_cast<unsigned long long>(selected.address), selected.index, selected.flags);
        Text("Outer: %016llX", static_cast<unsigned long long>(selected.outer_address));
        SameLine();
        if (Button("Follow outer")) {
            Unreal::ObjectInfo outer;
            if (inspector->FollowReference(selected, Unreal::ObjectOuterOffset, outer)) {
                selected = outer;
            }
        }
        Text("Live: %s", inspector->IsLive(selected.index, selected) ? "yes" : "no");
    }
}

void UnrealObjectsViewer::Draw() {
    SetNextWindowSize({620.0f, 520.0f}, ImGuiCond_FirstUseEver);
    if (!Begin("Unreal objects", &open)) {
        End();
        return;
    }

    DrawDiscovery();
    Separator();
    DrawObjectList();

    Separator();
    TextUnformatted("Memory snapshot (ps4-unreal-dumper format)");
    SetNextItemWidth(360.0f);
    std::array<char, 512> path{};
    std::snprintf(path.data(), path.size(), "%s", snapshot_path.c_str());
    if (InputText("##snapshot_path", path.data(), path.size())) {
        snapshot_path = path.data();
    }
    SameLine();
    if (Button("Export snapshot")) {
        Capture();
    }

    Separator();
    TextUnformatted("Diagnostic report (discovery, layout, class histogram, hand/anim candidates)");
    SetNextItemWidth(360.0f);
    std::array<char, 512> report{};
    std::snprintf(report.data(), report.size(), "%s", report_path.c_str());
    if (InputText("##report_path", report.data(), report.size())) {
        report_path = report.data();
    }
    SameLine();
    const bool can_report = inspector != nullptr && inspector->Result().ok();
    BeginDisabled(!can_report);
    if (Button("Export report")) {
        ExportReport();
    }
    EndDisabled();
    if (!can_report) {
        SameLine();
        TextDisabled("needs a successful discovery");
    }
    if (!report_status.empty()) {
        TextWrapped("%s", report_status.c_str());
    }
    if (!snapshot_status.empty()) {
        TextWrapped("%s", snapshot_status.c_str());
    }

    End();
}

} // namespace Core::Devtools::Widget
