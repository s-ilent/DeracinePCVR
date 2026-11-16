// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <span>

#include "common/types.h"

namespace Common {

// One physically contiguous run of guest backing, in the order a guest write reaches it.
struct BackingWriteSegment {
    u8* destination;
    u64 size;
};

// Copies `size` bytes of `data` into the given backing segments, in order, and returns the
// number of bytes copied.
inline u64 WriteBackingSegments(const u8* data, u64 size,
                                std::span<const BackingWriteSegment> segments) {
    u64 copied = 0;
    for (const auto& segment : segments) {
        if (copied >= size) {
            break;
        }
        const u64 chunk = std::min(segment.size, size - copied);
        std::memcpy(segment.destination, data, chunk);
        copied += chunk;
    }
    return copied;
}

} // namespace Common
