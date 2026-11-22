// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/rhi_entry_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "common/arch.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/sha256.h"
#include "core/cpu_patches.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/loader/elf.h"
#include "core/memory.h"
#include "core/module.h"
#include "core/rhi_entry_probe_contract.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__FreeBSD__)
#include <pthread_np.h>
#endif

namespace Core::RhiEntryProbe {
namespace {

constexpr size_t MaxCapturedEvents = 128;
constexpr size_t MaxStackArguments = 8;

struct ProbeEvent {
    u64 sequence{};
    u64 native_thread_id{};
    VAddr module_base{};
    VAddr target{};
    VAddr original_rsp{};
    u64 caller_return_address{};
    std::array<u64, 6> integer_arguments{};
    std::array<u64, 32> ymm_arguments{};
    std::array<u64, MaxStackArguments> stack_arguments{};
    u8 stack_argument_count{};
    bool stack_arguments_validated{};
};

struct ProbeState {
    MemoryManager* memory{};
    VAddr module_base{};
    VAddr target{};
    GuestEntryObserverHandle handle{};
    std::atomic<u32> captured_count{};
    std::atomic<u32> captured_gnm_count{};
    std::atomic<u32> captured_pm4_count{};
    std::atomic<bool> limit_logged{};
    /// Absolute time before which entries are ignored. Zero means observe immediately.
    std::atomic<u64> start_after_ns{};
    std::array<ProbeEvent, MaxCapturedEvents> events{};
};

struct PendingGnmCorrelation {
    u64 rhi_sequence{};
    u64 native_thread_id{};
    VAddr module_base{};
    VAddr target{};
    u64 caller_return_address{};
    std::chrono::steady_clock::time_point captured_at{};
};

/// One guest draw followed from the deferred command wrapper to the Vulkan draw.
///
/// The identity is a monotonic record id assigned when the RHI entry is observed. It travels in
/// thread-local state to the GNM call, then into a packet record keyed by the command buffer the
/// packet was written into, and finally to the rasterizer. Nothing here is derived from an
/// independent ordinal on the consumer side.
struct ProvenanceRecord {
    u64 id{};
    u64 rhi_sequence{};
    u64 native_thread_id{};
    u64 guest_thread_id{};
    /// Guest command-buffer pointer the packet was constructed into.
    VAddr cmdbuf{};
    /// Byte offset of the packet within the submitted DCB, once known.
    u64 packet_offset{};
    /// Submission generation, assigned when the DCB containing the packet is submitted.
    u64 submission_id{};
    /// Copied-buffer address, when the submission path copied the DCB.
    VAddr copied_dcb{};
    u32 index_count{};
    u32 instance_count{};
    u64 index_addr{};
    u32 primitive_type{};
    u32 rhi_index_resource_lo{};
    u32 rhi_index_resource_hi{};
    std::chrono::steady_clock::time_point captured_at{};
    /// Set once packet construction succeeded.
    bool packet_constructed{};
    /// Set once the packet was decoded on the GPU thread.
    bool packet_decoded{};
    /// Set once the rasterizer recorded the draw.
    bool draw_recorded{};
    /// Set when the draw was decoded but produced no rasterizer draw.
    bool draw_skipped{};
};

/// A packet registered at construction time, waiting to be matched at decode time.
struct PendingPacket {
    u64 record_id{};
    VAddr cmdbuf{};
    u32 index_count{};
    u64 index_addr{};
    u64 submission_id{};
    std::chrono::steady_clock::time_point registered_at{};
};

struct ProvenanceState {
    std::atomic<bool> enabled{};
    std::atomic<u32> next_record_id{};
    std::atomic<u32> record_count{};
    std::atomic<u32> packet_count{};
    std::atomic<u32> decoded_count{};
    std::atomic<u32> recorded_count{};
    std::atomic<u32> skipped_count{};
    std::atomic<u32> expired_count{};
    std::atomic<u32> unmatched_packet_count{};
    std::atomic<u64> next_submission_id{};
    std::mutex mutex;
    std::array<ProvenanceRecord, MaxProvenanceRecords> records{};
    std::array<PendingPacket, MaxProvenanceRecords> packets{};
};

ProvenanceState g_provenance{};
thread_local u64 g_pending_provenance_id{};
/// Set on the GPU thread while a decoded packet's draw is being processed, so the rasterizer can
/// report the actual pipeline state against the right record.
thread_local u64 g_current_draw_provenance_id{};

[[nodiscard]] u64 CurrentGuestThreadId() noexcept {
    const auto* thread = Libraries::Kernel::g_curthread;
    if (thread == nullptr)
        return 0;
    return static_cast<u64>(thread->tid.load(std::memory_order_relaxed));
}

ProbeState g_probe_state{};
thread_local PendingGnmCorrelation g_pending_gnm_correlation{};

[[nodiscard]] std::optional<std::string> HashFile(const std::filesystem::path& path) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen())
        return std::nullopt;

