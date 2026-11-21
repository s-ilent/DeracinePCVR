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

} // namespace Core::RhiCommandCapture
