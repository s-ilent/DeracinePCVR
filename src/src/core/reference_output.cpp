// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/reference_output.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "common/logging/log.h"
#include "common/sha256.h"

namespace Core::ReferenceOutput {
namespace {

struct Entry {
    u64 provenance_id{};
    ReferenceSlot pre{};
    ReferenceSlot post{};
};

struct CaptureState {
    std::atomic<bool> enabled{};
    std::atomic<size_t> max_bytes{DefaultMaxReferenceBytes};
    std::atomic<u64> next_transfer_id{1};
    std::atomic<u32> count{};
    std::atomic<u32> pending_count{};
    std::atomic<u32> failure_count{};
    std::mutex mutex;
    std::array<Entry, MaxReferences> entries{};
};

CaptureState g_state{};

[[nodiscard]] std::string Sha256Hex(const std::vector<u8>& bytes) {
    sha256::SHA256 hasher;
    if (!bytes.empty()) {
        hasher.update(bytes.data(), bytes.size());
    }
    const auto digest = hasher.final();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const u8 byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

/// Finds the entry for a draw, creating it when there is room. Caller holds the mutex.
[[nodiscard]] Entry* FindOrCreate(u64 provenance_id) noexcept {
    const u32 count = g_state.count.load(std::memory_order_relaxed);
    for (u32 i = 0; i < count; ++i) {
        if (g_state.entries[i].provenance_id == provenance_id) {
            return &g_state.entries[i];
        }
    }
    if (count >= MaxReferences) {
        return nullptr;
    }
    auto& entry = g_state.entries[count];
    entry = {};
    entry.provenance_id = provenance_id;
    g_state.count.store(count + 1, std::memory_order_relaxed);
    return &entry;
}

[[nodiscard]] ReferenceSlot& SlotFor(Entry& entry, bool is_pre) noexcept {
    return is_pre ? entry.pre : entry.post;
}

} // namespace

bool IsEnabled() noexcept {
    return g_state.enabled.load(std::memory_order_relaxed);
}

size_t MaxBytes() noexcept {
    return g_state.max_bytes.load(std::memory_order_relaxed);
}

void TryInstall() noexcept {
    const char* value = std::getenv(EnableEnvironmentVariable.data());
    if (value == nullptr || std::string_view{value} != "1") {
        return;
    }

    // The bound is configurable because a full eye buffer does not fit a default that is safe to
    // allocate unconditionally. A value outside the accepted range is refused rather than clamped,
    // so a typo cannot silently produce a bound the caller did not ask for.
    size_t max_bytes = DefaultMaxReferenceBytes;
    if (const char* bound = std::getenv(MaxBytesEnvironmentVariable.data());
        bound != nullptr && *bound != '\0') {
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(bound, &end, 10);
        if (end == bound || *end != '\0' || parsed == 0 || parsed > HardMaxReferenceBytes) {
            LOG_WARNING(Core_Linker,
                        "Reference output refused: {} must be a byte count up to {}",
                        MaxBytesEnvironmentVariable, HardMaxReferenceBytes);
            return;
        }
        max_bytes = static_cast<size_t>(parsed);
    }
    g_state.max_bytes.store(max_bytes, std::memory_order_relaxed);
    g_state.enabled.store(true, std::memory_order_relaxed);
    LOG_INFO(Core_Linker, "DERACINE_REFERENCE_OUTPUT_INSTALL limit={} max_bytes={}",
             MaxReferences, max_bytes);
}

u64 NextTransferId() noexcept {
    return g_state.next_transfer_id.fetch_add(1, std::memory_order_relaxed);
}

void NoteCaptureFailure(u64 provenance_id, bool is_pre, CaptureStatus status) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed) || provenance_id == 0) {
            return;
        }
        std::scoped_lock lock{g_state.mutex};
        Entry* entry = FindOrCreate(provenance_id);
        if (entry == nullptr) {
            return;
        }
        auto& slot = SlotFor(*entry, is_pre);
        slot.status = status;
        slot.image = {};
        g_state.failure_count.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        // Capture must never affect rendering.
    }
}