    const u64 file_size = file.GetSize();
    sha256::SHA256 hasher;
    std::array<u8, 64 * 1024> buffer{};
    u64 remaining = file_size;
    while (remaining != 0) {
        const size_t request = static_cast<size_t>((std::min<u64>)(remaining, buffer.size()));
        const size_t read = file.ReadRaw<u8>(buffer.data(), request);
        if (read != request)
            return std::nullopt;
        hasher.update(buffer.data(), read);
        remaining -= read;
    }
    return sha256::SHA256::Hex(hasher.final());
}

[[nodiscard]] u64 CurrentNativeThreadId() noexcept {
#ifdef _WIN32
    return static_cast<u64>(GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<u64>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
    u64 thread_id{};
    return pthread_threadid_np(nullptr, &thread_id) == 0 ? thread_id : 0;
#elif defined(__FreeBSD__)
    return static_cast<u64>(pthread_getthreadid_np());
#else
    return 0;
#endif
}

[[nodiscard]] bool ReserveEventSlot(ProbeState& state, u32& index) noexcept {
    u32 observed = state.captured_count.load(std::memory_order_relaxed);
    while (observed < MaxCapturedEvents) {
        if (state.captured_count.compare_exchange_weak(
                observed, observed + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            index = observed;
            return true;
        }
    }
    if (!state.limit_logged.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO(Core_Linker, "RHI entry probe reached its {}-event capture limit",
                 MaxCapturedEvents);
    }
    return false;
}

[[nodiscard]] bool CaptureValidatedStack(ProbeState& state, VAddr rsp, ProbeEvent& event) noexcept {
    if (state.memory == nullptr || rsp == 0 || rsp > (std::numeric_limits<VAddr>::max)() - 8) {
        return false;
    }

    void* mapped_start{};
    void* mapped_end{};
    u32 protection{};
    if (state.memory->QueryProtection(rsp, &mapped_start, &mapped_end, &protection) != 0 ||
        (protection & static_cast<u32>(MemoryProt::CpuRead)) == 0) {
        return false;
    }

    void* stack_start_ptr{};
    void* stack_end_ptr{};
    if (state.memory->IsStack(rsp, &stack_start_ptr, &stack_end_ptr) != 0)
        return false;
    const VAddr stack_start = reinterpret_cast<VAddr>(stack_start_ptr);
    const VAddr stack_end = reinterpret_cast<VAddr>(stack_end_ptr);
    const VAddr region_start = reinterpret_cast<VAddr>(mapped_start);
    const VAddr region_end = reinterpret_cast<VAddr>(mapped_end);
    if (stack_start == 0 || stack_end <= stack_start || rsp < stack_start || rsp >= stack_end ||
        region_start > rsp || region_end < rsp || stack_end - rsp < sizeof(u64)) {
        return false;
    }

    // Prior A2 static mapping: stack_args[0]=StartIndex ([RBP+0x10]),
    // stack_args[1]=NumPrimitives ([RBP+0x18]), stack_args[2]=NumInstances ([RBP+0x20]).
    // The prologue sets RBP=entry_rsp-8, so these follow the return address at [entry_rsp].
    // Keep the values raw; this does not establish the full C++ prototype.
    const u64 words_available = (stack_end - rsp) / sizeof(u64);
    const size_t argument_count = static_cast<size_t>(
        (std::min<u64>)(MaxStackArguments, words_available > 0 ? words_available - 1 : 0));
    const size_t word_count = argument_count + 1; // return address followed by stack arguments
    if (word_count > (std::numeric_limits<VAddr>::max)() / sizeof(u64))
        return false;
    const VAddr read_size = static_cast<VAddr>(word_count * sizeof(u64));
    if (rsp > (std::numeric_limits<VAddr>::max)() - read_size || region_end < rsp + read_size ||
        stack_end < rsp + read_size) {
        return false;
    }

    std::memcpy(&event.caller_return_address, reinterpret_cast<const void*>(rsp), sizeof(u64));
    if (argument_count != 0) {
        std::memcpy(event.stack_arguments.data(), reinterpret_cast<const void*>(rsp + sizeof(u64)),
                    argument_count * sizeof(u64));
    }
    event.stack_argument_count = static_cast<u8>(argument_count);
    event.stack_arguments_validated = true;
    return true;
}

static void CaptureEntryImpl(const GuestEntryRegisterFrame* frame, void* user_data) {
    auto* state = static_cast<ProbeState*>(user_data);
    if (frame == nullptr || state == nullptr)
        return;

    // A bounded start delay lets the probe observe the same interval as the command capture.
    const u64 start_after = state->start_after_ns.load(std::memory_order_relaxed);
    if (start_after != 0) {
        const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
        if (now < start_after)
            return;
    }

    u32 index{};
    if (!ReserveEventSlot(*state, index))
        return;

    ProbeEvent event{};
    event.sequence = static_cast<u64>(index) + 1;
    event.native_thread_id = CurrentNativeThreadId();
    event.module_base = state->module_base;
    event.target = state->target;
    event.original_rsp = frame->OriginalStackPointer();
    event.integer_arguments = {frame->rdi, frame->rsi, frame->rdx,
                               frame->rcx, frame->r8,  frame->r9};
    for (size_t register_index = 0; register_index < 8; ++register_index) {
        for (size_t word_index = 0; word_index < 4; ++word_index) {
            std::memcpy(&event.ymm_arguments[register_index * 4 + word_index],
                        frame->ymm[register_index].data() + word_index * sizeof(u64), sizeof(u64));
        }
    }
    (void)CaptureValidatedStack(*state, event.original_rsp, event);
    state->events[index] = event;
    g_pending_gnm_correlation = {
        event.sequence, event.native_thread_id,      event.module_base,
        event.target,   event.caller_return_address, std::chrono::steady_clock::now()};

    // Start a provenance record for this draw. The id travels in thread-local state to the GNM
    // call, so the association is by execution identity rather than by an independent ordinal.
    g_pending_provenance_id = 0;
    if (g_provenance.enabled.load(std::memory_order_relaxed)) {
        const u32 slot = g_provenance.record_count.fetch_add(1, std::memory_order_relaxed);
        if (slot < MaxProvenanceRecords) {
            const u64 id = static_cast<u64>(slot) + 1;
            ProvenanceRecord record{};
            record.id = id;
            record.rhi_sequence = event.sequence;
            record.native_thread_id = event.native_thread_id;
            record.guest_thread_id = CurrentGuestThreadId();
            record.captured_at = std::chrono::steady_clock::now();
            // The index-buffer resource is the entry's second argument.
            record.rhi_index_resource_lo = static_cast<u32>(event.integer_arguments[1]);
            record.rhi_index_resource_hi = static_cast<u32>(event.integer_arguments[1] >> 32);
            {
                std::scoped_lock lock{g_provenance.mutex};
                g_provenance.records[slot] = record;
            }
            g_pending_provenance_id = id;
        }
    }

    const auto& stack = event.stack_arguments;
    LOG_INFO(Core_Linker,
             "DERACINE_RHI_PROBE_EVENT sequence={} thread_id={} module_base={:#x} "
             "target={:#x} caller={:#x} entry_rsp={:#x} "
             "gpr_rdi={:#x} gpr_rsi={:#x} gpr_rdx={:#x} gpr_rcx={:#x} "
             "gpr_r8={:#x} gpr_r9={:#x} "
             "ymm_args=[{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},"
             "{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},"
             "{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},"
             "{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}] "
             "stack_validated={} stack_count={} "
             "stack_args=[{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}]",
             event.sequence, event.native_thread_id, event.module_base, event.target,
             event.caller_return_address, event.original_rsp, event.integer_arguments[0],
             event.integer_arguments[1], event.integer_arguments[2], event.integer_arguments[3],
             event.integer_arguments[4], event.integer_arguments[5], event.ymm_arguments[0],
             event.ymm_arguments[1], event.ymm_arguments[2], event.ymm_arguments[3],
             event.ymm_arguments[4], event.ymm_arguments[5], event.ymm_arguments[6],
             event.ymm_arguments[7], event.ymm_arguments[8], event.ymm_arguments[9],
             event.ymm_arguments[10], event.ymm_arguments[11], event.ymm_arguments[12],
             event.ymm_arguments[13], event.ymm_arguments[14], event.ymm_arguments[15],
             event.ymm_arguments[16], event.ymm_arguments[17], event.ymm_arguments[18],
             event.ymm_arguments[19], event.ymm_arguments[20], event.ymm_arguments[21],
             event.ymm_arguments[22], event.ymm_arguments[23], event.ymm_arguments[24],
             event.ymm_arguments[25], event.ymm_arguments[26], event.ymm_arguments[27],
             event.ymm_arguments[28], event.ymm_arguments[29], event.ymm_arguments[30],
             event.ymm_arguments[31], event.stack_arguments_validated ? 1 : 0,
             static_cast<u32>(event.stack_argument_count), stack[0], stack[1], stack[2], stack[3],
             stack[4], stack[5], stack[6], stack[7]);
}

static PS4_SYSV_ABI void CaptureEntry(const GuestEntryRegisterFrame* frame,
                                      void* user_data) noexcept {
    try {
        CaptureEntryImpl(frame, user_data);
    } catch (...) {
        // Exceptions must never unwind through generated guest trampoline code.
    }
}

[[nodiscard]] bool FindExecutableTargetSegment(const Module& module, u64& segment_rva,
                                               u64& segment_file_size, u64& segment_memory_size) {
    for (const auto& header : module.elf.GetProgramHeader()) {
        if (header.p_type != ::PT_LOAD || (header.p_flags & ::PF_EXEC) == 0) {
            continue;
        }
        if (IsExecutableLoadRange(TargetRva, TargetPrologue.size(), header.p_vaddr, header.p_filesz,
                                  header.p_memsz)) {
            segment_rva = header.p_vaddr;
            segment_file_size = header.p_filesz;
            segment_memory_size = header.p_memsz;
            return true;
        }
    }
    return false;
}

} // namespace

void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version) {
    if (!IsOptedIn(std::getenv(EnableEnvironmentVariable.data())))
        return;

#if !defined(ARCH_X86_64) || defined(SHADPS4_ENABLE_FEX_GUEST_CPU)
    LOG_WARNING(Core_Linker,
                "RHI entry probe refused: this build uses FEX/translated or non-native execution");
    return;
#else
    if (module.memory == nullptr || module.name != ModuleName ||
        module.file.filename().string() != ModuleName || title_id != TitleId) {
        LOG_WARNING(Core_Linker,
                    "RHI entry probe refused: expected native CUSA11919 eboot.bin, got title='{}' "
                    "module='{}' path='{}'",
                    title_id, module.name, module.file.string());
        return;
    }

    const auto image_hash = HashFile(module.file);
    if (!image_hash) {
        LOG_WARNING(Core_Linker, "RHI entry probe refused: unable to hash selected image '{}'",
                    module.file.string());
        return;
    }
    if (!IsTargetBuild(module.name, title_id, *image_hash)) {
        LOG_WARNING(Core_Linker,
                    "RHI entry probe refused: unapproved CUSA11919 eboot.bin SHA-256 {}",
                    *image_hash);
        return;
    }

    u64 segment_rva{};
    u64 segment_file_size{};
    u64 segment_memory_size{};
    if (!FindExecutableTargetSegment(module, segment_rva, segment_file_size, segment_memory_size)) {
        LOG_WARNING(Core_Linker,
                    "RHI entry probe refused: RVA {:#x} is not in a file-backed executable "
                    "PT_LOAD segment",
                    TargetRva);
        return;
    }
    if (module.base_virtual_addr == 0 ||
        module.base_virtual_addr > (std::numeric_limits<VAddr>::max)() - TargetRva) {
        LOG_WARNING(Core_Linker, "RHI entry probe refused: invalid module base {:#x}",
                    module.base_virtual_addr);
        return;
    }
    const VAddr target = module.base_virtual_addr + TargetRva;
    if (target > (std::numeric_limits<VAddr>::max)() - TargetPrologue.size()) {
        LOG_WARNING(Core_Linker, "RHI entry probe refused: target range overflows {:#x}", target);
        return;
    }

    void* mapping_start{};
    void* mapping_end{};
    u32 protection{};
    if (module.memory->QueryProtection(target, &mapping_start, &mapping_end, &protection) != 0 ||
        reinterpret_cast<VAddr>(mapping_start) > target ||
        reinterpret_cast<VAddr>(mapping_end) < target + TargetPrologue.size() ||
        (protection & static_cast<u32>(MemoryProt::CpuRead)) == 0 ||
        (protection & static_cast<u32>(MemoryProt::CpuExec)) == 0) {
        LOG_WARNING(Core_Linker,
                    "RHI entry probe refused: target {:#x} is not in a readable executable VMA",
                    target);
        return;
    }
    const auto target_bytes =
        std::span<const u8>{reinterpret_cast<const u8*>(target), TargetPrologue.size()};
    if (!MatchesTargetPrologue(target_bytes)) {
        LOG_WARNING(Core_Linker,
                    "RHI entry probe refused: target prologue signature mismatch at {:#x}", target);
        return;
    }

    g_probe_state.memory = module.memory;
    g_probe_state.module_base = module.base_virtual_addr;
    g_probe_state.target = target;

    // Optional bounded start delay, so the probe can observe the same interval as the capture.
    u32 start_delay_seconds{};
    if (const char* delay = std::getenv(StartDelayEnvironmentVariable.data());
        delay != nullptr && *delay != '\0') {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(delay, &end, 10);
        if (end == delay || *end != '\0' || parsed > MaxStartDelaySeconds) {
            LOG_WARNING(Core_Linker,
                        "RHI entry probe refused: {} must be a whole number of seconds up to {}",
                        StartDelayEnvironmentVariable, MaxStartDelaySeconds);
            return;
        }
        start_delay_seconds = static_cast<u32>(parsed);
    }
    if (start_delay_seconds != 0) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{start_delay_seconds};
        g_probe_state.start_after_ns.store(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 deadline.time_since_epoch())
                                 .count()),
            std::memory_order_relaxed);
    }

    if (!InstallGuestEntryObserver(*module.memory, target, CaptureEntry, &g_probe_state,
                                   g_probe_state.handle)) {
        LOG_ERROR(Core_Linker, "RHI entry probe failed to install at {:#x}", target);
        return;
    }

    // The end-to-end provenance trace is a separate opt-in, so the existing bounded observation
    // keeps its current behaviour and cost.
    if (const char* provenance = std::getenv(ProvenanceEnvironmentVariable.data());
        provenance != nullptr && std::string_view{provenance} == "1") {
        g_provenance.enabled.store(true, std::memory_order_relaxed);
        LOG_INFO(Core_Linker,
                 "DERACINE_DRAW_PROVENANCE_INSTALL record_limit={} max_age_us={}",
                 MaxProvenanceRecords, MaxProvenanceAgeUs);
    }
    LOG_INFO(Core_Linker,
             "DERACINE_RHI_PROBE_INSTALL title_id={} app_version={} image_sha256={} "
             "module_name={} module_base={:#x} target_rva={:#x} target={:#x} "
             "exec_pt_load_rva={:#x} exec_pt_load_filesz={:#x} exec_pt_load_memsz={:#x} "
             "event_limit={} start_delay_s={}",
             title_id, app_version, *image_hash, ModuleName, module.base_virtual_addr, TargetRva,
             target, segment_rva, segment_file_size, segment_memory_size, MaxCapturedEvents,
             start_delay_seconds);
