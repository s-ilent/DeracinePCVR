// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdlib>
#include <cstring>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_gpu_timing.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"

namespace Vulkan {

namespace {

/// Whether the one warning about a device that cannot time submissions has been written. Each
/// scheduler tries for itself, and three identical lines at startup would say less than one.
std::atomic<bool> g_unsupported_logged{};

} // namespace

bool GpuTiming::Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_GPU_TIMING");
        if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) {
            return false;
        }
        return true;
    }();
    return enabled;
}

GpuTiming::GpuTiming(const Instance& instance_, MasterSemaphore& semaphore_)
    : instance{instance_}, semaphore{semaphore_} {
    const auto physical = instance.GetPhysicalDevice();
    tick_ns = physical.getProperties().limits.timestampPeriod;
    const auto families = physical.getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    const u32 bits = family < families.size() ? families[family].timestampValidBits : 0;
    valid_mask = bits == 0 ? 0 : (bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1);

    // Nothing records a timestamp into a command buffer here, so no host query reset is asked
    // for: each pair resets its own two queries from inside the submission that then writes them,
    // which keeps the reuse ordered on the GPU's own timeline.
    if (bits == 0 || tick_ns <= 0.0) {
        if (!g_unsupported_logged.exchange(true, std::memory_order_relaxed)) {
            LOG_WARNING(Render_Vulkan,
                        "GPU timing: this device cannot time submissions (timestamp bits {}, {:.2f} "
                        "ns a tick); SHADPS4_GPU_TIMING will report nothing",
                        bits, tick_ns);
        }
        return;
    }

    const auto device = instance.GetDevice();
    auto [result, created] = device.createQueryPool({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = TimerPairs * 2,
    });
    if (result != vk::Result::eSuccess) {
        if (!g_unsupported_logged.exchange(true, std::memory_order_relaxed)) {
            LOG_WARNING(Render_Vulkan, "GPU timing: no query pool: {}", vk::to_string(result));
        }
        return;
    }
    pool = created;
    ready_instances.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan,
             "GPU timing: a timestamp pair around every submission, {} pairs, {:.2f} ns a tick, "
             "{} timestamp bits",
             TimerPairs, tick_ns, bits);
}

GpuTiming::~GpuTiming() {
    if (pool == nullptr) {
        return;
    }
    ready_instances.fetch_sub(1, std::memory_order_relaxed);
    instance.GetDevice().destroyQueryPool(pool);
}

void GpuTiming::OpenSubmission(vk::CommandBuffer cmdbuf) {
    if (pool == nullptr) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (open) {
        return;
    }
    // The slot's previous pair was submitted at least TimerPairs submissions ago and the
    // scheduler never has more than MAX_IN_FLIGHT_SUBMISSIONS in flight, so the GPU is long since
    // through with it. Resetting here, rather than from the host, keeps that ordering on the GPU's
    // own timeline.
    cmdbuf.resetQueryPool(pool, slot * 2, 2);
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, pool, slot * 2);
    open = true;
}

void GpuTiming::CloseSubmission(vk::CommandBuffer cmdbuf, u64 signal_value) {
    if (pool == nullptr) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (!open) {
        return;
    }
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, slot * 2 + 1);
    if (pending.size() >= TimerPairs) {
        // Unreachable while the scheduler holds the in-flight limit, kept so that a slot can
        // never be reused with an unread pair still owed: the submission is counted as dropped
        // rather than silently losing a slot to a stale entry.
        pending.pop_front();
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    pending.push_back(Pending{.tick = signal_value, .slot = slot});
    slot = (slot + 1) % TimerPairs;
    open = false;
}

void GpuTiming::Collect() {
    if (pool == nullptr) {
        return;
    }
    std::scoped_lock lock{mutex};
    const auto device = instance.GetDevice();
    // Exactly the test the scheduler uses to reclaim anything else: whatever already proven idle,
    // and no refresh of the timeline the driver would have to answer for.
    while (!pending.empty() && semaphore.IsFree(pending.front().tick)) {
        const auto [tick, which] = pending.front();
        pending.pop_front();

        std::array<u64, 2> stamps{};
        const vk::Result result = device.getQueryPoolResults(
            pool, which * 2, 2, stamps.size() * sizeof(u64), stamps.data(), sizeof(u64),
            vk::QueryResultFlagBits::e64);
        if (result != vk::Result::eSuccess) {
            unreadable.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const auto measured = math.Add(stamps[0], stamps[1], tick_ns, valid_mask);
        if (!measured.counted) {
            invalid.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        busy_ns.fetch_add(measured.busy_ns, std::memory_order_relaxed);
        // Only time this scheduler spent with nothing of its own in flight: the submission before
        // it ended here, and the GPU started this one there.
        gap_ns.fetch_add(measured.gap_ns, std::memory_order_relaxed);
        submits.fetch_add(1, std::memory_order_relaxed);
    }
}

GpuTiming::Reading GpuTiming::TakeReading() {
    Reading reading;
    reading.busy_ns = busy_ns.exchange(0, std::memory_order_relaxed);
    reading.gap_ns = gap_ns.exchange(0, std::memory_order_relaxed);
    reading.submits = submits.exchange(0, std::memory_order_relaxed);
    reading.dropped = dropped.exchange(0, std::memory_order_relaxed);
    reading.unreadable = unreadable.exchange(0, std::memory_order_relaxed);
    reading.invalid = invalid.exchange(0, std::memory_order_relaxed);
    return reading;
}

} // namespace Vulkan
