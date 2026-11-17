// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "video_core/color_contract.h"

namespace VideoCore::Color {
namespace {

// The piecewise sRGB curves are evaluated in float; both round trips stay well inside this.
constexpr float CurveTolerance = 1e-6f;
// The shader sees the encoded byte table, not a continuous curve: this is the same bound on it.
constexpr float ShaderCurveTolerance = 1e-5f;
// The encoded-byte pass-through on an sRGB target decodes and re-encodes through float, while
// the byte was already what the target must store. The contract allows one LSB of rounding.
constexpr int MaxPassThroughLsb = 1;

int LsbDistance(const std::uint8_t lhs, const std::uint8_t rhs) {
    return std::abs(static_cast<int>(lhs) - static_cast<int>(rhs));
}

TEST(ColorContract, SrgbCurve) {
    EXPECT_EQ(LinearToSrgb(0.0f), 0.0f);
    // SrgbA - SrgbB rounds one ULP below 1.0f; the byte the target stores is still 255.
    EXPECT_FLOAT_EQ(LinearToSrgb(1.0f), 1.0f);
    EXPECT_EQ(Unorm8Byte(LinearToSrgb(1.0f)), 255);
    EXPECT_EQ(LinearToSrgb(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(LinearToSrgb(1.5f), LinearToSrgb(1.0f));

    EXPECT_EQ(SrgbToLinear(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(SrgbToLinear(1.0f), 1.0f);
    EXPECT_EQ(SrgbToLinear(-0.5f), 0.0f);
    EXPECT_EQ(SrgbToLinear(1.5f), 1.0f);

    float previous = LinearToSrgb(0.0f);
    for (int step = 1; step <= 1000; ++step) {
        const float linear = static_cast<float>(step) / 1000.0f;
        const float encoded = LinearToSrgb(linear);
        EXPECT_GE(encoded, previous) << "not monotonic at linear " << linear;
        previous = encoded;
    }

    // The samples straddle the piecewise breakpoints (SrgbCutoff on the linear domain, 0.04045f
    // on the encoded one), where a threshold mismatch would show up first.
    for (int step = 0; step <= 1000; ++step) {
        const float value = static_cast<float>(step) / 1000.0f;
        EXPECT_NEAR(SrgbToLinear(LinearToSrgb(value)), value, CurveTolerance) << "at " << value;
        EXPECT_NEAR(LinearToSrgb(SrgbToLinear(value)), value, CurveTolerance) << "at " << value;
    }
    for (const float value : {SrgbCutoff, 0.04045f}) {
        EXPECT_NEAR(SrgbToLinear(LinearToSrgb(value)), value, CurveTolerance) << "at " << value;
        EXPECT_NEAR(LinearToSrgb(SrgbToLinear(value)), value, CurveTolerance) << "at " << value;
    }

    // Unorm8Value and Unorm8Byte are exact inverses over the whole byte range.
    for (int byte = 0; byte <= 255; ++byte) {
        const auto stored = static_cast<std::uint8_t>(byte);
        EXPECT_EQ(Unorm8Byte(Unorm8Value(stored)), byte) << "byte " << byte;
    }
    EXPECT_EQ(Unorm8Value(std::uint8_t{0}), 0.0f);
    EXPECT_EQ(Unorm8Value(std::uint8_t{255}), 1.0f);
    EXPECT_EQ(Unorm8Byte(0.0f), 0);
    EXPECT_EQ(Unorm8Byte(0.5f), 128);
    EXPECT_EQ(Unorm8Byte(1.0f), 255);
    EXPECT_EQ(Unorm8Byte(-0.25f), 0);
    EXPECT_EQ(Unorm8Byte(1.25f), 255);
}

TEST(ColorContract, DoubleEncodeDefect) {
    // The defect: the shader's gamma() re-encodes bytes that already were display-encoded.
    EXPECT_EQ(DoubleEncodedByte(24), 86);
    EXPECT_EQ(DoubleEncodedByte(28), 93);
    EXPECT_EQ(DoubleEncodedByte(37), 106);

    // The user's measured pixels, (24, 24, 16) and (37, 33, 23), byte by byte.
    constexpr std::array<std::uint8_t, 6> stored = {24, 24, 16, 37, 33, 23};
    constexpr std::array<std::uint8_t, 6> double_encoded = {86, 86, 71, 106, 101, 85};
    for (std::size_t index = 0; index < stored.size(); ++index) {
        EXPECT_EQ(DoubleEncodedByte(stored[index]), double_encoded[index])
            << "stored byte " << static_cast<int>(stored[index]);
    }

    // The endpoints are fixed (0 stays black, 255 saturates); every other byte moves.
    EXPECT_EQ(DoubleEncodedByte(0), 0);
    EXPECT_EQ(DoubleEncodedByte(255), 255);
    for (int byte = 1; byte <= 254; ++byte) {
        EXPECT_NE(DoubleEncodedByte(static_cast<std::uint8_t>(byte)), byte) << "byte " << byte;
    }
}

TEST(ColorContract, PlanMatrix) {
    constexpr std::array<ByteView, 3> views = {
        ByteView::Unavailable,
        ByteView::StoredBytes,
        ByteView::SrgbViewWithSibling,
    };
    int cases = 0;
    for (const ByteView view : views) {
        for (const bool target_srgb : {false, true}) {
            for (const bool gamma_one : {false, true}) {
                for (const bool hdr : {false, true}) {
                    for (const bool a_b_off : {false, true}) {
                        const Presentation plan = Plan(view, target_srgb, gamma_one, hdr, a_b_off);
                        const bool display_encoded =
                            view != ByteView::Unavailable && gamma_one && !hdr && !a_b_off;
                        SCOPED_TRACE(::testing::Message()
                                     << "view=" << static_cast<int>(view)
                                     << " target_srgb=" << target_srgb
                                     << " gamma_one=" << gamma_one << " hdr=" << hdr
                                     << " a_b_off=" << a_b_off);
                        EXPECT_EQ(plan.read_stored_bytes, display_encoded);
                        EXPECT_EQ(plan.encoded_in, display_encoded);
                        EXPECT_EQ(plan.linear_out, target_srgb && !hdr);
                        ++cases;
                    }
                }
            }
        }
    }
    EXPECT_EQ(cases, 48);
}

TEST(ColorContract, PresentedByteStoredSource) {
    constexpr std::array<ByteView, 2> views = {ByteView::StoredBytes,
                                               ByteView::SrgbViewWithSibling};
    for (const ByteView view : views) {
        const Presentation unorm = Plan(view, false, true, false, false);
        ASSERT_TRUE(unorm.read_stored_bytes);
        ASSERT_TRUE(unorm.encoded_in);
        ASSERT_FALSE(unorm.linear_out);
        for (int byte = 0; byte <= 255; ++byte) {
            const auto stored = static_cast<std::uint8_t>(byte);
            // A UNORM target stores exactly the bytes the guest handed over.
            EXPECT_EQ(PresentedByte(stored, false, unorm, false), byte) << "byte " << byte;
        }

        const Presentation srgb = Plan(view, true, true, false, false);
        ASSERT_TRUE(srgb.read_stored_bytes);
        ASSERT_TRUE(srgb.encoded_in);
        ASSERT_TRUE(srgb.linear_out);
        int worst_distance = 0;
        int worst_byte = -1;
        for (int byte = 0; byte <= 255; ++byte) {
            const auto stored = static_cast<std::uint8_t>(byte);
            const int distance = LsbDistance(PresentedByte(stored, false, srgb, true), stored);
            EXPECT_LE(distance, MaxPassThroughLsb) << "byte " << byte;
            if (distance > worst_distance) {
                worst_distance = distance;
                worst_byte = byte;
            }
        }
        EXPECT_LE(worst_distance, MaxPassThroughLsb)
            << "worst byte " << worst_byte << " differs by " << worst_distance << " LSB";
    }
}

TEST(ColorContract, PresentedByteHardwareDecodeSource) {
    // An sRGB view without a UNORM sibling: the hardware decodes, the shader re-encodes. The
    // round trip through the float curve must land on the same byte the picture held.
    const Presentation unorm = Plan(ByteView::Unavailable, false, true, false, false);
    ASSERT_FALSE(unorm.read_stored_bytes);
    ASSERT_FALSE(unorm.encoded_in);
    ASSERT_FALSE(unorm.linear_out);

    const Presentation srgb = Plan(ByteView::Unavailable, true, true, false, false);
    ASSERT_FALSE(srgb.read_stored_bytes);
    ASSERT_FALSE(srgb.encoded_in);
    ASSERT_TRUE(srgb.linear_out);

    for (int byte = 0; byte <= 255; ++byte) {
        const auto stored = static_cast<std::uint8_t>(byte);
        EXPECT_LE(LsbDistance(PresentedByte(stored, true, unorm, false), stored),
                  MaxPassThroughLsb)
            << "byte " << byte;
        EXPECT_LE(LsbDistance(PresentedByte(stored, true, srgb, true), stored), MaxPassThroughLsb)
            << "byte " << byte;
    }
}

TEST(ColorContract, PresentedByteLegacyDoublePath) {
    // The old pass treated display-encoded bytes as linear and encoded them again.
    constexpr Presentation legacy{false, false, false};
    for (int byte = 0; byte <= 255; ++byte) {
        const auto stored = static_cast<std::uint8_t>(byte);
        EXPECT_EQ(PresentedByte(stored, false, legacy, false), DoubleEncodedByte(stored))
            << "byte " << byte;
    }

    // The A/B aid asks for that behaviour by disabling the display-encoded contract.
    const Presentation a_b_off = Plan(ByteView::StoredBytes, false, true, false, true);
    EXPECT_FALSE(a_b_off.read_stored_bytes);
    EXPECT_FALSE(a_b_off.encoded_in);
    EXPECT_FALSE(a_b_off.linear_out);
    for (int byte = 0; byte <= 255; ++byte) {
        const auto stored = static_cast<std::uint8_t>(byte);
        EXPECT_EQ(PresentedByte(stored, false, a_b_off, false), DoubleEncodedByte(stored))
            << "byte " << byte;
    }
    EXPECT_EQ(PresentedByte(24, false, a_b_off, false), 86);
    EXPECT_EQ(PresentedByte(37, false, a_b_off, false), 106);
}

TEST(ColorContract, ShaderCurveConsistency) {
    // post_process.frag implements the same curve: the byte a display shows is the byte the
    // shader sampled, whether it is expressed linear or encoded.
    for (int byte = 0; byte <= 255; ++byte) {
        const float encoded = Unorm8Value(static_cast<std::uint8_t>(byte));
        EXPECT_NEAR(LinearToSrgb(SrgbToLinear(encoded)), encoded, ShaderCurveTolerance)
            << "byte " << byte;
    }
}

} // namespace
} // namespace VideoCore::Color