#endif
}

void CaptureGnmDrawIndex(std::uintptr_t cmdbuf, std::uint32_t size, std::uint32_t index_count,
                         std::uintptr_t index_addr, std::uint32_t flags,
                         std::uint32_t type) noexcept {
    try {
        if (!g_probe_state.handle.active)
            return;

        const PendingGnmCorrelation pending = g_pending_gnm_correlation;
        g_pending_gnm_correlation = {};
        if (pending.rhi_sequence == 0)
            return;

        const u64 native_thread_id = CurrentNativeThreadId();
        const auto elapsed = std::chrono::steady_clock::now() - pending.captured_at;
        if (elapsed < std::chrono::steady_clock::duration::zero())
            return;
        const u64 delta_us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        if (!IsRhiToGnmCorrelationValid(pending.rhi_sequence, pending.native_thread_id,
                                        native_thread_id, delta_us)) {
            return;
        }

        if (g_probe_state.captured_gnm_count.load(std::memory_order_relaxed) >= MaxCapturedEvents)
            return;
        const u32 index = g_probe_state.captured_gnm_count.fetch_add(1, std::memory_order_relaxed);
        if (index >= MaxCapturedEvents)
            return;

        // Record the GNM arguments against the provenance record. The id is kept in thread-local
        // state so the packet registration that follows successful construction can still claim
        // it; it is cleared there, not here.
        const u64 provenance_id = g_pending_provenance_id;
        if (provenance_id != 0 && provenance_id <= MaxProvenanceRecords) {
            std::scoped_lock lock{g_provenance.mutex};
            auto& record = g_provenance.records[provenance_id - 1];
            if (record.id == provenance_id) {
                record.cmdbuf = cmdbuf;
                record.index_count = index_count;
                record.index_addr = index_addr;
                record.primitive_type = type;
            }
        }

        LOG_INFO(Core_Linker,
                 "DERACINE_GNM_DRAW_INDEX gnm_sequence={} rhi_sequence={} provenance_id={} "
                 "thread_id={} delta_us={} rhi_module_base={:#x} rhi_target={:#x} "
                 "rhi_caller={:#x} cmdbuf={:#x} size={} index_count={} index_addr={:#x} "
                 "flags={:#x} type={:#x}",
                 static_cast<u64>(index) + 1, pending.rhi_sequence, provenance_id, native_thread_id,
                 delta_us, pending.module_base, pending.target, pending.caller_return_address,
                 cmdbuf, size, index_count, index_addr, flags, type);
    } catch (...) {
        // GNM observation must not affect packet construction or unwind into guest execution.
    }
}

