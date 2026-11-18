// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <compare>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_gpu_timing_math.h"

namespace Vulkan {

class Instance;
class MasterSemaphore;

/// SHADPS4_GPU_TIMING=1: a timestamp pair around every submission a scheduler makes, so the log
/// can say what the GPU spent executing it, what the gap was between two of that scheduler's
/// submissions, and how both compare with the command thread's own time on the frame.
///
/// What it is for. The frame statistics already know how busy the GPU looked (FrameStats::GpuLook
/// asks whether the title's work is done yet), how long the command thread took to translate a
/// frame, and how long the driver's calls took. They do not know how much of a frame the GPU
/// itself executed, nor whether the title and the GPU are taking turns. "The GPU is the limit"
/// and "we are waiting for the GPU" call for opposite work, and until this existed nothing in the
/// log separated them.
///
/// How an interval is tied to a submission. Its timestamps are recorded into that submission's own
/// command buffer - top of pipe where the submission begins, bottom of pipe where it ends - so the
/// interval belongs to exactly the submission whose command buffer holds it and not to whatever is
/// recorded next. The result is read only once that submission's own timeline value says the GPU
/// is through with it, which is what makes reading it free of any device wait. A submission whose
/// pair cannot be read is counted, never guessed at.
///
/// What "gap" is not. More than one scheduler feeds the one graphics queue here (the presenter has
/// its own for composition and for the flip), so their submissions interleave. The gap counted
/// here is the time between two consecutive submissions of the *same* scheduler, which is time
/// that scheduler had nothing in flight - not proof that the GPU was idle. No wait is ever
/// subtracted from an operation to pay for it; the gap is reported on its own.
///
/// Off unless the setting is on, and a normal run records nothing at all: no pool is created, no
/// timestamp is recorded, nothing is reset or read, and every command buffer is byte for byte the
/// one it would have been. On, it costs two timestamps a submission and one query read a
/// submission, both off the GPU's critical path.
class GpuTiming {
public:
    /// A Vulkan timestamp pair per submission. Slots are reused in a ring; the scheduler keeps at
    /// most MAX_IN_FLIGHT_SUBMISSIONS submissions in flight (it waits once it is that far ahead),
    /// so a slot comes round again long after the GPU has finished with it, and the ring only has to
    /// be comfortably larger than that limit - not large enough to hold a whole frame's worth of
    /// submissions. Each slot also holds the marks of the submission that wrote it.
    static constexpr u32 TimerPairs = 128;

    GpuTiming(const Instance& instance, MasterSemaphore& semaphore);
    ~GpuTiming();

    GpuTiming(const GpuTiming&) = delete;
    GpuTiming& operator=(const GpuTiming&) = delete;

    /// Whether SHADPS4_GPU_TIMING=1 asks for the measurement.
    static bool Enabled();

    /// Whether any scheduler here got a query pool, i.e. whether the device can time submissions
    /// and there is anything for the frame statistics to report.
    static bool Measuring() noexcept {
        return ready_instances.load(std::memory_order_relaxed) != 0;
    }

    /// How many schedulers are timing. Their submissions interleave on the one graphics queue,
    /// and the gap figure is summed over them, so the frame statistics say over how many.
    static u32 SchedulersTiming() noexcept {
        return ready_instances.load(std::memory_order_relaxed);
    }

    /// A submission's command buffer has just begun: open its interval.
    void OpenSubmission(vk::CommandBuffer cmdbuf);

    /// The submission is about to be handed to the driver: close its interval and remember which
    /// timeline value it will reach the GPU at. `signal_value` is that submission's own tick.
    void CloseSubmission(vk::CommandBuffer cmdbuf, u64 signal_value);

    /// Reads every submission the GPU has finished with, without waiting for any of them. Called
    /// where the scheduler already reclaims what is provably idle.
    void Collect();

    /// What the submissions measured since the last call add up to. Summed over every scheduler
    /// that is timing, which is why it is the frame statistics - and not this - that divides by a
    /// frame count.
    struct Reading {
        u64 busy_ns{};     ///< GPU execution over the submissions whose pair came back.
        u64 gap_ns{};      ///< Between two consecutive submissions of one scheduler.
        u64 submits{};     ///< Submissions whose pair came back and was counted.
        u64 dropped{};     ///< Slots reused before their result was read: counted, never guessed.
        u64 unreadable{};  ///< The driver would not give the timestamps back.
        u64 invalid{};     ///< The pair came back reversed: neither value can be trusted.
    };
    static Reading TakeReading();

