// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/texture_capture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "common/logging/log.h"

namespace Core::TextureCapture {

namespace {

struct State {
    std::atomic<bool> enabled{};
    std::mutex mutex;
    std::vector<ImageRecord> images;
    std::vector<SamplerRecord> samplers;
    u64 next_sampler_id{1};
    u64 draws_seen{};
    u32 snapshots_taken{};
    u64 snapshot_provenance{};
    bool capacity_exhausted{};
};

State g_state{};

/// Per-draw image records, filled by the binding path and committed only for the draw that is
/// selected. Thread-local because a draw's bindings never span threads.
thread_local std::vector<ImageRecord> g_pending_images;

} // namespace

const char* StatusName(u8 status) noexcept {
    switch (static_cast<ContentStatus>(status)) {
    case ContentStatus::BindingOnly:
        return "binding_only";
    case ContentStatus::HostContentCaptured:
        return "host_content_captured";
    case ContentStatus::CopyPending:
        return "copy_pending";
    case ContentStatus::Unsupported:
        return "unsupported";
    case ContentStatus::Partial:
        return "partial";
    case ContentStatus::Invalid:
        return "invalid";
    }
    return "unknown";
}

bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv(EnableEnvironmentVariable.data());
        if (value == nullptr || value[0] == '\0') {
            return false;
        }
        return value[0] == '1' && value[1] == '\0';
    }();
    g_state.enabled.store(enabled, std::memory_order_relaxed);
    return enabled;
}

u64 NextTransferId() noexcept {
    static std::atomic<u64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

bool ShouldSnapshot() noexcept {
    if (!Enabled())
        return false;
    std::scoped_lock lock{g_state.mutex};
    return g_state.snapshots_taken < MaxSnapshotDraws;
}

void NoteSnapshotTaken() noexcept {
    std::scoped_lock lock{g_state.mutex};
    ++g_state.snapshots_taken;
}

bool CapacityExhausted() noexcept {
    std::scoped_lock lock{g_state.mutex};
    return g_state.capacity_exhausted;
}

bool HasContentFor(u64 provenance_id) noexcept {
    std::scoped_lock lock{g_state.mutex};
    for (const auto& image : g_state.images) {
        if (image.provenance_id == provenance_id && !image.bytes.empty()) {
            return true;
        }
    }
    return false;
}

void BeginDraw() noexcept {
    if (!Enabled())
        return;
    // The pending list is per draw. Without this it fills with every earlier draw's records, and
    // the selected draw's own records are dropped by the cap before they can be committed.
    g_pending_images.clear();
    std::scoped_lock lock{g_state.mutex};
    ++g_state.draws_seen;
}

void NoteImageBinding(const ImageRecord& record) noexcept {
    try {
        if (!Enabled())
            return;
        // Records go to a per-draw pending list first. The snapshot decision is made at the end of
        // the draw's binding, after these records exist, so committing straight to the store would
        // discard the very draw that was selected.
        if (g_pending_images.size() >= MaxImageRecords)
            return;
        g_pending_images.push_back(record);
    } catch (...) {
        // Observation must never affect the binding path.
    }
}

void CommitPendingImages(u64 provenance_id) noexcept {
    try {
        std::scoped_lock lock{g_state.mutex};
        // A draw's bindings can be committed more than once: BindTextures runs per stage, and a
        // stage can be visited again. Committing is therefore incremental rather than a one-shot
        // latch. The draw is counted once, and a record already stored for this draw is not
        // stored again, so a repeated stage does not duplicate its bindings.
        if (g_state.snapshot_provenance != provenance_id) {
            g_state.snapshot_provenance = provenance_id;
        }
        // Only the draw that is actually snapshotted contributes records. A binding record for a
        // draw whose contents were never captured cannot be replayed, and keeping every draw's
        // bindings fills the budget before the selected draw is reached.
        const bool snapshot_draw = g_state.snapshot_provenance == provenance_id &&
                                   g_state.snapshots_taken != 0;
        if (!snapshot_draw) {
            g_pending_images.clear();
            return;
        }
        for (const auto& record : g_pending_images) {
            if (record.provenance_id != provenance_id)
                continue;
            if (g_state.images.size() >= MaxImageRecords) {
                // Capacity exhaustion is reported rather than silently dropping the rest: a
                // package built from a truncated set must not claim complete capture.
                g_state.capacity_exhausted = true;
                break;
            }
            // The same descriptor observed twice is one binding. The identity is the stage, the
            // unified binding and the image generation, which is what distinguishes a repeated
            // visit from a genuinely separate binding of the same image.
            const bool duplicate = std::ranges::any_of(
                g_state.images, [&record](const ImageRecord& existing) {
                    return existing.provenance_id == record.provenance_id &&
                           existing.stage_hash == record.stage_hash &&
                           existing.vk_binding == record.vk_binding &&
                           existing.image_uid == record.image_uid &&
                           existing.mip_first == record.mip_first &&
                           existing.layer_first == record.layer_first;
                });
            if (duplicate)
                continue;
            g_state.images.push_back(record);
        }
        g_pending_images.clear();
    } catch (...) {
        // Observation must never affect the binding path.
    }
}

void DiscardPendingImages() noexcept {
    g_pending_images.clear();
}

void StoreLateImage(ImageRecord record) noexcept {
    try {
        std::scoped_lock lock{g_state.mutex};
        if (g_state.images.size() >= MaxImageRecords)
            return;
        g_state.images.push_back(std::move(record));
    } catch (...) {
        // Observation must never affect the rendering path.
    }
}

void NoteSampler(const SamplerRecord& record) noexcept {
    try {
        if (!Enabled())
            return;
        std::scoped_lock lock{g_state.mutex};
        // Samplers are cached by the guest descriptor hash, so the same state recurs across draws.
        // Record each distinct (state, stage) pair once: the same guest state bound in two stages
        // is two records, because a sampler's image association is per shader program and joining
        // on the state alone would pair a sampler with an image from a different stage.
        for (const auto& existing : g_state.samplers) {
            if (existing.guest_hash == record.guest_hash &&
                existing.stage_hash == record.stage_hash) {
                return;
            }
        }
        if (g_state.samplers.size() >= MaxSamplerRecords)
            return;
        SamplerRecord stored = record;
        stored.id = g_state.next_sampler_id++;
        g_state.samplers.push_back(stored);
    } catch (...) {
        // Observation must never affect the binding path.
    }
}

void EndDraw() noexcept {}

size_t ImageCount() noexcept {
    std::scoped_lock lock{g_state.mutex};
    return g_state.images.size();
}

std::span<const ImageRecord> Images() noexcept {
    std::scoped_lock lock{g_state.mutex};
    return {g_state.images.data(), g_state.images.size()};
}

std::span<const SamplerRecord> Samplers() noexcept {
    std::scoped_lock lock{g_state.mutex};
    return {g_state.samplers.data(), g_state.samplers.size()};
}

void ResetForTesting() noexcept {
    std::scoped_lock lock{g_state.mutex};
    g_state.images.clear();
    g_state.samplers.clear();
    g_state.next_sampler_id = 1;
    g_state.draws_seen = 0;
    g_state.snapshots_taken = 0;
    g_state.snapshot_provenance = 0;
    g_state.capacity_exhausted = false;
    g_pending_images.clear();
}

} // namespace Core::TextureCapture