void CapturePm4DrawIndex2(std::uint32_t max_size, std::uint32_t index_base_lo,
                          std::uint32_t index_base_hi, std::uint32_t index_count,
                          std::uint32_t draw_initiator) noexcept {
    try {
        if (!g_probe_state.handle.active)
            return;

        // The GPU thread decodes packets submitted from boot, so it needs the same start gate as
        // the RHI entries; otherwise the two layers observe different intervals.
        const u64 start_after = g_probe_state.start_after_ns.load(std::memory_order_relaxed);
        if (start_after != 0) {
            const auto now = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (now < start_after)
                return;
        }

        if (g_probe_state.captured_pm4_count.load(std::memory_order_relaxed) >= MaxCapturedEvents)
            return;
        const u32 index = g_probe_state.captured_pm4_count.fetch_add(1, std::memory_order_relaxed);
        if (index >= MaxCapturedEvents)
            return;

        // The packet is decoded on the GPU thread, so this record carries no RHI sequence. Pair it
        // offline by the exact index_count and index address the GNM call supplied.
        LOG_INFO(Core_Linker,
                 "DERACINE_PM4_DRAW_INDEX2 pm4_sequence={} thread_id={} max_size={} "
                 "index_base={:#x} index_count={} draw_initiator={:#x}",
                 static_cast<u64>(index) + 1, CurrentNativeThreadId(), max_size,
                 (static_cast<u64>(index_base_hi) << 32) | index_base_lo, index_count,
                 draw_initiator);
    } catch (...) {
        // PM4 observation must not affect packet decoding or unwind into the GPU thread.
    }
}

