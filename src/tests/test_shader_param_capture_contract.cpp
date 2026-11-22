// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/shader_param_capture.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace {

using Core::ShaderParamCapture::CaptureStatus;
using Core::ShaderParamCapture::EnableEnvironmentVariable;
using Core::ShaderParamCapture::HashCapturedBytes;
using Core::ShaderParamCapture::MaxBindingsPerDraw;
using Core::ShaderParamCapture::MaxBytesPerResource;
using Core::ShaderParamCapture::MaxCapturedDraws;
using Core::ShaderParamCapture::MaxTotalBytes;
using Core::ShaderParamCapture::MaxUserDataRegs;

TEST(ShaderParamCaptureContract, BoundsAreSane) {
    EXPECT_GT(MaxCapturedDraws, 0u);
    EXPECT_LE(MaxCapturedDraws, 1024u);
    EXPECT_GT(MaxBindingsPerDraw, 0u);
    EXPECT_LE(MaxBindingsPerDraw, 256u);
    // The PS4 exposes 32 user-data registers per stage.
    EXPECT_GT(MaxUserDataRegs, 0u);
    EXPECT_LE(MaxUserDataRegs, 32u);
    EXPECT_GT(MaxBytesPerResource, 0u);
    EXPECT_LE(MaxBytesPerResource, 1024u * 1024u);
    EXPECT_GT(MaxTotalBytes, 0u);
    EXPECT_LE(MaxTotalBytes, 64u * 1024u * 1024u);
    // A single resource must not be able to consume the whole budget.
    EXPECT_LT(MaxBytesPerResource, MaxTotalBytes);
}

TEST(ShaderParamCaptureContract, OptInIsSeparateFromOtherTraces) {
    EXPECT_FALSE(EnableEnvironmentVariable.empty());
    EXPECT_NE(EnableEnvironmentVariable, std::string_view{"SHADPS4_DERACINE_RHI_PROBE"});
    EXPECT_NE(EnableEnvironmentVariable, std::string_view{"SHADPS4_DERACINE_DRAW_PROVENANCE"});
    EXPECT_NE(EnableEnvironmentVariable, std::string_view{"SHADPS4_DERACINE_VERTEX_INPUT"});
    EXPECT_NE(EnableEnvironmentVariable, std::string_view{"SHADPS4_DERACINE_CMD_CAPTURE"});
}

TEST(ShaderParamCaptureContract, ContentHashIsStableAndSensitive) {
    // The hash is the version identity, so it must be deterministic for identical bytes and must
    // change when the bytes change.
    const std::array<u8, 8> a{1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<u8, 8> b{1, 2, 3, 4, 5, 6, 7, 9};
    EXPECT_EQ(HashCapturedBytes(a.data(), a.size()), HashCapturedBytes(a.data(), a.size()));
    EXPECT_NE(HashCapturedBytes(a.data(), a.size()), HashCapturedBytes(b.data(), b.size()));
    // A prefix must not collide with the whole.
    EXPECT_NE(HashCapturedBytes(a.data(), 4), HashCapturedBytes(a.data(), 8));
    // Empty input is well defined.
    EXPECT_EQ(HashCapturedBytes(nullptr, 0), HashCapturedBytes(nullptr, 0));
}

TEST(ShaderParamCaptureContract, ByteBudgetIsBounded) {
    // The budget is global and atomic, so concurrent producers cannot jointly exceed it. The
    // enforcement lives in the capture translation unit; here the invariant is that the limits
    // themselves are coherent.
    EXPECT_GT(MaxTotalBytes, 0u);
    EXPECT_LT(MaxBytesPerResource, MaxTotalBytes);
    // A draw's bindings must not be able to exceed the per-resource budget in aggregate without
    // being reported as partial.
    EXPECT_GT(MaxBindingsPerDraw, 0u);
}

TEST(ShaderParamCaptureContract, StatusValuesAreDistinct) {
    // The exporter distinguishes complete, metadata-only, partial, unsupported and invalid, so a
    // package can say what it actually has rather than implying completeness.
    const std::array<CaptureStatus, 5> all{
        CaptureStatus::Complete, CaptureStatus::MetadataOnly, CaptureStatus::Partial,
        CaptureStatus::Unsupported, CaptureStatus::Invalid};
    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t j = i + 1; j < all.size(); ++j) {
            EXPECT_NE(static_cast<u8>(all[i]), static_cast<u8>(all[j]));
        }
    }
}

} // namespace
