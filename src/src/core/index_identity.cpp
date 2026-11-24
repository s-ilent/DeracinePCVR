// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/index_identity.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "common/logging/log.h"
#include "core/module.h"
#include "core/rhi_command_capture_contract.h"
#include "core/rhi_entry_probe.h"

namespace Core::IndexIdentity {
namespace {

struct ObserverState {
    std::atomic<bool> enabled{};
    std::atomic<u64> execution_id{};
    std::atomic<u32> count{};
    std::atomic<u32> duplicate_count{};
    std::atomic<u32> capacity_dropped{};
    std::mutex mutex;
    std::array<OriginalIndexResourceIdentity, MaxIdentityRecords> records{};
};

ObserverState g_state{};

} // namespace

bool IsEnabled() noexcept {
    return g_state.enabled.load(std::memory_order_relaxed);
}

u64 ExecutionId() noexcept {
    return g_state.execution_id.load(std::memory_order_relaxed);
}

void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version) {
    const char* value = std::getenv(EnableEnvironmentVariable.data());
    if (value == nullptr || std::string_view{value} != "1") {
        return;
    }

    // The same identity gate the other observers use: the exact approved executable.
    if (module.memory == nullptr || module.name != RhiCommandCapture::ModuleName ||
        module.file.filename().string() != RhiCommandCapture::ModuleName ||
        title_id != RhiCommandCapture::TitleId) {
        LOG_WARNING(Core_Linker,
                    "Index identity refused: expected native CUSA11919 eboot.bin, got "
                    "title={} module={}",
                    title_id, module.name);
        return;
    }

    // The module and title checks above are necessary but not sufficient: they do not prove the
    // executable is one of the approved images. The RHI entry probe performs the hash check, and
    // this observer depends on the exact layout recovered from that image, so it must not become
    // active unless the probe is active. Requiring the probe's own gate means the index observer
    // cannot be enabled on an unapproved build by setting its flag alone.
    if (!RhiEntryProbe::IsInstalled()) {
        LOG_WARNING(Core_Linker,
                    "Index identity refused: the RHI entry probe is not installed, so the approved "
                    "executable hash was not verified");
        return;
    }

    // The execution identity is derived from the wall clock at install time. It is not a security
    // token: its only job is to make two records from different processes distinguishable, so a
    // reused provenance id across runs cannot be mistaken for the same draw.
    const u64 now = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    g_state.execution_id.store(now, std::memory_order_relaxed);
    g_state.enabled.store(true, std::memory_order_relaxed);

    LOG_INFO(Core_Linker,
             "DERACINE_INDEX_IDENTITY_INSTALL title_id={} app_version={} execution_id={} "
             "limit={}",
             title_id, app_version, now, MaxIdentityRecords);
}

void NoteIdentity(const OriginalIndexResourceIdentity& identity) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        if (identity.provenance_id == 0) {
            return;
        }

        std::scoped_lock lock{g_state.mutex};

        // One record per draw. A repeated observation of the same provenance id is the same draw
        // seen twice, not a second resource.
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < count; ++i) {
            if (g_state.records[i].provenance_id == identity.provenance_id) {
                g_state.duplicate_count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if (count >= MaxIdentityRecords) {
            g_state.capacity_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Copied by value. Nothing here retains a guest pointer.
        g_state.records[count] = identity;
        g_state.count.store(count + 1, std::memory_order_relaxed);
    } catch (...) {
        // Observation must never affect rendering.
    }
}

bool FindByProvenance(u64 provenance_id, OriginalIndexResourceIdentity& out) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed) || provenance_id == 0) {
            return false;
        }
        std::scoped_lock lock{g_state.mutex};
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < count; ++i) {
            if (g_state.records[i].provenance_id == provenance_id) {
                out = g_state.records[i];
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

u32 RetainedCount() noexcept {
    if (!g_state.enabled.load(std::memory_order_relaxed)) {
        return 0;
    }
    return g_state.count.load(std::memory_order_relaxed);
}

bool GetRetained(u32 index, OriginalIndexResourceIdentity& out) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lock{g_state.mutex};
        if (index >= g_state.count.load(std::memory_order_relaxed)) {
            return false;
        }
        out = g_state.records[index];
        return true;
    } catch (...) {
        return false;
    }
}