void RegisterConstructedPacket(std::uintptr_t cmdbuf, std::uint32_t index_count,
                               std::uintptr_t index_addr) noexcept {
    try {
        if (!g_provenance.enabled.load(std::memory_order_relaxed))
            return;
        if (cmdbuf == 0)
            return;

        const u64 record_id = g_pending_provenance_id;
        // The id is consumed here: one RHI entry produces at most one packet, and a second GNM
        // call on the same thread must not inherit it.
        g_pending_provenance_id = 0;
        if (record_id == 0)
            return;

        const u32 slot = g_provenance.packet_count.fetch_add(1, std::memory_order_relaxed);
        if (slot >= MaxProvenanceRecords)
            return;

        PendingPacket packet{};
        packet.record_id = record_id;
        packet.cmdbuf = cmdbuf;
        packet.index_count = index_count;
        packet.index_addr = index_addr;
        packet.registered_at = std::chrono::steady_clock::now();

        std::scoped_lock lock{g_provenance.mutex};
        g_provenance.packets[slot] = packet;
        if (record_id <= MaxProvenanceRecords) {
            auto& record = g_provenance.records[record_id - 1];
            if (record.id == record_id) {
                record.packet_constructed = true;
            }
        }
    } catch (...) {
        // Provenance must never affect packet construction.
    }
}

