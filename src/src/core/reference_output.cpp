// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/reference_output.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "common/logging/log.h"
#include "common/sha256.h"

namespace Core::ReferenceOutput {
namespace {

/// One retained reference pair. The bound is small because a reference is only useful for the draw
/// that is actually being packaged.
inline constexpr size_t MaxReferences = 4;

struct Entry {
    u64 provenance_id{};
    ReferenceImage pre{};
    ReferenceImage post{};
    bool has_pre{};
    bool has_post{};
};

struct CaptureState {
    std::atomic<bool> enabled{};
    std::atomic<u32> count{};
    std::atomic<u32> oversize_dropped{};
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
    LOG_INFO(Core_Linker, "DERACINE_REFERENCE_OUTPUT_INSTALL limit={} max_bytes={}",
             MaxReferences, MaxReferenceBytes);
}

void NotePreDraw(u64 provenance_id, const ReferenceImage& image) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed) || provenance_id == 0) {
            return;
        }
        if (image.bytes.size() > MaxReferenceBytes) {
            g_state.oversize_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::scoped_lock lock{g_state.mutex};
        Entry* entry = FindOrCreate(provenance_id);
        if (entry == nullptr) {
            return;
        }
        entry->pre = image;
        entry->has_pre = true;
    } catch (...) {
        // Capture must never affect rendering.
    }
}

void NotePostDraw(u64 provenance_id, const ReferenceImage& image) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed) || provenance_id == 0) {
            return;
        }
        if (image.bytes.size() > MaxReferenceBytes) {
            g_state.oversize_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::scoped_lock lock{g_state.mutex};
        Entry* entry = FindOrCreate(provenance_id);
        if (entry == nullptr) {
            return;
        }
        entry->post = image;
        entry->has_post = true;
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
            if (!entry.has_post) {
                return false;
            }
            pre = entry.pre;
            post = entry.post;
            has_pre = entry.has_pre;
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

void ExportReferences() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const u32 count = g_state.count.load(std::memory_order_relaxed);
        LOG_INFO(Core_Linker,
                 "DERACINE_REFERENCE_OUTPUT_SUMMARY references={} oversize_dropped={} limit={}",
                 count, g_state.oversize_dropped.load(std::memory_order_relaxed), MaxReferences);

        std::scoped_lock lock{g_state.mutex};
        for (u32 i = 0; i < count; ++i) {
            const auto& entry = g_state.entries[i];
            LOG_INFO(Core_Linker,
                     "DERACINE_REFERENCE_OUTPUT provenance_id={} has_pre={} has_post={} "
                     "width={} height={} format={} pre_sha256={} post_sha256={}",
                     entry.provenance_id, entry.has_pre ? 1 : 0, entry.has_post ? 1 : 0,
                     entry.post.width, entry.post.height, entry.post.format,
                     entry.has_pre ? Sha256Hex(entry.pre.bytes) : std::string{"none"},
                     entry.has_post ? Sha256Hex(entry.post.bytes) : std::string{"none"});
        }
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

} // namespace Core::ReferenceOutput