void NoteCapturePending(u64 provenance_id, bool is_pre, u64 transfer_id, u32 width, u32 height,
                        u32 format, u32 bytes_per_pixel) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed) || provenance_id == 0) {
            return;
        }
        std::scoped_lock lock{g_state.mutex};
        Entry* entry = FindOrCreate(provenance_id);
        if (entry == nullptr) {
            return;
        }
        auto& slot = SlotFor(*entry, is_pre);
        slot.status = CaptureStatus::Pending;
        slot.transfer_id = transfer_id;
        slot.image = {};
        slot.image.width = width;
        slot.image.height = height;
        slot.image.format = format;
        slot.image.bytes_per_pixel = bytes_per_pixel;
        g_state.pending_count.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        // Capture must never affect rendering.
    }
}

void CompleteCapture(u64 provenance_id, bool is_pre, u64 transfer_id, std::vector<u8>&& bytes,
                     std::string&& sha256) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        std::scoped_lock lock{g_state.mutex};
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < count; ++i) {
            auto& entry = g_state.entries[i];
            if (entry.provenance_id != provenance_id) {
                continue;
            }
            auto& slot = SlotFor(entry, is_pre);
            // The transfer identity is what stops a late readback from a previous capture being
            // attributed to this one. A mismatch means the slot was reused, so the bytes are
            // dropped rather than stored against the wrong draw.
            if (slot.status != CaptureStatus::Pending || slot.transfer_id != transfer_id) {
                return;
            }
            slot.image.bytes = std::move(bytes);
            slot.image.sha256 = std::move(sha256);
            slot.status = CaptureStatus::Complete;
            g_state.pending_count.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
    } catch (...) {
        // Capture must never affect rendering.
    }
}

bool GetReference(u64 provenance_id, ReferenceImage& pre, ReferenceImage& post,
                  bool& has_pre) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lock{g_state.mutex};
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < count; ++i) {
            const auto& entry = g_state.entries[i];
            if (entry.provenance_id != provenance_id) {
                continue;
            }
            // A reference is only usable when the post-draw readback has completed. A pending or
            // failed capture is not a reference, and reporting one would let an unverified hash
            // become an oracle.
            if (entry.post.status != CaptureStatus::Complete) {
                return false;
            }
            post = entry.post.image;
            has_pre = entry.pre.status == CaptureStatus::Complete;
            if (has_pre) {
                pre = entry.pre.image;
            }
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool HasPending(u64 provenance_id) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        std::scoped_lock lock{g_state.mutex};
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < count; ++i) {
            const auto& entry = g_state.entries[i];
            if (entry.provenance_id != provenance_id) {
                continue;
            }
            return entry.pre.status == CaptureStatus::Pending ||
                   entry.post.status == CaptureStatus::Pending;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool WaitForPending() noexcept {
    // The deferred readbacks run on the scheduler's priority thread once the GPU has passed the
    // tick the copy was recorded at. This waits for that to happen rather than forcing a
    // device-wide idle: the scheduler is already driving the GPU, and the only thing needed is for
    // the pending operations to drain.
    for (u32 attempt = 0; attempt < 2000; ++attempt) {
        if (g_state.pending_count.load(std::memory_order_relaxed) == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return g_state.pending_count.load(std::memory_order_relaxed) == 0;
}

void ExportReferences() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        LOG_INFO(Core_Linker,
                 "DERACINE_REFERENCE_OUTPUT_SUMMARY references={} pending={} failures={} "
                 "max_bytes={} limit={}",
                 count, g_state.pending_count.load(std::memory_order_relaxed),
                 g_state.failure_count.load(std::memory_order_relaxed), MaxBytes(), MaxReferences);

        std::scoped_lock lock{g_state.mutex};
        for (u32 i = 0; i < count; ++i) {
            const auto& entry = g_state.entries[i];
            LOG_INFO(Core_Linker,
                     "DERACINE_REFERENCE_OUTPUT provenance_id={} pre_status={} post_status={} "
                     "width={} height={} format={} pre_sha256={} post_sha256={}",
                     entry.provenance_id, CaptureStatusName(entry.pre.status),
                     CaptureStatusName(entry.post.status), entry.post.image.width,
                     entry.post.image.height, entry.post.image.format,
                     entry.pre.status == CaptureStatus::Complete ? entry.pre.image.sha256
                                                                : std::string{"none"},
                     entry.post.status == CaptureStatus::Complete ? entry.post.image.sha256
                                                                  : std::string{"none"});
        }
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

} // namespace Core::ReferenceOutput