void NoteSubmission(const void* dcb_data, std::size_t dcb_words, std::uintptr_t original_dcb,
                    bool copied) noexcept {
    try {
        if (!g_provenance.enabled.load(std::memory_order_relaxed))
            return;
        if (dcb_data == nullptr || dcb_words == 0)
            return;

        const u64 submission_id = g_provenance.next_submission_id.fetch_add(1) + 1;
        const auto dcb_base = reinterpret_cast<std::uintptr_t>(dcb_data);
        const auto dcb_end = dcb_base + dcb_words * sizeof(u32);

        std::scoped_lock lock{g_provenance.mutex};
        for (auto& packet : g_provenance.packets) {
            if (packet.record_id == 0 || packet.submission_id != 0)
                continue;
            // A packet belongs to this submission when its guest buffer lies inside the submitted
            // span. With the copy path the guest buffer is not inside the copied span, so the
            // original pointer is matched instead and the copy is recorded explicitly.
            const bool inside_copied = packet.cmdbuf >= dcb_base && packet.cmdbuf < dcb_end;
            const bool inside_original =
                copied && original_dcb != 0 && packet.cmdbuf >= original_dcb &&
                packet.cmdbuf < original_dcb + dcb_words * sizeof(u32);
            if (!inside_copied && !inside_original)
                continue;

            packet.submission_id = submission_id;
            if (packet.record_id <= MaxProvenanceRecords) {
                auto& record = g_provenance.records[packet.record_id - 1];
                if (record.id == packet.record_id) {
                    record.submission_id = submission_id;
                    record.copied_dcb = copied ? dcb_base : 0;
                    record.packet_offset = packet.cmdbuf - (copied ? original_dcb : dcb_base);
                }
            }
        }
    } catch (...) {
        // Provenance must never affect submission.
    }
}

