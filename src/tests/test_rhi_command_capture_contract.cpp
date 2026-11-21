// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/rhi_command_capture_contract.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace {

using Core::RhiCommandCapture::ClassifyCommandVtable;
using Core::RhiCommandCapture::CommandDrawIndexedPrimitive;
using Core::RhiCommandCapture::CommandDrawPrimitive;
using Core::RhiCommandCapture::CommandEntries;
using Core::RhiCommandCapture::CommandSetRenderTargets;
using Core::RhiCommandCapture::CommandSetStreamSource;
using Core::RhiCommandCapture::CommandSetViewport;
using Core::RhiCommandCapture::CommandUnknown;
using Core::RhiCommandCapture::EbootV100Sha256;
using Core::RhiCommandCapture::EbootV101Sha256;
using Core::RhiCommandCapture::IsOptedIn;
using Core::RhiCommandCapture::IsShaderSnapshotValid;
using Core::RhiCommandCapture::IsTargetBuild;
using Core::RhiCommandCapture::MatchesCommandPrologue;
using Core::RhiCommandCapture::MaxCapturedCommands;
using Core::RhiCommandCapture::MaxRenderTargets;
using Core::RhiCommandCapture::MaxUavs;
using Core::RhiCommandCapture::ModuleName;
using Core::RhiCommandCapture::ShaderSnapshotStatus;
using Core::RhiCommandCapture::TitleId;

TEST(RhiCommandCaptureContract, OptInRequiresExactlyOne) {
    EXPECT_FALSE(IsOptedIn(nullptr));
    EXPECT_FALSE(IsOptedIn(""));
    EXPECT_FALSE(IsOptedIn("0"));
    EXPECT_FALSE(IsOptedIn("01"));
    EXPECT_FALSE(IsOptedIn("1 "));
    EXPECT_TRUE(IsOptedIn("1"));
}

TEST(RhiCommandCaptureContract, OnlyApprovedFullImageIdentitiesAreAccepted) {
    EXPECT_TRUE(IsTargetBuild(ModuleName, TitleId, EbootV100Sha256));
    EXPECT_TRUE(IsTargetBuild(ModuleName, TitleId, EbootV101Sha256));
    EXPECT_FALSE(IsTargetBuild("other.bin", TitleId, EbootV100Sha256));
    EXPECT_FALSE(IsTargetBuild(ModuleName, "CUSA00000", EbootV100Sha256));
    EXPECT_FALSE(IsTargetBuild(ModuleName, TitleId, ""));
}

TEST(RhiCommandCaptureContract, VtableClassifiesTheFiveRecoveredCommands) {
    EXPECT_EQ(ClassifyCommandVtable(0x03F26A08), CommandSetViewport);
    EXPECT_EQ(ClassifyCommandVtable(0x03F26A28), CommandSetRenderTargets);
    EXPECT_EQ(ClassifyCommandVtable(0x03F26A48), CommandSetStreamSource);
    EXPECT_EQ(ClassifyCommandVtable(0x03F26A68), CommandDrawPrimitive);
    EXPECT_EQ(ClassifyCommandVtable(0x03F26A88), CommandDrawIndexedPrimitive);
}

TEST(RhiCommandCaptureContract, UnknownVtablesAreReportedAsUnknown) {
    EXPECT_EQ(ClassifyCommandVtable(0), CommandUnknown);
    EXPECT_EQ(ClassifyCommandVtable(0x03F26A00), CommandUnknown);
    EXPECT_EQ(ClassifyCommandVtable(0x03F26A90), CommandUnknown);
    EXPECT_EQ(ClassifyCommandVtable(0x03F26A10), CommandUnknown);
    EXPECT_EQ(ClassifyCommandVtable(0x0CBB9C0), CommandUnknown);
}

TEST(RhiCommandCaptureContract, EveryEntryHasADistinctCommandIdAndPrologue) {
    for (size_t i = 0; i < CommandEntries.size(); ++i) {
        EXPECT_NE(CommandEntries[i].command_id, CommandUnknown);
        for (size_t j = i + 1; j < CommandEntries.size(); ++j) {
            EXPECT_NE(CommandEntries[i].command_id, CommandEntries[j].command_id);
            EXPECT_NE(CommandEntries[i].rva, CommandEntries[j].rva);
        }
    }
}

