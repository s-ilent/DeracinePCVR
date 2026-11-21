// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <string_view>

#include "common/types.h"

namespace Core::RhiCommandCapture {

inline constexpr std::string_view TitleId = "CUSA11919";
inline constexpr std::string_view ModuleName = "eboot.bin";
inline constexpr std::string_view EbootV100Sha256 =
    "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0";
inline constexpr std::string_view EbootV101Sha256 =
    "8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75";

/// Opt-in; only the exact value "1" enables the capture.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_CMD_CAPTURE";

/// Optional bounded start delay in seconds, so a capture can target a later scene instead of the
/// first one. Absent or non-positive means capture from the first command.
inline constexpr std::string_view StartDelayEnvironmentVariable =
    "SHADPS4_DERACINE_CMD_CAPTURE_AFTER";

/// Upper bound on the accepted start delay. A larger value is refused rather than clamped, so a
/// typo cannot silently capture nothing.
inline constexpr u32 MaxStartDelaySeconds = 600;

/// Bounded record count. The capture never allocates and never grows.
inline constexpr size_t MaxCapturedCommands = 512;

/// The five recovered command Execute wrappers, with their exact 16-byte entry prologues.
/// Each prologue is displacement-free, so the displaced instructions need no fixups.
struct CommandEntry {
    VAddr rva;
    std::array<u8, 16> prologue;
    u8 command_id;
};

inline constexpr u8 CommandUnknown = 0;
inline constexpr u8 CommandDrawIndexedPrimitive = 1;
inline constexpr u8 CommandDrawPrimitive = 2;
inline constexpr u8 CommandSetStreamSource = 3;
inline constexpr u8 CommandSetRenderTargets = 4;
inline constexpr u8 CommandSetViewport = 5;

inline constexpr std::array<CommandEntry, 5> CommandEntries{{
    {0x13A68B0,
     {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x48, 0x8B, 0x46, 0x20, 0x48, 0x8B, 0x77, 0x10, 0x8B,
      0x57},
     CommandDrawIndexedPrimitive},
    {0x13A6900,
     {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x46, 0x20, 0x8B, 0x77, 0x10, 0x8B, 0x57, 0x14, 0x8B,
      0x4F},
     CommandDrawPrimitive},
    {0x13A6930,
     {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x4C,
      0x8B},
     CommandSetStreamSource},
    {0x13A6A30,
     {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x10, 0x48, 0x8B, 0x46, 0x20, 0x8B, 0x77, 0x10,
      0x44},
     CommandSetRenderTargets},
    {0x13A6BB0,
     {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x46, 0x20, 0x8B, 0x77, 0x10, 0x8B, 0x57, 0x14, 0xC5,
      0xFA},
     CommandSetViewport},
}};

[[nodiscard]] inline bool IsOptedIn(const char* value) noexcept {
    return value != nullptr && std::string_view{value} == "1";
}

[[nodiscard]] inline bool IsApprovedImageHash(std::string_view image_sha256) noexcept {
    return image_sha256 == EbootV100Sha256 || image_sha256 == EbootV101Sha256;
}

[[nodiscard]] inline bool IsTargetBuild(std::string_view module_name, std::string_view title_id,
                                        std::string_view image_sha256) noexcept {
    return module_name == ModuleName && title_id == TitleId && IsApprovedImageHash(image_sha256);
}

[[nodiscard]] inline bool MatchesCommandPrologue(u8 command_id,
                                                std::span<const u8> bytes) noexcept {
    for (const auto& entry : CommandEntries) {
        if (entry.command_id != command_id)
            continue;
        return bytes.size() >= entry.prologue.size() &&
               std::equal(entry.prologue.begin(), entry.prologue.end(), bytes.begin());
    }
    return false;
}

/// The command object's vtable identifies its type exactly. The five recovered vtables are
/// 0x20 apart, so the identity is a range check plus an index.
inline constexpr VAddr FirstCommandVtableRva = 0x03F26A08;
inline constexpr VAddr LastCommandVtableRva = 0x03F26A88;
inline constexpr VAddr CommandVtableStride = 0x20;

[[nodiscard]] inline u8 ClassifyCommandVtable(VAddr vtable_rva) noexcept {
    if (vtable_rva < FirstCommandVtableRva || vtable_rva > LastCommandVtableRva)
        return CommandUnknown;
    const VAddr offset = vtable_rva - FirstCommandVtableRva;
    if (offset % CommandVtableStride != 0)
        return CommandUnknown;
    switch (offset / CommandVtableStride) {
    case 0:
        return CommandSetViewport;
    case 1:
        return CommandSetRenderTargets;
    case 2:
        return CommandSetStreamSource;
    case 3:
        return CommandDrawPrimitive;
    case 4:
        return CommandDrawIndexedPrimitive;
    default:
        return CommandUnknown;
    }
}

/// The command list's RHI context is at +0x20. The shader-state chain below it is statically
/// supported by A6/A7 but its validity at the draw-wrapper entry is what SC-02 measures.
inline constexpr u32 CommandListContextOffset = 0x20;
inline constexpr u32 ContextShaderStateOffset = 0x468;
inline constexpr u32 ShaderStateResourceOffset = 0x200;
inline constexpr u32 ShaderStateAlternateResourceOffset = 0x1E0;
inline constexpr u32 ShaderStateModifierOffset = 0x1C0;
inline constexpr u32 ShaderResourceProgramDataOffset = 0x1D8;

/// Render-target array geometry, verified in A5/A9.
inline constexpr u32 RenderTargetCountOffset = 0x10;
inline constexpr u32 RenderTargetArrayOffset = 0x18;
inline constexpr u32 RenderTargetStride = 0x18;
inline constexpr u32 MaxRenderTargets = 8;
inline constexpr u32 DepthTargetOffset = 0xD8;
inline constexpr u32 UavCountOffset = 0xE8;
inline constexpr u32 UavArrayOffset = 0xF0;
inline constexpr u32 MaxUavs = 8;

/// A shader snapshot is only recorded when the whole pointer chain validates. The status field
/// records which link failed, so an unavailable chain is evidence rather than a silent zero.
///
/// The shader-state object holds TWO resource slots. The receiver at 0x0CCC2F0 reads +0x200 and,
/// when that is null, falls back to +0x1E0. Both then read resource+0x1D8 as the program data.
/// A snapshot that only reads +0x200 will report null on every draw that uses the alternate slot.
enum class ShaderSnapshotStatus : u8 {
    NotAttempted = 0,
    Valid = 1,
    ContextUnreadable = 2,
    ShaderStateNull = 3,
    ShaderStateUnreadable = 4,
    ShaderResourceNull = 5,
    ProgramDataNull = 6,
    ProgramDataUnreadable = 7,
    ValidViaAlternate = 8,
};

[[nodiscard]] inline bool IsShaderSnapshotValid(ShaderSnapshotStatus status) noexcept {
    return status == ShaderSnapshotStatus::Valid ||
           status == ShaderSnapshotStatus::ValidViaAlternate;
}

} // namespace Core::RhiCommandCapture
