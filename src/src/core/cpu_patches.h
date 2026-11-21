// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/guest_entry_detour.h"

namespace Core {

class MemoryManager;

using GuestEntryObserver = PS4_SYSV_ABI void (*)(const GuestEntryRegisterFrame*, void*) noexcept;

struct GuestEntryObserverHandle {
    VAddr target{};
    EntryPatchPlan patch{};
    u32 original_protection{};
    bool active{};
};

enum class GuestThreadQuiescence : u8 {
    NotConfirmed,
    Confirmed,
};

/// Registers a module for patching, providing an area to generate trampoline code.
void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size);

/// Installs a reversible guest function-entry observer in a registered module.
[[nodiscard]] bool InstallGuestEntryObserver(MemoryManager& memory, VAddr target,
                                             GuestEntryObserver observer, void* user_data,
                                             GuestEntryObserverHandle& handle);

/// Restores the exact bytes and protection saved by InstallGuestEntryObserver. The caller must
/// pass Confirmed only after all guest threads are stopped/joined; this is not a hot-toggle
/// operation. Trampoline storage is retained until module/process teardown, even after removal;
/// use is bounded by the module's fixed 8 MiB trampoline arena.
[[nodiscard]] bool RemoveGuestEntryObserver(MemoryManager& memory,
                                            GuestEntryObserverHandle& handle,
                                            GuestThreadQuiescence quiescence);

/// Applies CPU patches that need to be done before beginning executions.
void PrePatchInstructions(u64 segment_addr, u64 segment_size);

} // namespace Core
