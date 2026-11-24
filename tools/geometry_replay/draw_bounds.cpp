// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "draw_bounds.h"

#include <algorithm>

namespace bounds {

Check CheckDraw(const DrawRange& range, const std::vector<u32>& index_values) noexcept {
    Check check{};

    // --- Index range ---------------------------------------------------------------------------
    // The draw reads [first_index, first_index + index_count). Reading past the end of the index
    // buffer is a fault in its own right, and with robust buffer access enabled it would return
    // zeros rather than faulting, so it must be refused here rather than left to the driver.
    const u64 first = range.first_index;
    const u64 count = range.index_count;
    if (count == 0) {
        check.reason = "the draw consumes no indices";
        return check;
    }
    if (first + count > range.index_elements) {
        check.reason = "the draw reads indices [" + std::to_string(first) + ", " +
                       std::to_string(first + count) + ") but the index buffer holds " +
                       std::to_string(range.index_elements) + " elements";
        return check;
    }
    if (first + count > index_values.size()) {
        check.reason = "the draw reads indices [" + std::to_string(first) + ", " +
                       std::to_string(first + count) + ") but only " +
                       std::to_string(index_values.size()) + " were decoded";
        return check;
    }

    // --- Vertex range --------------------------------------------------------------------------
    // Every fetched index, after the signed base-vertex offset, must address a vertex that exists.
    // The check uses the values the draw will actually fetch, not the allocation size: an index
    // buffer can be large and still contain a value past the end of the vertex stream.
    u32 max_index = 0;
    u32 min_index = 0xFFFFFFFFu;
    for (u64 i = first; i < first + count; ++i) {
        const u32 value = index_values[i];
        max_index = std::max(max_index, value);
        min_index = std::min(min_index, value);
    }
    check.max_index = max_index;

    const s64 max_vertex = static_cast<s64>(max_index) + static_cast<s64>(range.base_vertex);
    const s64 min_vertex = static_cast<s64>(min_index) + static_cast<s64>(range.base_vertex);
    check.max_vertex = max_vertex;
    check.min_vertex = min_vertex;

    if (min_vertex < 0) {
        check.reason = "the draw addresses vertex " + std::to_string(min_vertex) +
                       " after a base-vertex offset of " + std::to_string(range.base_vertex) +
                       ", which is before the start of the vertex stream";
        return check;
    }
    if (static_cast<u64>(max_vertex) >= range.vertex_count) {
        check.reason = "the draw addresses vertex " + std::to_string(max_vertex) +
                       " after a base-vertex offset of " + std::to_string(range.base_vertex) +
                       ", but the vertex stream holds " + std::to_string(range.vertex_count) +
                       " vertices";
        return check;
    }

    check.valid = true;
    return check;
}

} // namespace bounds
