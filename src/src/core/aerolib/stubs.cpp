// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstring>

#include "common/logging/log.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"
#include "core/libraries/kernel/file_system.h"

namespace Core::AeroLib {

// Helper to provide stub implementations for missing functions
//
// This works by pre-compiling generic stub functions ("slots"), and then
// on lookup, setting up the nid_entry they are matched with
//
// If it runs out of stubs with name information, it will return
// a default implementation without function name details

// Bloodborne's eboot plus late-loaded libc exceeds 8192 imports. Keep enough named slots for the
// libc/FIOS boundary so missing imports remain diagnosable and can be routed to HLE functions.
// Déraciné's PSVR module set (libSceNpToolkit2, SigninDialog, Smart, HeadTracker...) goes past
// 12288, and a module that lands on UnknownStub gets a zero return it does not expect.
constexpr u32 MAX_STUBS = 32768;

u64 UnresolvedStub() {
    LOG_ERROR(Core, "Returning zero to {}", __builtin_return_address(0));
    return 0;
}

static u64 UnknownStub(u64 arg0, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5) {
    static std::atomic_uint32_t traces{};
    if (traces.fetch_add(1, std::memory_order_relaxed) < 256) {
        LOG_ERROR(Core,
                  "UnknownStub caller={} args={:#x},{:#x},{:#x},{:#x},{:#x},{:#x}; returning zero",
                  __builtin_return_address(0), arg0, arg1, arg2, arg3, arg4, arg5);
    }
    return 0;
}

static const NidEntry* stub_nids[MAX_STUBS];
static std::string stub_nids_unknown[MAX_STUBS];

static u64 CommonStub(int stub_index, void* addr) {
    auto entry = stub_nids[stub_index];
    if (entry) {
        LOG_ERROR(Core, "Stub: {} (nid: {}) called, returning zero to {}", entry->name, entry->nid,
                  addr);
    } else {
        LOG_ERROR(Core, "Stub: Unknown (nid: {}) called, returning zero to {}",
                  stub_nids_unknown[stub_index], addr);
    }
    return 0;
}

template <int stub_index>
static u64 CommonStubTemplate() {
    return CommonStub(stub_index, __builtin_return_address(0));
}

// A fold over 32768 entries exceeds clang's expression nesting limit, so fill by halving the
// range: depth log2(MAX_STUBS), one instantiation per slot.
template <size_t Lo, size_t Hi>
static void FillStubRange(std::array<u64 (*)(), MAX_STUBS>& out) {
    if constexpr (Hi - Lo == 1) {
        out[Lo] = &CommonStubTemplate<static_cast<int>(Lo)>;
    } else {
        constexpr size_t Mid = Lo + (Hi - Lo) / 2;
        FillStubRange<Lo, Mid>(out);
        FillStubRange<Mid, Hi>(out);
    }
}

static std::array<u64 (*)(), MAX_STUBS> stub_handlers = [] {
    std::array<u64 (*)(), MAX_STUBS> handlers{};
    FillStubRange<0, MAX_STUBS>(handlers);
    return handlers;
}();
static u32 UsedStubEntries;

u64 GetStub(const char* nid) {
    // These heavily used file APIs occur after the named-stub capacity in the generated aerolib
    // table. LLE modules such as Bloodborne's libc still import them late, so preserve their HLE
    // implementations instead of collapsing them to UnknownStub.
    if (std::strcmp(nid, "AqBioC2vF3I") == 0) {
        return reinterpret_cast<u64>(&Libraries::Kernel::posix_read);
    }
    if (std::strcmp(nid, "Cg4srZ6TKbU") == 0) {
        return reinterpret_cast<u64>(&Libraries::Kernel::sceKernelRead);
    }
    if (std::strcmp(nid, "Oy6IpwgtYOk") == 0) {
        return reinterpret_cast<u64>(&Libraries::Kernel::posix_lseek);
    }
    if (std::strcmp(nid, "oib76F-12fk") == 0) {
        return reinterpret_cast<u64>(&Libraries::Kernel::sceKernelLseek);
    }
    if (std::strcmp(nid, "kBwCPsYX-m4") == 0) {
        return reinterpret_cast<u64>(&Libraries::Kernel::sceKernelFstat);
    }
    if (UsedStubEntries >= MAX_STUBS) {
        static std::atomic_bool reported{};
        if (!reported.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR(Core, "Stub table exhausted after {} imports; nid {} gets UnknownStub",
                      MAX_STUBS, nid);
        }
        return (u64)&UnknownStub;
    }

    const auto entry = FindByNid(nid);
    if (!entry) {
        stub_nids_unknown[UsedStubEntries] = nid;
    } else {
        stub_nids[UsedStubEntries] = entry;
    }

    return (u64)stub_handlers[UsedStubEntries++];
}

} // namespace Core::AeroLib
