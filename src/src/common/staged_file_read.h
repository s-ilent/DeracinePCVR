// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#include "common/types.h"

namespace Common::FS {

// Number of bytes a read of `size` bytes can return from `position` in a file of `file_size`
// bytes. Computed without unsigned underflow when the position has reached or passed EOF.
inline u64 ReadableBytes(u64 position, u64 file_size, u64 size) {
    return position < file_size ? std::min<u64>(size, file_size - position) : 0;
}

// OS reads copy kernel to user space without faulting through the emulator's page handler, so a
// read into a guest page that is still GPU-tracked fails (ERROR_INVALID_USER_BUFFER on Windows)
// instead of being untracked on the store. Stage into bounded host memory and copy with CPU
// stores, which do take the guest page handler.
template <typename Reader>
std::size_t ReadStaged(void* destination, std::size_t size, Reader&& read) {
    constexpr std::size_t ChunkSize = 1_MB;
    thread_local std::vector<std::byte> staging;

    if (size == 0) {
        return 0;
    }

    staging.resize(std::min(size, ChunkSize));
    std::size_t total = 0;
    while (total < size) {
        const auto requested = std::min(size - total, staging.size());
        const auto actual = read(staging.data(), requested);
        if (actual != 0) {
            std::memcpy(static_cast<std::byte*>(destination) + total, staging.data(), actual);
        }
        total += actual;
        if (actual < requested) {
            break;
        }
    }
    return total;
}

} // namespace Common::FS
