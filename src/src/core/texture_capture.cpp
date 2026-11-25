// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/texture_capture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Core::TextureCapture {

namespace {

struct State {
    std::atomic<bool> enabled{};
    std::atomic<int> forced_enabled{-1};
    std::mutex mutex;
    std::vector<ImageRecord> images;
    std::vector<SamplerRecord> samplers;
    u64 next_sampler_id{1};
    u64 draws_seen{};
    u32 snapshots_taken{};
    u64 snapshot_provenance{};
    bool capacity_exhausted{};
    std::atomic<u32> sampler_capacity_dropped{};
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
    const int forced = g_state.forced_enabled.load(std::memory_order_relaxed);
    const bool answer = forced < 0 ? enabled : forced != 0;
    g_state.enabled.store(answer, std::memory_order_relaxed);
    return answer;
}

void SetEnabledForTesting(int enabled) noexcept {
    g_state.forced_enabled.store(enabled, std::memory_order_relaxed);
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

void NoteSnapshotTaken(u64 provenance_id) noexcept {
    std::scoped_lock lock{g_state.mutex};
    ++g_state.snapshots_taken;
    // The draw this snapshot belongs to. CommitPendingImages compares against this, so the records
    // kept are the ones for the draw whose contents were actually copied.
    g_state.snapshot_provenance = provenance_id;
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
        // Only the draw a snapshot was actually taken for contributes records. `snapshot_provenance`
        // is set by NoteSnapshotTaken, which runs when a copy was successfully scheduled, so this
        // compares against a value that was established rather than assigning it here and then
        // testing the assignment. A binding record for a draw whose contents were never captured
        // cannot be replayed, and keeping every draw's bindings fills the budget before the
        // selected draw is reached.
        if (g_state.snapshot_provenance != provenance_id) {
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
        // A sampler's identity is the descriptor binding it fills, not its state. One shader may
        // bind identical sampler state at several bindings, and each of those is an independent
        // observation the package must be able to name. Deduplicating on (state, stage) would
        // discard the second binding while still reporting the capture as complete.
        //
        // Only a genuinely identical observation is dropped: same draw, same stage, same binding,
        // same element, same state.
        for (const auto& existing : g_state.samplers) {
            if (existing.guest_hash != record.guest_hash) {
                continue;
            }
            if (existing.stage_hash != record.stage_hash) {
                continue;
            }
            if (existing.provenance_id != record.provenance_id) {
                continue;
            }
            if (existing.vk_binding != record.vk_binding) {
                continue;
            }
            if (existing.has_vk_binding != record.has_vk_binding) {
                continue;
            }
            if (existing.array_element != record.array_element) {
                continue;
            }
            if (existing.associated_image != record.associated_image) {
                continue;
            }
            return;
        }
        if (g_state.samplers.size() >= MaxSamplerRecords) {
            // Capacity exhaustion is reported rather than silently dropping a binding while
            // claiming a complete capture.
            g_state.sampler_capacity_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
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

size_t SamplerCount() noexcept {
    std::scoped_lock lock{g_state.mutex};
    return g_state.samplers.size();
}

u32 SamplerCapacityDropped() noexcept {
    return g_state.sampler_capacity_dropped.load(std::memory_order_relaxed);
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
    g_state.sampler_capacity_dropped.store(0, std::memory_order_relaxed);
    g_pending_images.clear();
    // The capture is opt-in through an environment variable, which a test binary does not set. The
    // forced flag is the existing mechanism for that, so the reset uses it rather than adding a
    // second way to enable capture.
    g_state.forced_enabled.store(1, std::memory_order_relaxed);
}

} // namespace Core::TextureCapture