/// Writes the machine-readable sidecar the replay consumes.
///
/// The replay cannot reconstruct the bridge from the package: the package carries a guest address,
/// and the whole point of the bridge is that it was independently observed from the original UE4
/// object. A replay that derived the bridge from the package would be comparing the package with
/// itself.
void ExportSidecar(const std::string& directory) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const auto dir = std::filesystem::path{directory};
        if (!std::filesystem::exists(dir)) {
            LOG_WARNING(Core_Linker, "Index bridge sidecar refused: directory missing {}",
                        dir.string());
            return;
        }
        const auto path = dir / "index_bridge.json";
        std::ofstream out{path, std::ios::trunc};
        if (!out) {
            LOG_WARNING(Core_Linker, "Index bridge sidecar refused: cannot open {}",
                        path.string());
            return;
        }

        std::scoped_lock lock{g_state.mutex};
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        out << "{\n";
        out << "  \"schema_version\": 1,\n";
        out << "  \"records\": [\n";
        for (u32 i = 0; i < count; ++i) {
            const auto& record = g_state.records[i];
            out << "    {\n";
            out << "      \"provenance_id\": " << record.provenance_id << ",\n";
            out << "      \"session_id\": " << record.session_id << ",\n";
            out << "      \"has_session_id\": " << (record.has_session_id ? "true" : "false")
                << ",\n";
            out << "      \"execution_id\": " << record.execution_id << ",\n";
            out << "      \"has_execution_id\": "
                << (record.execution_id != 0 ? "true" : "false") << ",\n";
            out << "      \"original_rhi_object\": " << record.original_rhi_object << ",\n";
            out << "      \"has_original_rhi_object\": "
                << (record.has_original_rhi_object ? "true" : "false") << ",\n";
            out << "      \"resource_base\": " << record.resource_base << ",\n";
            out << "      \"has_resource_base\": "
                << (record.has_resource_base ? "true" : "false") << ",\n";
            out << "      \"address_candidate_a\": " << record.address_candidate_a << ",\n";
            out << "      \"has_address_candidate_a\": "
                << (record.has_address_candidate_a ? "true" : "false") << ",\n";
            out << "      \"address_candidate_b\": " << record.address_candidate_b << ",\n";
            out << "      \"has_address_candidate_b\": "
                << (record.has_address_candidate_b ? "true" : "false") << ",\n";
            out << "      \"selected_slot\": \"" << AddressSlotName(record.address_slot)
                << "\",\n";
            out << "      \"selected_guest_address\": " << record.guest_allocation_base
                << ",\n";
            out << "      \"has_selected_guest_address\": "
                << (record.has_guest_allocation_base ? "true" : "false") << ",\n";
            out << "      \"index_element_width\": " << record.index_element_width << ",\n";
            out << "      \"has_index_element_width\": "
                << (record.has_index_element_width ? "true" : "false") << ",\n";
            out << "      \"allocation_size\": " << record.guest_allocation_size << ",\n";
            out << "      \"has_allocation_size\": "
                << (record.has_guest_allocation_size ? "true" : "false") << ",\n";
            out << "      \"allocation_offset\": " << record.allocation_offset << ",\n";
            out << "      \"has_allocation_offset\": "
                << (record.has_allocation_offset ? "true" : "false") << ",\n";
            out << "      \"resource_generation\": " << record.resource_generation << ",\n";
            out << "      \"has_resource_generation\": "
                << (record.has_resource_generation ? "true" : "false") << ",\n";
            out << "      \"predicate_determinable\": "
                << (record.predicate.determinable ? "true" : "false") << ",\n";
            out << "      \"address_evidence\": \""
                << EvidenceClassName(record.address_evidence) << "\"\n";
            out << "    }" << (i + 1 < count ? "," : "") << "\n";
        }
        out << "  ]\n";
        out << "}\n";
        out.close();

        LOG_INFO(Core_Linker, "DERACINE_INDEX_IDENTITY_SIDECAR records={} path={}", count,
                 path.string());
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

void ExportIdentities() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        LOG_INFO(Core_Linker,
                 "DERACINE_INDEX_IDENTITY_SUMMARY execution_id={} records={} duplicates={} "
                 "capacity_dropped={} limit={}",
                 ExecutionId(), count,
                 g_state.duplicate_count.load(std::memory_order_relaxed),
                 g_state.capacity_dropped.load(std::memory_order_relaxed), MaxIdentityRecords);

        std::scoped_lock lock{g_state.mutex};
        for (u32 i = 0; i < count; ++i) {
            const auto& record = g_state.records[i];
            LOG_INFO(Core_Linker,
                     "DERACINE_INDEX_IDENTITY execution_id={} provenance_id={} "
                     "original_object={:#x} has_object={} resource_base={:#x} has_base={} "
                     "guest_base={:#x} has_guest_base={} slot={} size={} has_size={} "
                     "offset={} has_offset={} width={} has_width={} generation={} "
                     "has_generation={} address_evidence={} size_evidence={}",
                     record.execution_id, record.provenance_id, record.original_rhi_object,
                     record.has_original_rhi_object ? 1 : 0, record.resource_base,
                     record.has_resource_base ? 1 : 0, record.guest_allocation_base,
                     record.has_guest_allocation_base ? 1 : 0,
                     AddressSlotName(record.address_slot), record.guest_allocation_size,
                     record.has_guest_allocation_size ? 1 : 0, record.allocation_offset,
                     record.has_allocation_offset ? 1 : 0, record.index_element_width,
                     record.has_index_element_width ? 1 : 0, record.resource_generation,
                     record.has_resource_generation ? 1 : 0,
                     EvidenceClassName(record.address_evidence),
                     EvidenceClassName(record.size_evidence));
        }
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

} // namespace Core::IndexIdentity