void CapturePm4DrawIndex2At(std::uint32_t max_size, std::uint32_t index_base_lo,
                            std::uint32_t index_base_hi, std::uint32_t index_count,
                            std::uint32_t draw_initiator, std::uintptr_t packet_address,
                            std::uintptr_t dcb_base) noexcept {
    try {
        if (!g_provenance.enabled.load(std::memory_order_relaxed))
            return;

        const u64 index_addr = (static_cast<u64>(index_base_hi) << 32) | index_base_lo;
        const u64 offset = packet_address >= dcb_base ? packet_address - dcb_base : 0;

        u64 matched_id = 0;
        {
            std::scoped_lock lock{g_provenance.mutex};
            for (auto& packet : g_provenance.packets) {
                if (packet.record_id == 0)
                    continue;
                if (packet.index_count != index_count || packet.index_addr != index_addr)
                    continue;
                // Prefer a packet whose recorded offset matches the decoded position; fall back to
                // the parameter match only when the offset is unknown.
                const auto& record = packet.record_id <= MaxProvenanceRecords
                                         ? g_provenance.records[packet.record_id - 1]
                                         : g_provenance.records[0];
                const bool offset_known = record.id == packet.record_id && record.packet_offset != 0;
                if (offset_known && record.packet_offset != offset)
                    continue;
                matched_id = packet.record_id;
                if (record.id == packet.record_id) {
                    g_provenance.records[packet.record_id - 1].packet_decoded = true;
                }
                break;
            }
        }

        if (matched_id == 0) {
            g_provenance.unmatched_packet_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_provenance.decoded_count.fetch_add(1, std::memory_order_relaxed);
        }
        // The rasterizer runs later on this same thread for this packet, so carry the identity.
        g_current_draw_provenance_id = matched_id;

        // Only matched packets are logged. Unmatched ones are counted, because the GPU thread
        // decodes far more packets than the bounded record set covers and logging each one would
        // exhaust the log budget before the summary is written.
        if (matched_id != 0) {
            LOG_INFO(Core_Linker,
                     "DERACINE_PM4_PROVENANCE provenance_id={} packet_offset={:#x} "
                     "index_base={:#x} index_count={} draw_initiator={:#x}",
                     matched_id, offset, index_addr, index_count, draw_initiator);
        }
    } catch (...) {
        // Provenance must never affect packet decoding.
    }
}

void NoteDrawRecorded(std::uint64_t record_id, std::uint64_t vs_hash, std::uint64_t ps_hash,
                      std::uint64_t pipeline_hash, std::uint32_t index_count,
                      std::uint32_t instance_count, std::uint32_t primitive_type,
                      std::uint64_t index_addr, std::uint32_t color_mask,
                      std::uint64_t depth_address, bool depth_test, bool depth_write) noexcept {
    try {
        if (!g_provenance.enabled.load(std::memory_order_relaxed) || record_id == 0)
            return;
        if (record_id > MaxProvenanceRecords)
            return;

        std::scoped_lock lock{g_provenance.mutex};
        auto& record = g_provenance.records[record_id - 1];
        if (record.id != record_id)
            return;
        record.draw_recorded = true;
        g_provenance.recorded_count.fetch_add(1, std::memory_order_relaxed);

        LOG_INFO(Core_Linker,
                 "DERACINE_DRAW_PROVENANCE provenance_id={} vs={:#x} ps={:#x} pipeline={:#x} "
                 "index_count={} instances={} prim={} index_addr={:#x} mask={:#x} db={:#x} "
                 "z_en={} z_wr={}",
                 record_id, vs_hash, ps_hash, pipeline_hash, index_count, instance_count,
                 primitive_type, index_addr, color_mask, depth_address, depth_test, depth_write);
    } catch (...) {
        // Provenance must never affect draw processing.
    }
}