TEST(RhiCommandCaptureContract, PrologueMatchRequiresTheExactBytes) {
    for (const auto& entry : CommandEntries) {
        EXPECT_TRUE(MatchesCommandPrologue(entry.command_id, entry.prologue));

        auto modified = entry.prologue;
        modified[8] ^= 0x01;
        EXPECT_FALSE(MatchesCommandPrologue(entry.command_id, modified));

        EXPECT_FALSE(MatchesCommandPrologue(
            entry.command_id,
            std::span<const u8>{entry.prologue.data(), entry.prologue.size() - 1}));
    }
}

TEST(RhiCommandCaptureContract, PrologueMatchIsScopedToTheCommandId) {
    const auto& draw = CommandEntries[0];
    for (size_t i = 1; i < CommandEntries.size(); ++i) {
        if (CommandEntries[i].prologue == draw.prologue)
            continue;
        EXPECT_FALSE(MatchesCommandPrologue(CommandEntries[i].command_id, draw.prologue));
    }
}

TEST(RhiCommandCaptureContract, CaptureIsBounded) {
    EXPECT_GT(MaxCapturedCommands, 0u);
    EXPECT_LE(MaxCapturedCommands, 4096u);
}

TEST(RhiCommandCaptureContract, RenderTargetAndUavBoundsAreClamped) {
    // The capture clamps the guest count to these capacities, so a corrupt count cannot read past
    // the payload. The public header's MaxSimultaneousRenderTargets is 8.
    EXPECT_EQ(MaxRenderTargets, 8u);
    EXPECT_GT(MaxUavs, 0u);
    EXPECT_LE(MaxUavs, 8u);
}

TEST(RhiCommandCaptureContract, OnlyTheValidShaderStatusCountsAsAvailable) {
    EXPECT_TRUE(IsShaderSnapshotValid(ShaderSnapshotStatus::Valid));
    EXPECT_TRUE(IsShaderSnapshotValid(ShaderSnapshotStatus::ValidViaAlternate));
    EXPECT_FALSE(IsShaderSnapshotValid(ShaderSnapshotStatus::NotAttempted));
    EXPECT_FALSE(IsShaderSnapshotValid(ShaderSnapshotStatus::ContextUnreadable));
    EXPECT_FALSE(IsShaderSnapshotValid(ShaderSnapshotStatus::ShaderStateNull));
    EXPECT_FALSE(IsShaderSnapshotValid(ShaderSnapshotStatus::ShaderStateUnreadable));
    EXPECT_FALSE(IsShaderSnapshotValid(ShaderSnapshotStatus::ShaderResourceNull));
    EXPECT_FALSE(IsShaderSnapshotValid(ShaderSnapshotStatus::ProgramDataNull));
    EXPECT_FALSE(IsShaderSnapshotValid(ShaderSnapshotStatus::ProgramDataUnreadable));
}

TEST(RhiCommandCaptureContract, ShaderStatusValuesAreDistinctAndBounded) {
    // The export indexes a fixed-size status histogram, so every status must fit it.
    constexpr std::array<ShaderSnapshotStatus, 9> all{
        ShaderSnapshotStatus::NotAttempted,       ShaderSnapshotStatus::Valid,
        ShaderSnapshotStatus::ContextUnreadable,  ShaderSnapshotStatus::ShaderStateNull,
        ShaderSnapshotStatus::ShaderStateUnreadable, ShaderSnapshotStatus::ShaderResourceNull,
        ShaderSnapshotStatus::ProgramDataNull,    ShaderSnapshotStatus::ProgramDataUnreadable,
        ShaderSnapshotStatus::ValidViaAlternate,
    };
    for (size_t i = 0; i < all.size(); ++i) {
        EXPECT_LT(static_cast<u8>(all[i]), all.size());
        for (size_t j = i + 1; j < all.size(); ++j)
            EXPECT_NE(all[i], all[j]);
    }
}

} // namespace
