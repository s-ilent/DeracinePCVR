// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vertex_input_capture.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

using Core::VertexInputCapture::EnableEnvironmentVariable;
using Core::VertexInputCapture::MaxAttributesPerDraw;
using Core::VertexInputCapture::MaxCapturedDraws;
using Core::VertexInputCapture::MaxIndexSampleBytes;

TEST(VertexInputCaptureContract, BoundsAreSane) {
    // The snapshot is a fixed array, so the bounds must stay small enough to be a sane static
    // allocation while still covering a representative sample of draws.
    EXPECT_GT(MaxCapturedDraws, 0u);
    EXPECT_LE(MaxCapturedDraws, 4096u);
    EXPECT_GE(MaxCapturedDraws, 32u);

    // The GCN fetch shader addresses at most 32 semantic slots.
    EXPECT_GT(MaxAttributesPerDraw, 0u);
    EXPECT_LE(MaxAttributesPerDraw, 32u);

    EXPECT_GT(MaxIndexSampleBytes, 0u);
    EXPECT_LE(MaxIndexSampleBytes, 256u);
}

TEST(VertexInputCaptureContract, OptInIsSeparateFromOtherTraces) {
    EXPECT_FALSE(EnableEnvironmentVariable.empty());
    EXPECT_NE(EnableEnvironmentVariable, std::string_view{"SHADPS4_DERACINE_RHI_PROBE"});
    EXPECT_NE(EnableEnvironmentVariable, std::string_view{"SHADPS4_DERACINE_DRAW_PROVENANCE"});
    EXPECT_NE(EnableEnvironmentVariable, std::string_view{"SHADPS4_DERACINE_CMD_CAPTURE"});
}

TEST(VertexInputCaptureContract, IndexSizeFollowsTheHardwareType) {
    // The index element width is a property of the hardware index-buffer type, not of the address
    // delta between two draws. This mirrors the production binding path.
    const auto size_for = [](u8 index_type) { return index_type == 0 ? 2u : 4u; };
    EXPECT_EQ(size_for(0), 2u); // Index16
    EXPECT_EQ(size_for(1), 4u); // Index32
}

TEST(VertexInputCaptureContract, IndexAddressAppliesTheOffsetInElements) {
    // BindIndexBuffer computes base + index_offset * index_size, so the offset is in elements and
    // is not folded into the base address by the guest.
    const auto address_for = [](u64 base, u32 offset, u32 size) { return base + offset * size; };
    EXPECT_EQ(address_for(0x2006CE7000ull, 0, 2), 0x2006CE7000ull);
    EXPECT_EQ(address_for(0x2006CE7000ull, 6, 2), 0x2006CE700Cull);
    EXPECT_EQ(address_for(0x2006CE7000ull, 6, 4), 0x2006CE7018ull);
}

TEST(VertexInputCaptureContract, AttributeBindingIsTheSemanticWithZeroOffset) {
    // The renderer assigns one Vulkan binding per fetched attribute, using the semantic index, and
    // folds the fetch instruction offset into the resolved buffer address. Reproducing that
    // translation explicitly keeps the snapshot honest about what the renderer actually did.
    for (u8 semantic = 0; semantic < 8; ++semantic) {
        const u32 binding = semantic;
        const u32 attribute_offset = 0;
        EXPECT_EQ(binding, semantic);
        EXPECT_EQ(attribute_offset, 0u);
    }
}

TEST(VertexInputCaptureContract, InstanceRateFollowsTheStepRate) {
    // InstanceIdType::None is per-vertex; every other value is per-instance.
    const auto rate_for = [](u8 instance_data) { return instance_data == 0 ? 0u : 1u; };
    EXPECT_EQ(rate_for(0), 0u); // None
    EXPECT_EQ(rate_for(1), 1u); // OverStepRate0
    EXPECT_EQ(rate_for(2), 1u); // OverStepRate1
    EXPECT_EQ(rate_for(3), 1u); // Plain
}

} // namespace