    /// What an interval was, so a cost can be read against the kind of work that spent it.
    enum class Op : u8 {
        Pass,     ///< A render pass, from the moment it began.
        Dispatch, ///< A compute dispatch.
        Copy,     ///< A blit, resolve or clear the emulator recorded.
        Count,
    };

    /// The numbers a label is built from, rather than a string: the marks are written on the GPU
    /// thread for every pass and dispatch of a frame, and formatting a name there would cost more
    /// than the measurement. A pass carries its target size, its attachment configuration and both
    /// shader hashes; a dispatch its compute hash and group count; a copy its extent and bytes.
    struct OpLabel {
        u32 width{};
        u32 height{};
        u32 extra{};
        u64 vs{};
        u64 ps{};
        u8 colors{};
        bool depth{};
        auto operator<=>(const OpLabel&) const = default;
    };

    /// Marks kept for one submission. A submission carries every pass, dispatch and copy recorded
    /// into it, and a measured frame of this title carries about a hundred operations across a few
    /// submissions, so this is the room for a busy one with a wide margin. Beyond it the rest of the
    /// submission is charged to the last mark and the overflow is counted, so a submission too heavy
    /// for even this loses detail and not correctness.
    static constexpr u32 MarksPerSubmission = 256;

    /// Whether labels are being attributed: the setting is on and the device can time.
    static bool ProfilingOps();

    /// Opens an interval for `op`. Call it where the work actually begins and, for a render pass,
    /// *outside* the pass: the profiler never ends a pass to place a timestamp, so attribution
    /// stays what the renderer recorded rather than something the measurement changed.
    void Mark(vk::CommandBuffer cmdbuf, Op op, const OpLabel& label);

    /// One operation's window total, for the frame statistics.
    struct TopOp {
        std::string kind;
        std::string label;
        double ms{};
        u64 count{};
    };

    /// The costliest operations since the last call, highest first, and what no operation owned
    /// (the top of a submission up to its first mark). Both are cleared by the call, like the other
    /// per-window counters here. `overflow` counts marks that did not fit the submission they
    /// belonged to.
    static std::vector<TopOp> TakeTopOps(u32 limit, double& measured_ms, double& unowned_ms,
                                         u64& overflow);

private:
    /// A mark made in the submission now being recorded: which query holds it, and what it was.
    struct MarkRecord {
        u32 query;
        Op op;
        OpLabel label;
    };

    /// A submission waiting for the GPU to be through it, and the pair it wrote.
    struct Pending {
        u64 tick;
        u32 slot;
        std::vector<MarkRecord> marks;
    };

    const Instance& instance;
    MasterSemaphore& semaphore;
    vk::QueryPool pool{};
    /// Nanoseconds a GPU timestamp tick is worth.
    double tick_ns = 1.0;
    /// The bits of a timestamp that mean anything; the rest reads as garbage.
    u64 valid_mask = ~u64{0};

    u32 slot{};
    bool open{};
    /// One lock over everything above: a scheduler is not entered by one thread only. The
    /// presenter flushes its own work through the same scheduler the title draws with, and the
    /// fault manager drains pending operations from wherever a fault is served, so opening,
    /// closing and collecting are all serialised here rather than assumed to be one thread's.
    std::mutex mutex;
    /// This scheduler's previous submission end, and the busy/gap it has measured.
    GpuTimelineMath math;
    /// The marks made in the submission being recorded, emptied into `pending` when it is closed.
    std::vector<MarkRecord> marks;
    /// A mark that did not fit: counted, and said so in the report, rather than silently dropped.
    u64 oversized{};
    std::deque<Pending> pending;

    static inline std::atomic<u32> ready_instances{};
    static inline std::atomic<u64> busy_ns{};
    static inline std::atomic<u64> gap_ns{};
    static inline std::atomic<u64> submits{};
    static inline std::atomic<u64> dropped{};
    static inline std::atomic<u64> unreadable{};
    static inline std::atomic<u64> invalid{};
};

} // namespace Vulkan
