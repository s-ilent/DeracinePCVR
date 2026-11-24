// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "reference_compare.h"

#include <cmath>
#include <cstdlib>

namespace reference {

const char* StatusName(Status status) noexcept {
    switch (status) {
    case Status::ExactMatch:
        return "exact_match";
    case Status::PixelMismatch:
        return "pixel_mismatch";
    case Status::MissingReference:
        return "missing_reference";
    case Status::IncompatibleReference:
        return "incompatible_reference";
    case Status::UnsupportedConfiguration:
        return "unsupported_configuration";
    case Status::IncompleteCapture:
        return "incomplete_capture";
    }
    return "unknown";
}

Comparison Compare(const package::Package& package, const Rendered& rendered, bool require_pre) {
    Comparison out{};
    const package::ReferenceOutput& captured = package.reference;

    // --- Presence ------------------------------------------------------------------------------
    if (captured.width == 0 || captured.height == 0 || captured.bytes_per_pixel == 0) {
        out.status = Status::MissingReference;
        out.reason = "the package carries no reference extent";
        return out;
    }
    if (captured.post_bytes.empty()) {
        out.status = Status::IncompleteCapture;
        out.reason = "the package declares a reference but carries no post-draw bytes";
        return out;
    }

    // A draw whose output depends on what the target held before it cannot be verified without the
    // pre-draw image. Treating a missing one as a cleared target would compare against a state the
    // draw never saw.
    if (require_pre && !captured.has_pre) {
        out.status = Status::IncompleteCapture;
        out.reason = "the draw depends on prior attachment contents, but the package carries no "
                     "pre-draw image";
        return out;
    }

    // --- Configuration -------------------------------------------------------------------------
    // A multisampled attachment cannot be read as pixels without a resolve, so a reference of one
    // is not comparable rather than merely different.
    if (package.pipeline_state.samples > 1) {
        out.status = Status::UnsupportedConfiguration;
        out.reason = "the captured pipeline used " +
                     std::to_string(package.pipeline_state.samples) +
                     " samples, and a multisampled reference cannot be compared without a resolve";
        return out;
    }

    out.width = captured.width;
    out.height = captured.height;
    out.bytes_per_pixel = captured.bytes_per_pixel;

    if (rendered.width != captured.width || rendered.height != captured.height) {
        out.status = Status::IncompatibleReference;
        out.reason = "the render is " + std::to_string(rendered.width) + "x" +
                     std::to_string(rendered.height) + " but the reference is " +
                     std::to_string(captured.width) + "x" + std::to_string(captured.height);
        return out;
    }
    if (rendered.bytes_per_pixel != captured.bytes_per_pixel) {
        out.status = Status::IncompatibleReference;
        out.reason = "the render is " + std::to_string(rendered.bytes_per_pixel) +
                     " bytes per pixel but the reference is " +
                     std::to_string(captured.bytes_per_pixel);
        return out;
    }

    const u64 expected = static_cast<u64>(captured.width) * captured.height *
                         captured.bytes_per_pixel;
    if (captured.post_bytes.size() != expected) {
        out.status = Status::IncompatibleReference;
        out.reason = "the reference holds " + std::to_string(captured.post_bytes.size()) +
                     " bytes but its extent and pixel size require " + std::to_string(expected);
        return out;
    }
    if (rendered.pixels.size() != expected) {
        out.status = Status::IncompatibleReference;
        out.reason = "the render holds " + std::to_string(rendered.pixels.size()) +
                     " bytes but its extent and pixel size require " + std::to_string(expected);
        return out;
    }

    // --- Comparison ----------------------------------------------------------------------------
    out.compared_pixels = static_cast<u64>(captured.width) * captured.height;
    u64 total_error = 0;
    for (u64 i = 0; i < out.compared_pixels; ++i) {
        const u64 base = i * captured.bytes_per_pixel;
        bool differs = false;
        for (u32 c = 0; c < captured.bytes_per_pixel; ++c) {
            const u32 a = rendered.pixels[base + c];
            const u32 b = captured.post_bytes[base + c];
            const u32 delta = a > b ? a - b : b - a;
            if (delta != 0) {
                differs = true;
                total_error += delta;
                if (delta > out.max_channel_error) {
                    out.max_channel_error = delta;
                }
            }
        }
        if (differs) {
            ++out.different_pixels;
        }
    }

    out.mean_absolute_error =
        out.compared_pixels == 0
            ? 0.0
            : static_cast<double>(total_error) /
                  (static_cast<double>(out.compared_pixels) * captured.bytes_per_pixel);
    out.exact_match = out.different_pixels == 0;
    out.status = out.exact_match ? Status::ExactMatch : Status::PixelMismatch;
    if (!out.exact_match) {
        out.reason = std::to_string(out.different_pixels) + " of " +
                     std::to_string(out.compared_pixels) + " pixels differ";
    }
    return out;
}

} // namespace reference
