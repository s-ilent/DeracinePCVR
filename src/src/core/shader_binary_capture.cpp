// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/shader_binary_capture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common/hash.h"
#include "common/logging/log.h"
#include "common/sha256.h"

namespace Core::ShaderBinaryCapture {
namespace {

struct CaptureState {
    std::atomic<bool> enabled{};
    std::atomic<u32> retained_count{};
    std::atomic<u32> duplicate_count{};
    std::atomic<u32> capacity_dropped{};
    std::atomic<u32> oversize_dropped{};
    std::mutex mutex;
    std::array<RetainedBinary, MaxRetainedBinaries> binaries{};
};

CaptureState g_state{};

[[nodiscard]] std::array<u8, 32> Sha256OfWords(std::span<const u32> words) noexcept {
    std::array<u8, 32> digest{};
    if (words.empty()) {
        return digest;
    }
    sha256::SHA256 hasher;
    hasher.update(words.data(), words.size() * sizeof(u32));
    const auto result = hasher.final();
    std::memcpy(digest.data(), result.data(), digest.size());
    return digest;
}

/// Finds an existing entry for this identity, or the end of the table.
[[nodiscard]] u32 FindSlot(const ShaderIdentity& identity) noexcept {
    const u32 count = g_state.retained_count.load(std::memory_order_relaxed);
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = g_state.binaries[i];
        if (entry.identity.program_hash == identity.program_hash &&
            entry.identity.permutation_index == identity.permutation_index) {
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
    LOG_INFO(Core_Linker, "DERACINE_SHADER_BINARY_INSTALL limit={} max_bytes={}",
             MaxRetainedBinaries, MaxBinaryBytes);
}

void NoteCompiledShader(const ShaderIdentity& identity, std::span<const u32> gcn_code,
                        std::span<const u32> spirv, std::span<const u32> patch_spirv,
                        bool patched) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        if (spirv.empty()) {
            return;
        }
        if (spirv.size() * sizeof(u32) > MaxBinaryBytes ||
            gcn_code.size() * sizeof(u32) > MaxBinaryBytes) {
            g_state.oversize_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::scoped_lock lock{g_state.mutex};

        // A permutation is compiled once and reused. A later draw referring to the same
        // (program hash, permutation index) refers to the same module, so it is not stored again.
        const u32 existing = FindSlot(identity);
        if (existing < g_state.retained_count.load(std::memory_order_relaxed)) {
            g_state.duplicate_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const u32 slot = g_state.retained_count.load(std::memory_order_relaxed);
        if (slot >= MaxRetainedBinaries) {
            g_state.capacity_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        RetainedBinary entry{};
        entry.identity = identity;
        // The u64 overload is selected explicitly: the permutation index is a u32, and letting
        // overload resolution pick would silently combine at 32-bit width.
        entry.identity.permutation_hash =
            HashCombine(identity.program_hash, static_cast<u64>(identity.permutation_index));
        entry.gcn_code.assign(gcn_code.begin(), gcn_code.end());
        entry.spirv.assign(spirv.begin(), spirv.end());
        entry.patch_spirv.assign(patch_spirv.begin(), patch_spirv.end());
        entry.patched = patched;
        entry.generated_sha256 = Sha256OfWords(entry.spirv);
        // The effective module is the patch when one was applied, and the generated SPIR-V
        // otherwise. Keeping both hashes distinct is what stops a consumer from loading the
        // unpatched module and believing it is what the draw executed.
        entry.effective_sha256 =
            patched && !entry.patch_spirv.empty() ? Sha256OfWords(entry.patch_spirv)
                                                  : entry.generated_sha256;
        entry.entry_point = "main";

        g_state.binaries[slot] = std::move(entry);
        g_state.retained_count.store(slot + 1, std::memory_order_relaxed);
    } catch (...) {
        // Capture must never affect compilation.
    }
}

bool FindByPermutationHash(u64 permutation_hash, RetainedBinary& out) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lock{g_state.mutex};
        const u32 count = g_state.retained_count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < count; ++i) {
            if (g_state.binaries[i].identity.permutation_hash == permutation_hash) {
                out = g_state.binaries[i];
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
    return g_state.retained_count.load(std::memory_order_relaxed);
}

bool GetRetained(u32 index, RetainedBinary& out) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lock{g_state.mutex};
        if (index >= g_state.retained_count.load(std::memory_order_relaxed)) {
            return false;
        }
        out = g_state.binaries[index];
        return true;
    } catch (...) {
        return false;
    }
}

void ExportBinaries() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const u32 count = g_state.retained_count.load(std::memory_order_relaxed);
        LOG_INFO(Core_Linker,
                 "DERACINE_SHADER_BINARY_SUMMARY retained={} duplicates={} capacity_dropped={} "
                 "oversize_dropped={} limit={}",
                 count, g_state.duplicate_count.load(std::memory_order_relaxed),
                 g_state.capacity_dropped.load(std::memory_order_relaxed),
                 g_state.oversize_dropped.load(std::memory_order_relaxed), MaxRetainedBinaries);

        std::scoped_lock lock{g_state.mutex};
        for (u32 i = 0; i < count; ++i) {
            const auto& entry = g_state.binaries[i];
            LOG_INFO(Core_Linker,
                     "DERACINE_SHADER_BINARY index={} program_hash={:#x} perm_idx={} "
                     "perm_hash={:#x} hw_stage={} logical_stage={} patched={} gcn_words={} "
                     "spv_words={} patch_words={}",
                     i, entry.identity.program_hash, entry.identity.permutation_index,
                     entry.identity.permutation_hash, entry.identity.hardware_stage,
                     entry.identity.logical_stage, entry.patched ? 1 : 0, entry.gcn_code.size(),
                     entry.spirv.size(), entry.patch_spirv.size());
        }
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

} // namespace Core::ShaderBinaryCapture
