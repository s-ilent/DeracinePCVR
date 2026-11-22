// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/rhi_entry_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
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

        LOG_INFO(Core_Linker,
                 "DERACINE_GNM_DRAW_INDEX gnm_sequence={} rhi_sequence={} thread_id={} "
                 "delta_us={} rhi_module_base={:#x} rhi_target={:#x} rhi_caller={:#x} "
                 "cmdbuf={:#x} size={} index_count={} index_addr={:#x} flags={:#x} type={:#x}",
                 static_cast<u64>(index) + 1, pending.rhi_sequence, native_thread_id, delta_us,
                 pending.module_base, pending.target, pending.caller_return_address, cmdbuf, size,
                 index_count, index_addr, flags, type);
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

} // namespace Core::RhiEntryProbe