void NoteDrawSkipped(std::uint64_t record_id, const char* reason) noexcept {
    try {
        if (!g_provenance.enabled.load(std::memory_order_relaxed) || record_id == 0)
            return;
        if (record_id > MaxProvenanceRecords)
            return;

        std::scoped_lock lock{g_provenance.mutex};
        auto& record = g_provenance.records[record_id - 1];
        if (record.id != record_id)
            return;
        record.draw_skipped = true;
        g_provenance.skipped_count.fetch_add(1, std::memory_order_relaxed);

        LOG_INFO(Core_Linker, "DERACINE_DRAW_SKIPPED provenance_id={} reason={}", record_id,
                 reason != nullptr ? reason : "unknown");
    } catch (...) {
        // Provenance must never affect draw processing.
    }
}

std::uint64_t CurrentDrawProvenanceId() noexcept {
    return g_current_draw_provenance_id;
}

void ExportProvenance() noexcept {
    try {
        if (!g_provenance.enabled.load(std::memory_order_relaxed))
            return;

        const u32 records = g_provenance.record_count.load(std::memory_order_relaxed);
        const u32 packets = g_provenance.packet_count.load(std::memory_order_relaxed);
        const u32 decoded = g_provenance.decoded_count.load(std::memory_order_relaxed);
        const u32 recorded = g_provenance.recorded_count.load(std::memory_order_relaxed);
        const u32 skipped = g_provenance.skipped_count.load(std::memory_order_relaxed);
        const u32 unmatched = g_provenance.unmatched_packet_count.load(std::memory_order_relaxed);

        u32 constructed = 0;
        u32 submitted = 0;
        u32 pending = 0;
        u32 expired = 0;
        {
            std::scoped_lock lock{g_provenance.mutex};
            const auto now = std::chrono::steady_clock::now();
            for (u32 i = 0; i < MaxProvenanceRecords && i < records; ++i) {
                const auto& record = g_provenance.records[i];
                if (record.id == 0)
                    continue;
                if (record.packet_constructed)
                    ++constructed;
                if (record.submission_id != 0)
                    ++submitted;
                if (!record.packet_decoded) {
                    const auto age = std::chrono::duration_cast<std::chrono::microseconds>(
                                         now - record.captured_at)
                                         .count();
                    if (age > static_cast<std::int64_t>(MaxProvenanceAgeUs))
                        ++expired;
                    else
                        ++pending;
                }
            }
        }

        LOG_INFO(Core_Linker,
                 "DERACINE_DRAW_PROVENANCE_SUMMARY records={} packets={} constructed={} "
                 "submitted={} decoded={} recorded={} skipped={} pending={} expired={} "
                 "unmatched_packets={} limit={}",
                 records, packets, constructed, submitted, decoded, recorded, skipped, pending,
                 expired, unmatched, MaxProvenanceRecords);

        std::scoped_lock lock{g_provenance.mutex};
        for (u32 i = 0; i < MaxProvenanceRecords && i < records; ++i) {
            const auto& record = g_provenance.records[i];
            if (record.id == 0)
                continue;
            LOG_INFO(Core_Linker,
                     "DERACINE_DRAW_PROVENANCE_RECORD id={} rhi_sequence={} thread={} "
                     "guest_thread={} cmdbuf={:#x} packet_offset={:#x} submission={} "
                     "copied_dcb={:#x} index_count={} index_addr={:#x} prim={} "
                     "rhi_index_resource={:#x} constructed={} decoded={} recorded={} skipped={}",
                     record.id, record.rhi_sequence, record.native_thread_id,
                     record.guest_thread_id, record.cmdbuf, record.packet_offset,
                     record.submission_id, record.copied_dcb, record.index_count,
                     record.index_addr, record.primitive_type,
                     (static_cast<u64>(record.rhi_index_resource_hi) << 32) |
                         record.rhi_index_resource_lo,
                     record.packet_constructed, record.packet_decoded, record.draw_recorded,
                     record.draw_skipped);
        }
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

} // namespace Core::RhiEntryProbe
