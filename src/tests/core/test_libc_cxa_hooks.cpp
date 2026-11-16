// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <utility>

#include "core/libraries/libc_internal/libc_internal.h"
#include "core/libraries/libc_internal/libc_internal_cxa.h"
#include "core/loader/symbols_resolver.h"

namespace {

// The library the C++ runtime entry points are imported from, as libSceNpToolkit2 does.
Core::Loader::SymbolResolver CxaImport(const char* nid) {
    return Core::Loader::SymbolResolver{
        .name = nid,
        .nidName = "",
        .library = "libSceLibcInternal",
        .library_version = 1,
        .module = "libSceLibcInternal",
        .type = Core::Loader::SymbolType::Function,
    };
}

void StaticDestructor(void*) {}

} // Anonymous namespace

TEST(LibcCxaHooks, GuardAcquireRunsTheInitializerOnce) {
    u64 guard[2] = {0, 0};

    EXPECT_EQ(Libraries::LibcInternal::fex_libc_cxa_guard_acquire(guard), 1)
        << "the first acquisition must tell the caller to run the initializer";

    Libraries::LibcInternal::fex_libc_cxa_guard_release(guard);

    EXPECT_EQ(Libraries::LibcInternal::fex_libc_cxa_guard_acquire(guard), 0)
        << "a released guard must report the static as initialized";
}

TEST(LibcCxaHooks, GuardAbortLeavesTheStaticUninitialized) {
    u64 guard[2] = {0, 0};

    ASSERT_EQ(Libraries::LibcInternal::fex_libc_cxa_guard_acquire(guard), 1);
    Libraries::LibcInternal::fex_libc_cxa_guard_abort(guard);

    EXPECT_EQ(Libraries::LibcInternal::fex_libc_cxa_guard_acquire(guard), 1);
    Libraries::LibcInternal::fex_libc_cxa_guard_release(guard);
}

TEST(LibcCxaHooks, AtexitAcceptsStaticDestructors) {
    EXPECT_EQ(Libraries::LibcInternal::fex_libc_cxa_atexit(&StaticDestructor, nullptr, nullptr), 0);
    EXPECT_EQ(Libraries::LibcInternal::fex_libc_cxa_atexit(nullptr, nullptr, nullptr), -1);
}

// libSceNpToolkit2 imports these from libSceLibcInternal and dies in its static initialization
// when they resolve to a stub that reports the guard as already taken.
TEST(LibcCxaHooks, RegisterLibResolvesTheCxxRuntimeForLibcInternal) {
    Core::Loader::SymbolsResolver resolver;
    Libraries::LibcInternal::RegisterLib(&resolver);

    for (const auto& [nid, name] :
         {std::pair{"3GPpjQdAMTw", "__cxa_guard_acquire"},
          std::pair{"9rAeANT2tyE", "__cxa_guard_release"},
          std::pair{"2emaaluWzUw", "__cxa_guard_abort"},
          std::pair{"tsvEmnenz48", "__cxa_atexit"}}) {
        const auto record = resolver.FindSymbol(CxaImport(nid));
        ASSERT_NE(record, nullptr) << name << " is not registered for libSceLibcInternal";
        EXPECT_NE(record->virtual_address, 0u) << name << " resolves to no guest address";
    }
}
