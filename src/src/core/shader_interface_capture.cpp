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
