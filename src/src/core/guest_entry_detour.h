// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

#include "common/types.h"

namespace Core {

/// Registers saved at a guest function entry before invoking the host observer.
/// original_rsp points to the guest return address at method entry. Full YMM state is captured so
/// AVX2 observer code cannot alter incoming SysV vector arguments or other live vector values.
struct GuestEntryRegisterFrame {
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rdi;
    u64 rsi;
    u64 rbp;
    u64 rbx;
    u64 rdx;
    u64 rcx;
    u64 rax;
    u64 rflags;
    std::array<std::array<u8, 32>, 16> ymm;
    VAddr original_rsp;

    [[nodiscard]] VAddr OriginalStackPointer() const noexcept {
        return original_rsp;
    }
};
static_assert(offsetof(GuestEntryRegisterFrame, ymm) == 16 * sizeof(u64));
static_assert(offsetof(GuestEntryRegisterFrame, original_rsp) == 16 * sizeof(u64) + 16 * 32);
static_assert(sizeof(GuestEntryRegisterFrame) == 16 * sizeof(u64) + 16 * 32 + sizeof(VAddr));

/// A prepared near-jump patch and the exact bytes it replaces.
struct EntryPatchPlan {
    static constexpr size_t MaxBytes = 32;

    std::array<u8, MaxBytes> original{};
    std::array<u8, MaxBytes> replacement{};
    size_t size{};
};

/// Build an E9 rel32 patch, padding only after complete instructions.
[[nodiscard]] inline std::optional<EntryPatchPlan> BuildEntryPatchPlan(
    VAddr target, VAddr destination, std::span<const u8> original,
    std::span<const u8> instruction_lengths) {
    constexpr size_t JumpSize = 5;
    if (original.size() < JumpSize || original.size() > EntryPatchPlan::MaxBytes ||
        target > (std::numeric_limits<VAddr>::max)() - original.size()) {
        return std::nullopt;
    }

    size_t decoded_size = 0;
    for (const u8 length : instruction_lengths) {
        if (length == 0 || length > 15 || decoded_size > original.size() ||
            length > original.size() - decoded_size) {
            return std::nullopt;
        }
        decoded_size += length;
    }
    if (decoded_size != original.size()) {
        return std::nullopt;
    }

    const VAddr jump_origin = target + JumpSize;
    s64 displacement = 0;
    if (destination >= jump_origin) {
        const u64 delta = destination - jump_origin;
        if (delta > static_cast<u64>((std::numeric_limits<s32>::max)())) {
            return std::nullopt;
        }
        displacement = static_cast<s64>(delta);
    } else {
        const u64 delta = jump_origin - destination;
        if (delta > static_cast<u64>((std::numeric_limits<s32>::max)()) + 1) {
            return std::nullopt;
        }
        displacement = -static_cast<s64>(delta);
    }

    EntryPatchPlan plan;
    plan.size = original.size();
    plan.replacement.fill(0x90);
    std::memcpy(plan.original.data(), original.data(), original.size());
    plan.replacement[0] = 0xE9;
    const auto relative = static_cast<u32>(static_cast<s32>(displacement));
    for (size_t i = 0; i < sizeof(relative); ++i) {
        plan.replacement[1 + i] = static_cast<u8>(relative >> (i * 8));
    }
    return plan;
}

/// Apply a prepared patch to writable storage.
[[nodiscard]] inline bool ApplyEntryPatch(std::span<u8> target, const EntryPatchPlan& plan) {
    if (plan.size < 5 || plan.size > EntryPatchPlan::MaxBytes || target.size() < plan.size) {
        return false;
    }
    std::memcpy(target.data(), plan.replacement.data(), plan.size);
    return true;
}

[[nodiscard]] inline bool MatchesOriginalEntryPatch(std::span<const u8> target,
                                                    const EntryPatchPlan& plan) {
    return plan.size >= 5 && plan.size <= EntryPatchPlan::MaxBytes && target.size() >= plan.size &&
           std::memcmp(target.data(), plan.original.data(), plan.size) == 0;
}

/// Apply only if the original bytes are still present.
[[nodiscard]] inline bool ApplyEntryPatchIfMatchesOriginal(std::span<u8> target,
                                                           const EntryPatchPlan& plan) {
    if (!MatchesOriginalEntryPatch(target, plan))
        return false;
    std::memcpy(target.data(), plan.replacement.data(), plan.size);
    return true;
}

/// Compare the current bytes with the installed E9 patch without changing memory.
[[nodiscard]] inline bool MatchesInstalledEntryPatch(std::span<const u8> target,
                                                     const EntryPatchPlan& plan) {
    return plan.size >= 5 && plan.size <= EntryPatchPlan::MaxBytes && target.size() >= plan.size &&
           std::memcmp(target.data(), plan.replacement.data(), plan.size) == 0;
}

/// Restore the exact bytes captured by BuildEntryPatchPlan.
[[nodiscard]] inline bool RestoreEntryPatch(std::span<u8> target, const EntryPatchPlan& plan) {
    if (plan.size < 5 || plan.size > EntryPatchPlan::MaxBytes || target.size() < plan.size) {
        return false;
    }
    std::memcpy(target.data(), plan.original.data(), plan.size);
    return true;
}

/// Compare and restore in one helper so unexpected code changes are never overwritten.
[[nodiscard]] inline bool RestoreEntryPatchIfMatches(std::span<u8> target,
                                                     const EntryPatchPlan& plan) {
    if (!MatchesInstalledEntryPatch(target, plan))
        return false;
    std::memcpy(target.data(), plan.original.data(), plan.size);
    return true;
}

} // namespace Core
