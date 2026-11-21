// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/rhi_command_capture.h"

#include <algorithm>
#include <array>
#include <atomic>
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
    u64 vtable_rva{};
    u64 payload[8]{};
    u8 command_id{};
    u8 payload_words{};
    u8 list_id{};
};

struct CaptureState {
    MemoryManager* memory{};
    VAddr module_base{};
    std::array<GuestEntryObserverHandle, CommandEntries.size()> handles{};
    std::atomic<u32> captured_count{};
    std::atomic<u32> unknown_vtable_count{};
    std::atomic<bool> limit_logged{};
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

/// Read one guest word, but only after proving the address is in a readable mapping. This is the
/// only guest read the capture performs, and it is bounded to the command object's own payload.
[[nodiscard]] bool ReadGuestWord(MemoryManager& memory, VAddr address, u64& value) noexcept {
    if (address == 0 || address > (std::numeric_limits<VAddr>::max)() - sizeof(u64))
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
    if (region_start > address || region_end < address + sizeof(u64))
        return false;

    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(u64));
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
    if (!state.limit_logged.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO(Core_Linker, "RHI command capture reached its {}-command limit",
                 MaxCapturedCommands);
    }
    return false;
}

/// Copy the scalar payload for a known command type. The offsets are the verified payload field
/// offsets from the A5/A6/A7/A8/A9 static work. Only scalars and opaque pointer values are read.
[[nodiscard]] u8 CapturePayload(MemoryManager& memory, VAddr command, u8 command_id,
                                u64 (&payload)[8]) noexcept {
    struct Field {
        u32 offset;
        u8 size;
    };
    std::array<Field, 8> fields{};
    u8 count{};

    switch (command_id) {
    case CommandDrawIndexedPrimitive:
        // IndexBuffer(+0x10, opaque), PrimitiveType(+0x18), BaseVertexIndex(+0x1C),
        // FirstInstance(+0x20), NumVertices(+0x24), StartIndex(+0x28), NumPrimitives(+0x2C),
        // NumInstances(+0x30).
        fields = {{{0x10, 8}, {0x18, 4}, {0x1C, 4}, {0x20, 4},
                   {0x24, 4}, {0x28, 4}, {0x2C, 4}, {0x30, 4}}};
        count = 8;
        break;
    case CommandDrawPrimitive:
        // PrimitiveType(+0x10), BaseVertexIndex(+0x14), NumPrimitives(+0x18),
        // NumInstances(+0x1C).
        fields = {{{0x10, 4}, {0x14, 4}, {0x18, 4}, {0x1C, 4}}};
        count = 4;
        break;
    case CommandSetStreamSource:
        // StreamIndex(+0x10), VertexBuffer(+0x18, opaque), Offset(+0x20).
        fields = {{{0x10, 4}, {0x18, 8}, {0x20, 4}}};
        count = 3;
        break;
    case CommandSetRenderTargets:
        // count(+0x10), RT[0](+0x18, opaque), depth(+0xD8, opaque), UAV count(+0xE8),
        // UAV[0](+0xF0, opaque).
        fields = {{{0x10, 4}, {0x18, 8}, {0xD8, 8}, {0xE8, 4}, {0xF0, 8}}};
        count = 5;
        break;
    case CommandSetViewport:
        // MinX(+0x10), MinY(+0x14), MinZ(+0x18, float), MaxX(+0x1C), MaxY(+0x20),
        // MaxZ(+0x24, float).
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
        payload[i] = word;
    }
    return count;
}

static void CaptureCommandImpl(const GuestEntryRegisterFrame* frame, void* user_data) {
    auto* state = static_cast<CaptureState*>(user_data);
    if (frame == nullptr || state == nullptr || state->memory == nullptr)
        return;

    // The command object is the first argument. The command list is the second and carries the
    // context at +0x20; it is not read here because the payload is what identifies the command.
    const VAddr command = frame->rdi;
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
    captured.vtable_rva = vtable_rva;
    captured.command_id = command_id;
    if (command_id == CommandUnknown) {
        state->unknown_vtable_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        captured.payload_words = CapturePayload(*state->memory, command, command_id,
                                               captured.payload);
    }
    state->commands[index] = captured;

    LOG_INFO(Core_Linker,
             "DERACINE_CMD_CAPTURE sequence={} thread_id={} vtable_rva={:#x} command_id={} "
             "payload_words={} payload=[{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x},{:#x}]",
             captured.sequence, captured.native_thread_id, captured.vtable_rva,
             static_cast<u32>(captured.command_id), static_cast<u32>(captured.payload_words),
             captured.payload[0], captured.payload[1], captured.payload[2], captured.payload[3],
             captured.payload[4], captured.payload[5], captured.payload[6], captured.payload[7]);
}

static PS4_SYSV_ABI void CaptureCommand(const GuestEntryRegisterFrame* frame,
                                       void* user_data) noexcept {
    try {
        CaptureCommandImpl(frame, user_data);
    } catch (...) {
        // Exceptions must never unwind through generated guest trampoline code.
    }
}

[[nodiscard]] bool FindExecutableTargetSegment(const Module& module, VAddr rva, size_t size,
                                              u64& segment_rva, u64& segment_file_size,
                                              u64& segment_memory_size) {
    for (const auto& header : module.elf.GetProgramHeader()) {
        if (header.p_type != ::PT_LOAD || (header.p_flags & ::PF_EXEC) == 0)
            continue;
        if (RhiEntryProbe::IsExecutableLoadRange(rva, size, header.p_vaddr, header.p_filesz,
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

    size_t installed{};
    for (size_t i = 0; i < CommandEntries.size(); ++i) {
        const auto& entry = CommandEntries[i];
        u64 segment_rva{};
        u64 segment_file_size{};
        u64 segment_memory_size{};
        if (!FindExecutableTargetSegment(module, entry.rva, entry.prologue.size(), segment_rva,
                                         segment_file_size, segment_memory_size)) {
            LOG_WARNING(Core_Linker,
                        "RHI command capture refused: RVA {:#x} is not in a file-backed "
                        "executable PT_LOAD segment",
                        entry.rva);
            return;
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
                        "RHI command capture refused: target {:#x} is not in a readable "
                        "executable VMA",
                        target);
            return;
        }

        const auto target_bytes =
            std::span<const u8>{reinterpret_cast<const u8*>(target), entry.prologue.size()};
        if (!MatchesCommandPrologue(entry.command_id, target_bytes)) {
            LOG_WARNING(Core_Linker,
                        "RHI command capture refused: prologue mismatch for command {} at {:#x}",
                        static_cast<u32>(entry.command_id), target);
            return;
        }

        if (!InstallGuestEntryObserver(*module.memory, target, CaptureCommand, &g_capture_state,
                                       g_capture_state.handles[i])) {
            LOG_ERROR(Core_Linker, "RHI command capture failed to install at {:#x}", target);
            return;
        }
        ++installed;
    }

    LOG_INFO(Core_Linker,
             "DERACINE_CMD_CAPTURE_INSTALL title_id={} app_version={} image_sha256={} "
             "module_name={} module_base={:#x} installed={} command_limit={}",
             title_id, app_version, *image_hash, ModuleName, module.base_virtual_addr, installed,
             MaxCapturedCommands);
#endif
}

} // namespace Core::RhiCommandCapture
