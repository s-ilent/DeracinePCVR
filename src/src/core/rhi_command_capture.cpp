// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/rhi_command_capture.h"

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
#include "core/libraries/kernel/threads/pthread.h"
#include "core/loader/elf.h"
#include "core/memory.h"
#include "core/module.h"
#include "core/rhi_command_capture_contract.h"
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

namespace Core::RhiCommandCapture {
namespace {

/// One captured command. Every field is a scalar or an opaque pointer value. Nothing here is
/// dereferenced after the hook returns, because the command allocator resets after the walk.
struct CapturedCommand {
    u64 sequence{};
    u64 native_thread_id{};
    u64 guest_thread_id{};
    u64 list_pointer{};
    u64 context_pointer{};
    u64 vtable_rva{};
    u64 payload[8]{};
    u64 render_targets[MaxRenderTargets]{};
    u64 depth_target{};
    u64 uavs[MaxUavs]{};
    u64 shader_state{};
    u64 shader_resource{};
    u64 program_data{};
    u32 shader_modifier{};
    u8 command_id{};
    u8 payload_words{};
    u8 render_target_count{};
    u8 uav_count{};
    u8 shader_status{};
};

struct CaptureState {
    MemoryManager* memory{};
    VAddr module_base{};
    std::array<GuestEntryObserverHandle, CommandEntries.size()> handles{};
    std::atomic<u32> captured_count{};
    std::atomic<u32> dropped_count{};
    std::atomic<bool> limit_logged{};
    std::atomic<bool> installed{};
    /// Absolute time before which commands are ignored. Zero means capture immediately.
    std::atomic<u64> start_after_ns{};
    std::array<CapturedCommand, MaxCapturedCommands> commands{};
};

CaptureState g_capture_state{};

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

/// The guest thread id is the emulator's own Pthread record for the calling thread. It is a
/// thread-local pointer, so reading it is cheap and needs no guest memory access.
[[nodiscard]] u64 CurrentGuestThreadId() noexcept {
    const auto* thread = Libraries::Kernel::g_curthread;
    if (thread == nullptr)
        return 0;
    return static_cast<u64>(thread->tid.load(std::memory_order_relaxed));
}

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

/// Prove that a whole guest span is readable, once, so the payload copy does not re-query the
/// mapping for every word. The capture reads at most this many bytes from one command object.
inline constexpr VAddr MaxPayloadSpan = 0x130;

[[nodiscard]] bool IsGuestSpanReadable(MemoryManager& memory, VAddr address,
                                       VAddr length) noexcept {
    if (address == 0 || length == 0 || address > (std::numeric_limits<VAddr>::max)() - length)
        return false;

    void* mapped_start{};
    void* mapped_end{};
    u32 protection{};
    if (memory.QueryProtection(address, &mapped_start, &mapped_end, &protection) != 0 ||
        (protection & static_cast<u32>(MemoryProt::CpuRead)) == 0) {
        return false;
    }
    const VAddr region_start = reinterpret_cast<VAddr>(mapped_start);
    const VAddr region_end = reinterpret_cast<VAddr>(mapped_end);
    return region_start <= address && region_end >= address + length;
}

/// Read one guest word from an already-validated span. No mapping query.
[[nodiscard]] u64 ReadValidatedWord(VAddr address) noexcept {
    u64 value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(u64));
    return value;
}

/// Read one guest word, but only after proving the address is in a readable mapping. Used for the
/// pointer-chain links, which are not inside the command payload span.
[[nodiscard]] bool ReadGuestWord(MemoryManager& memory, VAddr address, u64& value) noexcept {
    if (!IsGuestSpanReadable(memory, address, sizeof(u64)))
        return false;
    value = ReadValidatedWord(address);
    return true;
}

[[nodiscard]] bool ReserveSlot(CaptureState& state, u32& index) noexcept {
    u32 observed = state.captured_count.load(std::memory_order_relaxed);
    while (observed < MaxCapturedCommands) {
        if (state.captured_count.compare_exchange_weak(observed, observed + 1,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
            index = observed;
            return true;
        }
    }
    state.dropped_count.fetch_add(1, std::memory_order_relaxed);
    if (!state.limit_logged.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO(Core_Linker, "RHI command capture reached its {}-command limit",
                 MaxCapturedCommands);
    }
    return false;
}

/// Copy the scalar payload for a known command type. The offsets are the verified payload field
/// offsets from the A5/A6/A7/A8/A9 static work. Only scalars and opaque pointer values are read.
[[nodiscard]] u8 CapturePayload(MemoryManager& memory, VAddr command, u8 command_id,
                                CapturedCommand& captured) noexcept {
    struct Field {
        u32 offset;
        u8 size;
    };
    std::array<Field, 8> fields{};
    u8 count{};

    switch (command_id) {
    case CommandDrawIndexedPrimitive:
        fields = {{{0x10, 8}, {0x18, 4}, {0x1C, 4}, {0x20, 4},
                   {0x24, 4}, {0x28, 4}, {0x2C, 4}, {0x30, 4}}};
        count = 8;
        break;
    case CommandDrawPrimitive:
        fields = {{{0x10, 4}, {0x14, 4}, {0x18, 4}, {0x1C, 4}}};
        count = 4;
        break;
    case CommandSetStreamSource:
        fields = {{{0x10, 4}, {0x18, 8}, {0x20, 4}}};
        count = 3;
        break;
    case CommandSetRenderTargets:
        fields = {{{0x10, 4}, {0x18, 8}, {0xD8, 8}, {0xE8, 4}, {0xF0, 8}}};
        count = 5;
        break;
    case CommandSetViewport:
        fields = {{{0x10, 4}, {0x14, 4}, {0x18, 4}, {0x1C, 4}, {0x20, 4}, {0x24, 4}}};
        count = 6;
        break;
    default:
        return 0;
    }

    for (u8 i = 0; i < count; ++i) {
        u64 word{};
        if (!ReadGuestWord(memory, command + fields[i].offset, word))
            return i;
        if (fields[i].size == 4)
            word &= 0xFFFFFFFFull;
        captured.payload[i] = word;
    }
    return count;
}
/// Copy the complete render-target and UAV arrays within the verified bounds. The count fields are
/// clamped to the array capacity, so a corrupt count cannot read past the payload.
void CaptureRenderTargets(MemoryManager& memory, VAddr command, CapturedCommand& captured) noexcept {
    u64 count{};
    if (!ReadGuestWord(memory, command + RenderTargetCountOffset, count))
        return;
    captured.render_target_count =
        static_cast<u8>((std::min<u64>)(count & 0xFFFFFFFFull, MaxRenderTargets));
    for (u8 i = 0; i < captured.render_target_count; ++i) {
        u64 word{};
        if (!ReadGuestWord(memory, command + RenderTargetArrayOffset + i * RenderTargetStride, word))
            break;
        captured.render_targets[i] = word;
    }
    u64 depth{};
    if (ReadGuestWord(memory, command + DepthTargetOffset, depth))
        captured.depth_target = depth;

    u64 uav_count{};
    if (!ReadGuestWord(memory, command + UavCountOffset, uav_count))
        return;
    captured.uav_count = static_cast<u8>((std::min<u64>)(uav_count & 0xFFFFFFFFull, MaxUavs));
    for (u8 i = 0; i < captured.uav_count; ++i) {
        u64 word{};
        if (!ReadGuestWord(memory, command + UavArrayOffset + i * sizeof(u64), word))
            break;
        captured.uavs[i] = word;
    }
}

/// Walk the statically supported shader-state chain and record pointer identity only. Every link
/// is validated, and a failure records which link failed rather than a silent zero.
void CaptureShaderSnapshot(MemoryManager& memory, VAddr list, CapturedCommand& captured) noexcept {
    captured.shader_status = static_cast<u8>(ShaderSnapshotStatus::NotAttempted);

    u64 context{};
    if (!ReadGuestWord(memory, list + CommandListContextOffset, context) || context == 0) {
        captured.shader_status = static_cast<u8>(ShaderSnapshotStatus::ContextUnreadable);
        return;
    }
    captured.context_pointer = context;

    u64 shader_state{};
    if (!ReadGuestWord(memory, context + ContextShaderStateOffset, shader_state)) {
        captured.shader_status = static_cast<u8>(ShaderSnapshotStatus::ShaderStateUnreadable);
        return;
    }
    if (shader_state == 0) {
        captured.shader_status = static_cast<u8>(ShaderSnapshotStatus::ShaderStateNull);
        return;
    }
    captured.shader_state = shader_state;

    u64 modifier{};
    if (ReadGuestWord(memory, shader_state + ShaderStateModifierOffset, modifier))
        captured.shader_modifier = static_cast<u32>(modifier & 0xFFFFFFFFull);

    u64 shader_resource{};
    if (!ReadGuestWord(memory, shader_state + ShaderStateResourceOffset, shader_resource)) {
        captured.shader_status = static_cast<u8>(ShaderSnapshotStatus::ShaderStateUnreadable);
        return;
    }
    bool via_alternate{};
    if (shader_resource == 0) {
        // The receiver falls back to +0x1E0 when +0x200 is null. Mirror that exactly.
        if (!ReadGuestWord(memory, shader_state + ShaderStateAlternateResourceOffset,
                            shader_resource)) {
            captured.shader_status = static_cast<u8>(ShaderSnapshotStatus::ShaderStateUnreadable);
            return;
        }
        if (shader_resource == 0) {
            captured.shader_status = static_cast<u8>(ShaderSnapshotStatus::ShaderResourceNull);
            return;
        }
        via_alternate = true;
    }
    captured.shader_resource = shader_resource;

    u64 program_data{};
    if (!ReadGuestWord(memory, shader_resource + ShaderResourceProgramDataOffset, program_data)) {
        captured.shader_status = static_cast<u8>(ShaderSnapshotStatus::ProgramDataUnreadable);
        return;
    }
    if (program_data == 0) {
        captured.shader_status = static_cast<u8>(ShaderSnapshotStatus::ProgramDataNull);
        return;
    }
    captured.program_data = program_data;
    captured.shader_status = static_cast<u8>(via_alternate
                                                ? ShaderSnapshotStatus::ValidViaAlternate
                                                : ShaderSnapshotStatus::Valid);
}

/// The deferred command currently executing on this thread.
///
/// The observer fires on entry only, so this is set at wrapper entry and consumed by the first
/// RHI invocation that runs at a deeper stack pointer. The stack check is what makes the scope
/// real: a wrapper that returns without calling the RHI method leaves a stale entry, and a later
/// unrelated RHI call at a shallower or equal stack pointer will not claim it.
struct DeferredCommandScope {
    std::uint64_t capture_id{};
    std::uint8_t command_type{};
    std::uint64_t command_list{};
    std::uint64_t rhi_context{};
    std::uint32_t ue4_primitive_type{};
    std::uint64_t index_resource{};
    /// Stack pointer at the wrapper's entry. A nested RHI call has a smaller pointer.
    std::uint64_t entry_rsp{};
};
thread_local DeferredCommandScope g_deferred_scope{};
/// Monotonic per-thread sequence for deferred draw commands, independent of the capture budget so
/// the identity exists even when the command is not recorded.
thread_local std::uint64_t g_deferred_sequence{};
/// Counts scope claims refused by the stack-depth guard, so a silent rejection is visible.
thread_local std::uint64_t g_deferred_rejected{};

static void CaptureCommandImpl(const GuestEntryRegisterFrame* frame, void* user_data) {
    auto* state = static_cast<CaptureState*>(user_data);
    if (frame == nullptr || state == nullptr || state->memory == nullptr)
        return;

    // Publish the deferred-command scope first, before any capture budget or start-delay check.
    // The identity of the command being executed is independent of whether this run is recording
    // it, and the RHI invocation it issues must be able to claim it either way.
    //
    // `frame->rdi` is the command object; its vtable is the first word of that object, so it must
    // be read from guest memory rather than taken from the register.
    {
        const VAddr command_object = frame->rdi;
        u64 vtable{};
        if (command_object != 0 && ReadGuestWord(*state->memory, command_object, vtable)) {
            const VAddr vtable_rva =
                vtable >= state->module_base ? vtable - state->module_base : 0;
            const u8 command_id = ClassifyCommandVtable(vtable_rva);
            if (command_id == CommandDrawIndexedPrimitive || command_id == CommandDrawPrimitive) {
                g_deferred_scope.capture_id = ++g_deferred_sequence;
                g_deferred_scope.command_type = command_id;
                g_deferred_scope.command_list = frame->rsi;
                g_deferred_scope.entry_rsp = frame->OriginalStackPointer();
            }
        }
    }

    // A bounded start delay lets a capture target a later scene. The clock read is cheap and only
    // happens while the delay is still pending.
    const u64 start_after = state->start_after_ns.load(std::memory_order_relaxed);
    if (start_after != 0) {
        const auto now = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (now < start_after)
            return;
    }

    // The command object is the first argument and the command list is the second.
    const VAddr command = frame->rdi;
    const VAddr list = frame->rsi;
    if (command == 0)
        return;

    u64 vtable{};
    if (!ReadGuestWord(*state->memory, command, vtable))
        return;

    const VAddr vtable_rva = vtable >= state->module_base ? vtable - state->module_base : 0;
    const u8 command_id = ClassifyCommandVtable(vtable_rva);

    u32 index{};
    if (!ReserveSlot(*state, index))
        return;

    CapturedCommand captured{};
    captured.sequence = static_cast<u64>(index) + 1;
    captured.native_thread_id = CurrentNativeThreadId();
    captured.guest_thread_id = CurrentGuestThreadId();
    captured.list_pointer = list;
    captured.vtable_rva = vtable_rva;
    captured.command_id = command_id;
    if (command_id != CommandUnknown) {
        captured.payload_words = CapturePayload(*state->memory, command, command_id, captured);
        if (command_id == CommandSetRenderTargets)
            CaptureRenderTargets(*state->memory, command, captured);
        if (command_id == CommandDrawIndexedPrimitive && list != 0)
            CaptureShaderSnapshot(*state->memory, list, captured);
    }
    state->commands[index] = captured;
}

static PS4_SYSV_ABI void CaptureCommand(const GuestEntryRegisterFrame* frame,
                                       void* user_data) noexcept {
    try {
        CaptureCommandImpl(frame, user_data);
    } catch (...) {
        // Exceptions must never unwind through generated guest trampoline code.
    }
}

/// The deferred command currently executing on this thread.
///
/// The observer fires on entry only, so this is set at wrapper entry and consumed by the first
/// RHI invocation that runs at a deeper stack pointer. The stack check is what makes the scope
/// real: a wrapper that returns without calling the RHI method leaves a stale entry, and a later
/// unrelated RHI call at a shallower or equal stack pointer will not claim it.
[[nodiscard]] bool FindExecutableTargetSegment(const Module& module, VAddr rva, size_t size,
                                              u64& segment_rva, u64& file_size, u64& memory_size) {
    for (const auto& header : module.elf.GetProgramHeader()) {
        if (header.p_type != ::PT_LOAD || (header.p_flags & ::PF_EXEC) == 0)
            continue;
        if (RhiEntryProbe::IsExecutableLoadRange(rva, size, header.p_vaddr, header.p_filesz,
                                                 header.p_memsz)) {
            segment_rva = header.p_vaddr;
            file_size = header.p_filesz;
            memory_size = header.p_memsz;
            return true;
        }
    }
    return false;
}

/// Install one command observer, validating the segment, mapping and prologue first.
[[nodiscard]] bool InstallOne(Module& module, const CommandEntry& entry, size_t index) {
    u64 segment_rva{};
    u64 file_size{};
    u64 memory_size{};
    if (!FindExecutableTargetSegment(module, entry.rva, entry.prologue.size(), segment_rva,
                                     file_size, memory_size)) {
        LOG_WARNING(Core_Linker,
                    "RHI command capture refused: RVA {:#x} is not in a file-backed executable "
                    "PT_LOAD segment",
                    entry.rva);
        return false;
    }

    const VAddr target = module.base_virtual_addr + entry.rva;
    void* mapping_start{};
    void* mapping_end{};
    u32 protection{};
    if (module.memory->QueryProtection(target, &mapping_start, &mapping_end, &protection) != 0 ||
        reinterpret_cast<VAddr>(mapping_start) > target ||
        reinterpret_cast<VAddr>(mapping_end) < target + entry.prologue.size() ||
        (protection & static_cast<u32>(MemoryProt::CpuRead)) == 0 ||
        (protection & static_cast<u32>(MemoryProt::CpuExec)) == 0) {
        LOG_WARNING(Core_Linker,
                    "RHI command capture refused: target {:#x} is not in a readable executable "
                    "VMA",
                    target);
        return false;
    }

    const auto target_bytes =
        std::span<const u8>{reinterpret_cast<const u8*>(target), entry.prologue.size()};
    if (!MatchesCommandPrologue(entry.command_id, target_bytes)) {
        LOG_WARNING(Core_Linker,
                    "RHI command capture refused: prologue mismatch for command {} at {:#x}",
                    static_cast<u32>(entry.command_id), target);
        return false;
    }

    if (!InstallGuestEntryObserver(*module.memory, target, CaptureCommand, &g_capture_state,
                                   g_capture_state.handles[index])) {
        LOG_ERROR(Core_Linker, "RHI command capture failed to install at {:#x}", target);
        return false;
    }
    return true;
}

/// Restore every observer installed so far. Called while the guest threads are still stopped.
void RollbackInstalled(Module& module, size_t installed) {
    for (size_t i = 0; i < installed; ++i) {
        if (!g_capture_state.handles[i].active)
            continue;
        if (!RemoveGuestEntryObserver(*module.memory, g_capture_state.handles[i],
                                      GuestThreadQuiescence::Confirmed)) {
            LOG_ERROR(Core_Linker,
                      "RHI command capture rollback failed to restore the observer at {:#x}",
                      g_capture_state.handles[i].target);
        }
    }
}

} // namespace

bool CurrentDeferredCommand(CurrentCommand& out) noexcept {
    const auto& scope = g_deferred_scope;
    if (scope.capture_id == 0) {
        return false;
    }
    // The RHI invocation runs inside the wrapper, so its stack pointer is below the wrapper's
    // entry. A call at the same or a shallower pointer is not nested inside this command, which
    // means the scope is stale: the wrapper returned without issuing the RHI call. Refusing it
    // keeps a failed or early-returning command from attaching to unrelated work.
    std::uint64_t current_rsp{};
#if defined(_MSC_VER)
    current_rsp = reinterpret_cast<std::uint64_t>(_AddressOfReturnAddress());
#else
    current_rsp = reinterpret_cast<std::uint64_t>(__builtin_frame_address(0));
#endif
    if (current_rsp >= scope.entry_rsp) {
        ++g_deferred_rejected;
        return false;
    }
    out.capture_id = scope.capture_id;
    out.command_type = scope.command_type;
    out.command_list = scope.command_list;
    out.rhi_context = scope.rhi_context;
    out.ue4_primitive_type = scope.ue4_primitive_type;
    out.index_resource = scope.index_resource;
    return true;
}

void ConsumeDeferredCommand() noexcept {
    g_deferred_scope = {};
}

void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version) {
    if (!IsOptedIn(std::getenv(EnableEnvironmentVariable.data())))
        return;

#if !defined(ARCH_X86_64) || defined(SHADPS4_ENABLE_FEX_GUEST_CPU)
    LOG_WARNING(Core_Linker,
                "RHI command capture refused: this build uses FEX/translated or non-native "
                "execution");
    return;
#else
    if (module.memory == nullptr || module.name != ModuleName ||
        module.file.filename().string() != ModuleName || title_id != TitleId) {
        LOG_WARNING(Core_Linker,
                    "RHI command capture refused: expected native CUSA11919 eboot.bin, got "
                    "title='{}' module='{}' path='{}'",
                    title_id, module.name, module.file.string());
        return;
    }

    const auto image_hash = HashFile(module.file);
    if (!image_hash) {
        LOG_WARNING(Core_Linker,
                    "RHI command capture refused: unable to hash selected image '{}'",
                    module.file.string());
        return;
    }
    if (!IsTargetBuild(module.name, title_id, *image_hash)) {
        LOG_WARNING(Core_Linker,
                    "RHI command capture refused: unapproved CUSA11919 eboot.bin SHA-256 {}",
                    *image_hash);
        return;
    }
    if (module.base_virtual_addr == 0) {
        LOG_WARNING(Core_Linker, "RHI command capture refused: invalid module base");
        return;
    }

    g_capture_state.memory = module.memory;
    g_capture_state.module_base = module.base_virtual_addr;

    // Transactional: if any installation fails, restore the ones already installed. The guest
    // threads have not started yet, so the rollback is safe.
    size_t installed{};
    for (size_t i = 0; i < CommandEntries.size(); ++i) {
        if (!InstallOne(module, CommandEntries[i], i)) {
            RollbackInstalled(module, installed);
            g_capture_state.memory = nullptr;
            LOG_WARNING(Core_Linker,
                        "RHI command capture aborted: rolled back {} installed observer(s)",
                        installed);
            return;
        }
        ++installed;
    }
    g_capture_state.installed.store(true, std::memory_order_relaxed);

    // Optional bounded start delay, so a capture can target a later scene.
    u32 start_delay_seconds{};
    if (const char* delay = std::getenv(StartDelayEnvironmentVariable.data());
        delay != nullptr && *delay != '\0') {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(delay, &end, 10);
        if (end == delay || *end != '\0' || parsed > MaxStartDelaySeconds) {
            LOG_WARNING(Core_Linker,
                        "RHI command capture refused: {} must be a whole number of seconds up to "
                        "{}",
                        StartDelayEnvironmentVariable, MaxStartDelaySeconds);
            RollbackInstalled(module, installed);
            g_capture_state.installed.store(false, std::memory_order_relaxed);
            g_capture_state.memory = nullptr;
            return;
        }
        start_delay_seconds = static_cast<u32>(parsed);
    }
    if (start_delay_seconds != 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto deadline = now + std::chrono::seconds{start_delay_seconds};
        g_capture_state.start_after_ns.store(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 deadline.time_since_epoch())
                                 .count()),
            std::memory_order_relaxed);
    }

    LOG_INFO(Core_Linker,
             "DERACINE_CMD_CAPTURE_INSTALL title_id={} app_version={} image_sha256={} "
             "module_name={} module_base={:#x} installed={} command_limit={} start_delay_s={}",
             title_id, app_version, *image_hash, ModuleName, module.base_virtual_addr, installed,
             MaxCapturedCommands, start_delay_seconds);
