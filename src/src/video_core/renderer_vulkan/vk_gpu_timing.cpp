// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_gpu_timing.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"

namespace Vulkan {

namespace {

/// Whether the one warning about a device that cannot time submissions has been written. Each
/// scheduler tries for itself, and three identical lines at startup would say less than one.
std::atomic<bool> g_unsupported_logged{};

/// Where the per-operation totals are gathered out of the retired submissions, and read once a
/// window by the frame statistics. Guarded by its own lock: collecting happens on whichever thread
/// retires a submission and the report on the presenter's.
struct OpTotal {
    double ms{};
    u64 count{};
};
std::mutex g_ops_mutex;
std::map<std::pair<u8, GpuTiming::OpLabel>, OpTotal> g_ops_totals;
/// Time inside a measured submission that no mark owned: the top of pipe up to the first mark.
u64 g_ops_unowned_ns{};
/// Marks that did not fit the submission they belonged to.
u64 g_ops_overflow{};

const char* OpName(GpuTiming::Op op) {
    switch (op) {
    case GpuTiming::Op::Pass:
        return "pass";
    case GpuTiming::Op::Dispatch:
        return "dispatch";
    case GpuTiming::Op::Copy:
        return "copy";
    default:
        return "other";
    }
}

/// A label the marks were made with, spelled out only when a report is written. Every pass of a
/// frame marks, so building these strings at mark time would cost more than the measurement.
std::string OpLabelText(GpuTiming::Op op, const GpuTiming::OpLabel& label) {
    switch (op) {
    case GpuTiming::Op::Pass: {
        std::string text = fmt::format("{}x{} {} colour{}", label.width, label.height, label.colors,
                                       label.colors == 1 ? "" : "s");
        text += label.depth ? " + depth" : "";
        return fmt::format("{} vs {:x} ps {:x}", text, label.vs, label.ps);
    }
    case GpuTiming::Op::Dispatch:
        return fmt::format("dispatch shader {:x}", label.vs);
    case GpuTiming::Op::Copy:
        // Marked where the emulator records the copy, which knows the target but not always the
        // bytes: the size alone is still worth reading next to the title's passes.
        return label.extra != 0 ? fmt::format("copy {}x{} ({} KB)", label.width, label.height,
                                              label.extra)
                                : fmt::format("copy {}x{}", label.width, label.height);
    default:
        return "other";
    }
}

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

bool GpuTiming::ProfilingOps() {
    return Enabled() && Measuring();
}

GpuTiming::GpuTiming(const Instance& instance_, MasterSemaphore& semaphore_)
    : instance{instance_}, semaphore{semaphore_} {
    const auto physical = instance.GetPhysicalDevice();
    tick_ns = physical.getProperties().limits.timestampPeriod;
    const auto families = physical.getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    const u32 bits = family < families.size() ? families[family].timestampValidBits : 0;
    valid_mask = bits == 0 ? 0 : (bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1);

    // No host query reset is asked for: each submission resets its own slot from inside the command
    // buffer that then writes it, so the reset stays ordered on the GPU's timeline. Resetting from
    // the host would need the slot proven idle first, which is what the in-command-buffer reset
    // avoids.
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
        .queryCount = TimerPairs * (MarksPerSubmission + 2),
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
             "GPU timing: a timestamp pair around every submission, and a mark per operation ({} "
             "pairs of {}, {:.2f} ns a tick, {} timestamp bits)",
             TimerPairs, MarksPerSubmission, tick_ns, bits);
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
    const u32 base = slot * (MarksPerSubmission + 2);
    // Every query of this slot is reset before the submission writes any of them. Vulkan requires a
    // query to be reset before it is used again, and this slot's were written by an earlier
    // submission - overwriting them is not a reset. The reset is recorded into the submission's own
    // command buffer rather than done from the host, so it runs on the GPU's timeline after the
    // submission that last used the slot; that is what makes the reuse safe without a host wait.
    // The margin is large: the scheduler keeps at most MAX_IN_FLIGHT_SUBMISSIONS (8) submissions in
    // flight, and a slot only comes round again after TimerPairs (128) of them.
    cmdbuf.resetQueryPool(pool, base, MarksPerSubmission + 2);
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, pool, base);
    marks.clear();
    open = true;
}

void GpuTiming::Mark(vk::CommandBuffer cmdbuf, Op op, const OpLabel& label) {
    if (pool == nullptr) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (!open) {
        return;
    }
    if (marks.size() >= MarksPerSubmission) {
        // Counted rather than dropped: a submission that carried more operations than there is room
        // for is one whose report is short, and that has to be visible in it.
        ++oversized;
        return;
    }
    const u32 base = slot * (MarksPerSubmission + 2);
    const u32 query = base + 1 + static_cast<u32>(marks.size());
    // Bottom of pipe is what the existing per-draw profiler uses inside render passes on this
    // tree; a pass here is marked before it begins, where no stage restriction applies at all.
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, query);
    marks.push_back(MarkRecord{.query = query, .op = op, .label = label});
}

