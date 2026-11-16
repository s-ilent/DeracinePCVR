// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/types.h"

namespace Core::Unreal {

// Reads guest memory as it currently is. A live game frees and recycles objects while this runs,
// so every read has to fail cleanly on an address that is no longer mapped instead of faulting.
class GuestMemory {
public:
    virtual ~GuestMemory() = default;

    virtual bool Read(VAddr address, void* out, u64 size) const = 0;

    template <typename T>
    bool Read(VAddr address, T& out) const {
        return Read(address, &out, sizeof(T));
    }

    template <typename T>
    bool ReadAt(VAddr address, u64 offset, T& out) const {
        return Read(address + offset, &out, sizeof(T));
    }

    // Reads a NUL-terminated string, stopping after `limit` bytes. Returns false only when the
    // bytes could not be read at all; an unterminated or empty string is still a valid read.
    bool ReadString(VAddr address, std::string& out, u64 limit) const;
};

// Reads through the emulator's live guest address space, refusing anything that is not mapped.
class LiveGuestMemory final : public GuestMemory {
public:
    bool Read(VAddr address, void* out, u64 size) const override;
};

} // namespace Core::Unreal
