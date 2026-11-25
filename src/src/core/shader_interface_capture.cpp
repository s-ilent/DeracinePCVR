// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/shader_interface_capture.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "common/hash.h"
#include "common/logging/log.h"

namespace Core::ShaderInterface {

/// The logical stages a graphics pipeline can allocate bindings for. The shader compiler's
/// own enum is not visible here, and this module only needs the bound.
inline constexpr u32 MaxLogicalStages = 8;
namespace {

struct CaptureState {
    std::atomic<bool> enabled{};
    std::atomic<u32> count{};
    std::atomic<u32> duplicate_count{};
    std::atomic<u32> capacity_dropped{};
    std::mutex mutex;
    std::array<Interface, MaxRetainedInterfaces> interfaces{};
};

CaptureState g_state{};

[[nodiscard]] u32 FindSlot(u64 program_hash, u32 permutation_index) noexcept {
    const u32 count = g_state.count.load(std::memory_order_relaxed);
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = g_state.interfaces[i];
        if (entry.program_hash == program_hash &&
            entry.permutation_index == permutation_index) {
            return i;
        }
    }
    return count;
}

} // namespace

bool IsEnabled() noexcept {
    return g_state.enabled.load(std::memory_order_relaxed);
}

void TryInstall() noexcept {
    const char* value = std::getenv(EnableEnvironmentVariable.data());
    if (value == nullptr || std::string_view{value} != "1") {
        return;
    }
    g_state.enabled.store(true, std::memory_order_relaxed);
    LOG_INFO(Core_Linker, "DERACINE_SHADER_INTERFACE_INSTALL limit={} max_descriptors={}",
             MaxRetainedInterfaces, MaxDescriptorsPerStage);
}

void ResetForTesting() noexcept {
    std::scoped_lock lock{g_state.mutex};
    g_state.count.store(0, std::memory_order_relaxed);
    g_state.duplicate_count.store(0, std::memory_order_relaxed);
    g_state.capacity_dropped.store(0, std::memory_order_relaxed);
    g_state.enabled.store(true, std::memory_order_relaxed);
}

bool ResolveGlobalBindings(std::span<const u32> stage_order,
                           std::span<const u32> stage_counts) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        if (stage_order.size() != stage_counts.size()) {
            return false;
        }

        // The base and count each stage is allocated at, keyed by logical stage. The layout's
        // arrays are positional, so they are indexed by position here and by stage afterwards.
        std::array<u32, MaxLogicalStages> base{};
        std::array<u32, MaxLogicalStages> count_by_stage{};
        std::array<bool, MaxLogicalStages> known{};
        u32 running = 0;
        for (size_t i = 0; i < stage_order.size(); ++i) {
            if (stage_order[i] >= MaxLogicalStages) {
                return false;
            }
            base[stage_order[i]] = running;
            count_by_stage[stage_order[i]] = stage_counts[i];
            known[stage_order[i]] = true;
            running += stage_counts[i];
        }

        std::scoped_lock lock{g_state.mutex};
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < count; ++i) {
            auto& interface = g_state.interfaces[i];
            if (interface.logical_stage >= MaxLogicalStages ||
                !known[interface.logical_stage]) {
                // A stage the layout did not allocate has no binding number. Leaving it unresolved
                // is correct: a guessed number would be worse than an absent one.
                continue;
            }
            // The capture's own count must agree with the layout's for this stage, or the two
            // describe different interfaces and no binding number can be trusted.
            if (interface.unified_binding_count != count_by_stage[interface.logical_stage]) {
                return false;
            }
            interface.stage_binding_base = base[interface.logical_stage];
            interface.stage_binding_base_known = true;
            for (u32 d = 0; d < interface.descriptor_count; ++d) {
                auto& descriptor = interface.descriptors[d];
                descriptor.binding = interface.stage_binding_base + descriptor.local_binding;
                descriptor.binding_resolved = true;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

void NoteInterface(const Interface& entry) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        std::scoped_lock lock{g_state.mutex};

        // A permutation is compiled once and reused. A later draw referring to the same
        // (program hash, permutation index) refers to the same module and therefore the same
        // interface, so it is not stored again.
        const u32 existing = FindSlot(entry.program_hash, entry.permutation_index);
        if (existing < g_state.count.load(std::memory_order_relaxed)) {
            g_state.duplicate_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const u32 slot = g_state.count.load(std::memory_order_relaxed);
        if (slot >= MaxRetainedInterfaces) {
            g_state.capacity_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Copied by value. The caller's Interface is a snapshot of the compiler's state at the
        // point it was taken, so nothing here borrows a pointer into mutable Shader::Info or a
        // temporary binding allocation.
        g_state.interfaces[slot] = entry;
        g_state.count.store(slot + 1, std::memory_order_relaxed);
    } catch (...) {
        // Capture must never affect compilation.
    }
}

bool FindByPermutationHash(u64 permutation_hash, Interface& out) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lock{g_state.mutex};
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < count; ++i) {
            if (g_state.interfaces[i].permutation_hash == permutation_hash) {
                out = g_state.interfaces[i];
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

bool GetRetained(u32 index, Interface& out) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lock{g_state.mutex};
        if (index >= g_state.count.load(std::memory_order_relaxed)) {
            return false;
        }
        out = g_state.interfaces[index];
        return true;
    } catch (...) {
        return false;
    }
}

void ExportInterfaces() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        LOG_INFO(Core_Linker,
                 "DERACINE_SHADER_INTERFACE_SUMMARY retained={} duplicates={} capacity_dropped={} "
                 "limit={}",
                 count, g_state.duplicate_count.load(std::memory_order_relaxed),
                 g_state.capacity_dropped.load(std::memory_order_relaxed), MaxRetainedInterfaces);

        std::scoped_lock lock{g_state.mutex};
        for (u32 i = 0; i < count; ++i) {
            const auto& entry = g_state.interfaces[i];
            LOG_INFO(Core_Linker,
                     "DERACINE_SHADER_INTERFACE index={} program_hash={:#x} perm_idx={} "
                     "perm_hash={:#x} hw_stage={} logical_stage={} patched={} descriptors={} "
                     "push_offset={} push_size={} unified={}",
                     i, entry.program_hash, entry.permutation_index, entry.permutation_hash,
                     entry.hardware_stage, entry.logical_stage, entry.patched ? 1 : 0,
                     entry.descriptor_count, entry.push_constant.offset,
                     entry.push_constant.size, entry.unified_binding_count);
            for (u32 d = 0; d < entry.descriptor_count; ++d) {
                const auto& descriptor = entry.descriptors[d];
                LOG_INFO(Core_Linker,
                         "DERACINE_SHADER_INTERFACE_DESC perm_hash={:#x} set={} binding={} "
                         "class={} count={} stage_flags={:#x} guest_index={} written={}",
                         entry.permutation_hash, descriptor.set, descriptor.binding,
                         DescriptorClassName(descriptor.cls), descriptor.count,
                         descriptor.stage_flags, descriptor.guest_resource_index,
                         descriptor.written ? 1 : 0);
            }
        }
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

} // namespace Core::ShaderInterface
