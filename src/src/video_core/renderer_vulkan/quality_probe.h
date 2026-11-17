// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <charconv>
#include <optional>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Vulkan::QualityDiag {

/// What a SHADPS4_QUALITY_PROBE entry reads.
enum class ProbeKind {
    U8,
    U32,
    F32,
    PointerToF32,
};

struct ProbeSpec {
    ProbeKind kind{};
    VAddr address{};
};

/// Parses `type@address` entries separated by ';', the address in guest virtual address space
/// with or without a 0x prefix. Anything else is skipped: a probe list a run cannot use must
/// not stop the run.
inline std::vector<ProbeSpec> ParseProbeList(std::string_view list) {
    std::vector<ProbeSpec> result;
    while (!list.empty()) {
        const auto end = list.find(';');
        std::string_view item = list.substr(0, end);
        list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
        const auto at = item.find('@');
        if (at == std::string_view::npos) {
            continue;
        }
        const std::string_view kind = item.substr(0, at);
        std::string_view text = item.substr(at + 1);
        if (text.starts_with("0x") || text.starts_with("0X")) {
            text.remove_prefix(2);
        }
        u64 address{};
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), address, 16);
        if (ec != std::errc{} || ptr != text.data() + text.size() || address == 0) {
            continue;
        }
        std::optional<ProbeKind> parsed;
        if (kind == "u8") {
            parsed = ProbeKind::U8;
        } else if (kind == "u32") {
            parsed = ProbeKind::U32;
        } else if (kind == "f32") {
            parsed = ProbeKind::F32;
        } else if (kind == "ptr_f32") {
            parsed = ProbeKind::PointerToF32;
        }
        if (parsed) {
            result.push_back({*parsed, static_cast<VAddr>(address)});
        }
    }
    return result;
}

} // namespace Vulkan::QualityDiag
