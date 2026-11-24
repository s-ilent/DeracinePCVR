// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Reference-image comparison.
//
// A rendered image that differs from the captured reference is a rendering mismatch, not a
// successful replay. This module decides which, and reports enough for a reader to tell a shader
// problem from a resource problem from a configuration problem.
//
// The checks are ordered so the cheapest and most decisive come first: a reference of the wrong
// extent cannot be compared at all, and comparing it anyway would produce a pixel count that looks
// like a rendering failure when it is a configuration failure.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "package_reader.h"

namespace reference {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// What the comparison established.
enum class Status : u8 {
    /// Every pixel matched exactly.
    ExactMatch = 0,
    /// The images are comparable and differ.
    PixelMismatch,
    /// The package carries no reference.
    MissingReference,
    /// The reference cannot be compared with this render: extent, format or sample count differs.
    IncompatibleReference,
    /// The reference configuration cannot be captured, such as a multisampled attachment.
    UnsupportedConfiguration,
    /// The capture did not complete, so the reference is not trustworthy.
    IncompleteCapture,
};

[[nodiscard]] const char* StatusName(Status status) noexcept;

/// The comparison result.
struct Comparison {
    Status status{Status::MissingReference};
    /// A precise explanation when the comparison could not be made.
    std::string reason;
    u32 width{};
    u32 height{};
    u32 bytes_per_pixel{};
    u64 compared_pixels{};
    u64 different_pixels{};
    /// The largest absolute difference in any channel of any pixel.
    u32 max_channel_error{};
    /// The mean absolute difference over every channel of every compared pixel.
    double mean_absolute_error{};
    bool exact_match{};
};

/// What the render produced, for comparison against the reference.
struct Rendered {
    u32 width{};
    u32 height{};
    u32 bytes_per_pixel{};
    /// The pixels, tightly packed, in the reference's channel order.
    std::vector<u8> pixels;
};

/// Compares a rendered image against a package's captured reference.
///
/// param require_pre  When true, a package without a pre-draw image is reported as an incomplete
///                    capture rather than compared. A draw whose output depends on what the target
///                    held before it cannot be verified without that image.
[[nodiscard]] Comparison Compare(const package::Package& package, const Rendered& rendered,
                                 bool require_pre = false);

} // namespace reference
