// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Single-execution capture selection.
//
// The previous design captured a resource package in one game launch, read its provenance id, then
// launched the game again to export a tape pinned to that id. That is unsound: a provenance id is a
// per-execution record ordinal, so the same id in a second process names a different draw.
//
// This module replaces it. Both observers run in one execution, every artifact carries a shared
// session id, and the compatible pair is selected during final export or offline postprocessing.
//
// The selection is not a match. A shared provenance id is a candidate lookup, not proof of resource
// identity; the original index-resource bridge is what establishes that, and a candidate whose
// bridge does not resolve is reported as a failure rather than silently replaced by another draw.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Core::CaptureSession {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// Opt-in for writing the session manifest. Default OFF.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_CAPTURE_SESSION";

/// The output directory for the manifest. Required when the session is enabled.
inline constexpr std::string_view DirectoryEnvironmentVariable =
    "SHADPS4_DERACINE_CAPTURE_SESSION_DIR";

/// One candidate the semantic observer recorded.
struct SemanticCandidate {
    u64 provenance_id{};
    u64 command_capture_id{};
    u64 command_list{};
    u64 index_resource{};
    u32 primitive_type{};
    u32 num_primitives{};
    u32 num_instances{};
    u32 num_vertices{};
    u32 start_index{};
    /// Whether the original index-resource bridge resolved for this candidate.
    bool bridge_resolved{};
    u64 resolved_guest_base{};
};

/// One candidate the resource observer recorded.
struct ResourceCandidate {
    u64 provenance_id{};
    u64 index_guest_base{};
    u64 index_guest_size{};
    u32 index_element_size{};
    u32 index_count{};
    u32 first_index{};
    u32 base_vertex{};
    u32 instance_count{};
    u32 first_instance{};
    u32 primitive_type{};
    /// Whether the package was written for this candidate.
    bool package_written{};
};

/// Why a pair could not be selected.
enum class SelectionOutcome : u8 {
    /// Exactly one compatible pair was found and its bridge resolved.
    Selected = 0,
    /// No semantic candidate shares a provenance id with a resource candidate.
    NoSharedProvenance = 1,
    /// A shared provenance id exists but the bridge did not resolve.
    BridgeUnresolved = 2,
    /// More than one pair is compatible, so the choice is not determined.
    Ambiguous = 3,
    /// The two sides were captured in different executions.
    DifferentExecution = 4,
    /// Nothing was captured.
    Empty = 5,
};

[[nodiscard]] constexpr std::string_view SelectionOutcomeName(SelectionOutcome outcome) noexcept {
    switch (outcome) {
    case SelectionOutcome::Selected:
        return "selected";
    case SelectionOutcome::NoSharedProvenance:
        return "no_shared_provenance";
    case SelectionOutcome::BridgeUnresolved:
        return "bridge_unresolved";
    case SelectionOutcome::Ambiguous:
        return "ambiguous";
    case SelectionOutcome::DifferentExecution:
        return "different_execution";
    case SelectionOutcome::Empty:
        return "empty";
    default:
        return "unknown";
    }
}

/// The result of selecting a pair.
struct Selection {
    SelectionOutcome outcome{SelectionOutcome::Empty};
    u64 provenance_id{};
    /// The reason, when the selection did not succeed.
    std::string reason;
};

/// Selects the compatible pair from one execution.
///
/// A shared provenance id is the candidate lookup. It is not proof of resource identity: the
/// semantic candidate must also have a resolved bridge, and the resolved guest base must agree with
/// the resource candidate. A candidate that fails either test is reported rather than replaced.
[[nodiscard]] inline Selection SelectPair(
    u64 semantic_execution_id, const std::vector<SemanticCandidate>& semantic,
    u64 resource_execution_id, const std::vector<ResourceCandidate>& resource) noexcept {
    Selection selection{};

    if (semantic.empty() || resource.empty()) {
        selection.outcome = SelectionOutcome::Empty;
        selection.reason = "one side captured nothing";
        return selection;
    }

    // A different execution is decisive. The same provenance id in two processes names different
    // draws, so no field can repair it.
    if (semantic_execution_id != 0 && resource_execution_id != 0 &&
        semantic_execution_id != resource_execution_id) {
        selection.outcome = SelectionOutcome::DifferentExecution;
        selection.reason = "the semantic and resource captures are from different executions";
        return selection;
    }

    std::vector<u64> compatible;
    bool saw_shared = false;
    bool saw_unresolved_bridge = false;

    for (const auto& semantic_candidate : semantic) {
        for (const auto& resource_candidate : resource) {
            if (semantic_candidate.provenance_id != resource_candidate.provenance_id) {
                continue;
            }
            saw_shared = true;
            // The bridge is what establishes resource identity. A candidate without it is not a
            // match, however well the provenance id agrees.
            if (!semantic_candidate.bridge_resolved) {
                saw_unresolved_bridge = true;
                continue;
            }
            if (semantic_candidate.resolved_guest_base != resource_candidate.index_guest_base) {
                continue;
            }
            compatible.push_back(semantic_candidate.provenance_id);
        }
    }

    if (compatible.size() == 1) {
        selection.outcome = SelectionOutcome::Selected;
        selection.provenance_id = compatible.front();
        return selection;
    }
    if (compatible.size() > 1) {
        selection.outcome = SelectionOutcome::Ambiguous;
        selection.reason = "more than one compatible pair was found";
        return selection;
    }
    if (saw_unresolved_bridge) {
        selection.outcome = SelectionOutcome::BridgeUnresolved;
        selection.reason =
            "a shared provenance id exists but the original index-resource bridge did not "
            "resolve, so resource identity is not established";
        return selection;
    }
    if (saw_shared) {
        selection.outcome = SelectionOutcome::BridgeUnresolved;
        selection.reason =
            "a shared provenance id exists but the resolved allocation disagrees with the "
            "package";
        return selection;
    }

    selection.outcome = SelectionOutcome::NoSharedProvenance;
    selection.reason = "no semantic candidate shares a provenance id with a resource candidate";
    return selection;
}

} // namespace Core::CaptureSession