#endif
}

void ExportCapturedCommands() {
    if (!g_capture_state.installed.load(std::memory_order_relaxed))
        return;

    const u32 captured = g_capture_state.captured_count.load(std::memory_order_relaxed);
    const u32 dropped = g_capture_state.dropped_count.load(std::memory_order_relaxed);

    // Per-type totals, so the export states what was observed rather than implying coverage.
    std::array<u32, 6> per_type{};
    std::array<u32, 9> shader_status{};
    u32 draws_with_valid_shader{};
    u32 distinct_shader_resources{};
    std::array<u64, MaxCapturedCommands> seen_resources{};
    u32 seen_count{};

    for (u32 i = 0; i < captured; ++i) {
        const auto& command = g_capture_state.commands[i];
        if (command.command_id < per_type.size())
            ++per_type[command.command_id];
        if (command.shader_status < shader_status.size())
            ++shader_status[command.shader_status];
        if (command.command_id != CommandDrawIndexedPrimitive)
            continue;
        if (!IsShaderSnapshotValid(static_cast<ShaderSnapshotStatus>(command.shader_status)))
            continue;
        ++draws_with_valid_shader;
        bool seen{};
        for (u32 j = 0; j < seen_count; ++j) {
            if (seen_resources[j] == command.shader_resource) {
                seen = true;
                break;
            }
        }
        if (!seen && seen_count < seen_resources.size())
            seen_resources[seen_count++] = command.shader_resource;
    }
    distinct_shader_resources = seen_count;

    LOG_INFO(Core_Linker,
             "DERACINE_CMD_CAPTURE_SUMMARY captured={} dropped={} limit={} "
             "draw_indexed={} draw={} stream_source={} render_targets={} viewport={} unknown={}",
             captured, dropped, MaxCapturedCommands, per_type[CommandDrawIndexedPrimitive],
             per_type[CommandDrawPrimitive], per_type[CommandSetStreamSource],
             per_type[CommandSetRenderTargets], per_type[CommandSetViewport],
             per_type[CommandUnknown]);
    LOG_INFO(Core_Linker,
             "DERACINE_CMD_CAPTURE_SHADER draws_with_valid_shader={} distinct_shader_resources={} "
             "status_not_attempted={} status_valid={} status_context_unreadable={} "
             "status_state_null={} status_state_unreadable={} status_resource_null={} "
             "status_program_null={} status_program_unreadable={} status_valid_via_alternate={}",
             draws_with_valid_shader, distinct_shader_resources, shader_status[0],
             shader_status[1], shader_status[2], shader_status[3], shader_status[4],
             shader_status[5], shader_status[6], shader_status[7], shader_status[8]);

    // Emit the records only now, outside the guest hot path.
    for (u32 i = 0; i < captured; ++i) {
        const auto& c = g_capture_state.commands[i];
        LOG_INFO(Core_Linker,
                 "DERACINE_CMD_CAPTURE sequence={} native_thread={} guest_thread={} list={:#x} "
                 "context={:#x} vtable_rva={:#x} command_id={} payload_words={} "
                 "payload=[{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}] "
                 "rt_count={} rt=[{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}] "
                 "depth={:#x} uav_count={} uav=[{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}] "
                 "shader_status={} shader_state={:#x} shader_resource={:#x} program_data={:#x} "
                 "shader_modifier={:#x}",
                 c.sequence, c.native_thread_id, c.guest_thread_id, c.list_pointer,
                 c.context_pointer, c.vtable_rva, static_cast<u32>(c.command_id),
                 static_cast<u32>(c.payload_words), c.payload[0], c.payload[1], c.payload[2],
                 c.payload[3], c.payload[4], c.payload[5], c.payload[6], c.payload[7],
                 static_cast<u32>(c.render_target_count), c.render_targets[0], c.render_targets[1],
                 c.render_targets[2], c.render_targets[3], c.render_targets[4], c.render_targets[5],
                 c.render_targets[6], c.render_targets[7], c.depth_target,
                 static_cast<u32>(c.uav_count), c.uavs[0], c.uavs[1], c.uavs[2], c.uavs[3],
                 c.uavs[4], c.uavs[5], c.uavs[6], c.uavs[7], static_cast<u32>(c.shader_status),
                 c.shader_state, c.shader_resource, c.program_data, c.shader_modifier);
    }
}

} // namespace Core::RhiCommandCapture