void GpuTiming::CloseSubmission(vk::CommandBuffer cmdbuf, u64 signal_value) {
    if (pool == nullptr) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (!open) {
        return;
    }
    const u32 base = slot * (MarksPerSubmission + 2);
    // The bottom of pipe goes straight after the marks, so a retired submission's stamps are one
    // contiguous read.
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool,
                          base + 1 + static_cast<u32>(marks.size()));
    if (pending.size() >= TimerPairs) {
        // Unreachable while the scheduler holds the in-flight limit, kept so that a slot can never
        // be reused with an unread pair still owed: the submission is counted as dropped rather
        // than silently losing a slot to a stale entry.
        pending.pop_front();
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    if (oversized != 0) {
        std::scoped_lock ops_lock{g_ops_mutex};
        g_ops_overflow += oversized;
        oversized = 0;
    }
    pending.push_back(Pending{.tick = signal_value, .slot = slot, .marks = std::move(marks)});
    marks.clear();
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
        Pending entry = std::move(pending.front());
        pending.pop_front();

        const size_t marks = entry.marks.size();
        std::vector<u64> stamps(marks + 2);
        const vk::Result result = device.getQueryPoolResults(
            pool, entry.slot * (MarksPerSubmission + 2), stamps.size(),
            stamps.size() * sizeof(u64), stamps.data(), sizeof(u64),
            vk::QueryResultFlagBits::e64);
        if (result != vk::Result::eSuccess) {
            unreadable.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // The submission's own execution and the gap before it come from its two ends, unchanged.
        const auto measured =
            math.Add(stamps.front(), stamps[marks + 1], tick_ns, valid_mask);
        if (measured.counted) {
            busy_ns.fetch_add(measured.busy_ns, std::memory_order_relaxed);
            // Only time this scheduler spent with nothing of its own in flight: the submission
            // before it ended here, and the GPU started this one there.
            gap_ns.fetch_add(measured.gap_ns, std::memory_order_relaxed);
            submits.fetch_add(1, std::memory_order_relaxed);
        } else {
            invalid.fetch_add(1, std::memory_order_relaxed);
        }
        if (marks == 0) {
            continue;
        }

        const auto attributed = AttributeIntervals(stamps, tick_ns, valid_mask);
        std::scoped_lock ops_lock{g_ops_mutex};
        g_ops_unowned_ns += attributed.head_ns;
        const size_t attributed_marks =
            std::min(attributed.mark_ns.size(), entry.marks.size());
        for (size_t i = 0; i < attributed_marks; ++i) {
            const MarkRecord& mark = entry.marks[i];
            auto& total =
                g_ops_totals[std::pair<u8, OpLabel>{static_cast<u8>(mark.op), mark.label}];
            total.ms += static_cast<double>(attributed.mark_ns[i]) / 1e6;
            ++total.count;
        }
        invalid.fetch_add(attributed.unusable, std::memory_order_relaxed);
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

const char* GpuTiming::OpKindName(Op op) {
    return OpName(op);
}

std::vector<GpuTiming::TopOp> GpuTiming::TakeTopOps(
    u32 limit, double& measured_ms, double& unowned_ms, u64& overflow,
    std::array<KindTotal, static_cast<size_t>(Op::Count)>& by_kind) {
    by_kind = {};
    std::map<std::pair<u8, OpLabel>, OpTotal> taken;
    {
        std::scoped_lock ops_lock{g_ops_mutex};
        taken.swap(g_ops_totals);
        unowned_ms = static_cast<double>(g_ops_unowned_ns) / 1e6;
        g_ops_unowned_ns = 0;
        overflow = g_ops_overflow;
        g_ops_overflow = 0;
    }
    std::vector<std::pair<std::pair<u8, OpLabel>, OpTotal>> ranked{taken.begin(), taken.end()};
    std::ranges::sort(ranked, [](const auto& lhs, const auto& rhs) {
        return lhs.second.ms > rhs.second.ms;
    });
    measured_ms = 0.0;
    std::vector<TopOp> top;
    top.reserve(std::min<size_t>(limit, ranked.size()));
    for (const auto& [key, total] : ranked) {
        measured_ms += total.ms;
        const auto op = static_cast<Op>(key.first);
        const size_t kind_index = static_cast<size_t>(op);
        if (kind_index < by_kind.size()) {
            by_kind[kind_index].ms += total.ms;
            by_kind[kind_index].count += total.count;
        }
        if (top.size() >= limit) {
            continue;
        }
        top.push_back(TopOp{.kind = OpKindName(op),
                            .label = OpLabelText(op, key.second),
                            .ms = total.ms,
                            .count = total.count});
    }
    return top;
}

} // namespace Vulkan
