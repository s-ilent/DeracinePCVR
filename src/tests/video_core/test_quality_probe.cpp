// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/quality_probe.h"

using namespace Vulkan::QualityDiag;

TEST(QualityProbe, ParsesEachKind) {
    const auto specs = ParseProbeList("u8@0x847c8130;u32@0x847c8138;f32@0x8035b030c;"
                                      "ptr_f32@0x847c8138");
    ASSERT_EQ(specs.size(), 4u);
    EXPECT_EQ(specs[0].kind, ProbeKind::U8);
    EXPECT_EQ(specs[0].address, 0x847c8130u);
    EXPECT_EQ(specs[1].kind, ProbeKind::U32);
    EXPECT_EQ(specs[1].address, 0x847c8138u);
    EXPECT_EQ(specs[2].kind, ProbeKind::F32);
    EXPECT_EQ(specs[2].address, 0x8035b030cu);
    EXPECT_EQ(specs[3].kind, ProbeKind::PointerToF32);
    EXPECT_EQ(specs[3].address, 0x847c8138u);
}

TEST(QualityProbe, AcceptsAddressesWithoutPrefixAndSkipsWhatItCannotRead) {
    const auto specs = ParseProbeList("f32@8035b030c;bogus@1234;f32@;@7;f32@zz;"
                                      "u32@0x1234");
    ASSERT_EQ(specs.size(), 2u);
    EXPECT_EQ(specs[0].address, 0x8035b030cu);
    EXPECT_EQ(specs[0].kind, ProbeKind::F32);
    EXPECT_EQ(specs[1].address, 0x1234u);
    EXPECT_EQ(specs[1].kind, ProbeKind::U32);
}

TEST(QualityProbe, SkipsZeroAndTrailingTheOnlyEntry) {
    EXPECT_TRUE(ParseProbeList("").empty());
    EXPECT_TRUE(ParseProbeList("u8@0").empty());
    const auto specs = ParseProbeList("u8@0x10;");
    ASSERT_EQ(specs.size(), 1u);
    EXPECT_EQ(specs[0].address, 0x10u);
}
