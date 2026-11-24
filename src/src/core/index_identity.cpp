// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/index_identity.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "common/logging/log.h"
#include "core/module.h"
#include "core/rhi_command_capture_contract.h"

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
