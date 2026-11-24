// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Bounds validation for an indexed draw against the resources it actually binds.
//
// A mutation test that silently reads past the end of a buffer proves nothing: the draw may still
// execute, and with robust buffer access enabled it will return zeros rather than faulting, so the
// test would appear to pass while exercising an invalid draw. This module refuses such a draw
// before any GPU work, so a mutation fixture is either valid or rejected.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bounds {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

/// What the draw will actually consume.
struct DrawRange {
    /// Index of the first element read from the index buffer.
    u32 first_index{};
    /// Number of indices read.
    u32 index_count{};
    /// Signed offset added to every fetched index.
    s32 base_vertex{};
    /// Elements available in the index buffer.
    u64 index_elements{};
    /// Vertices available in the bound vertex stream.
    u64 vertex_count{};
};

/// The result of checking a draw against its resources.
struct Check {
    bool valid{};
    /// A one-line explanation when the draw is out of bounds.
    std::string reason;
    /// The highest index value the draw would fetch, before the base-vertex offset.
    u32 max_index{};
    /// The highest vertex the draw would address, after the base-vertex offset.
    s64 max_vertex{};
    /// The lowest vertex the draw would address, after the base-vertex offset.
    s64 min_vertex{};
};

/// Checks that the draw reads only elements and vertices that exist.
///
/// The index range is checked first, because reading past the index buffer is a different fault
/// from addressing a vertex that does not exist. Both are refused.
///
/// param index_values  The index buffer's contents, so the vertex range can be computed from the
///                     values the draw will actually fetch rather than from the allocation size.
[[nodiscard]] Check CheckDraw(const DrawRange& range,
                              const std::vector<u32>& index_values) noexcept;

} // namespace bounds
