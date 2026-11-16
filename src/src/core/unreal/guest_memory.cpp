// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/unreal/guest_memory.h"

#include <cstring>

#include "core/memory.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Core::Unreal {

bool GuestMemory::ReadString(VAddr address, std::string& out, u64 limit) const {
    out.clear();
    if (limit == 0) {
        return false;
    }
    // Read in small blocks so a short string does not need the whole limit to be mapped.
    constexpr u64 Block = 64;
    std::string buffer;
    for (u64 read = 0; read < limit; read += Block) {
        const u64 size = std::min<u64>(Block, limit - read);
        buffer.resize(size);
        if (!Read(address + read, buffer.data(), size)) {
            return false;
        }
        for (const char c : buffer) {
            if (c == '\0') {
                return true;
            }
            out.push_back(c);
        }
    }
    return true;
}

bool LiveGuestMemory::Read(VAddr address, void* out, u64 size) const {
    if (size == 0) {
        return true;
    }
#ifdef _WIN32
    // The guest address space is mapped into this process, so ReadProcessMemory reports an
    // unmapped or unreadable range as a failure instead of raising an access violation.
    SIZE_T done = 0;
    return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(address), out,
                               static_cast<SIZE_T>(size), &done) != 0 &&
           done == size;
#else
    const auto* memory = Core::Memory::Instance();
    if (!memory->IsValidMapping(address, size)) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
#endif
}

} // namespace Core::Unreal
