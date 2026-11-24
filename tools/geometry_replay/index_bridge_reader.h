// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Reader for the persisted original index-resource bridge.
//
// The bridge cannot be reconstructed from the package: the package carries a guest address, and the
// whole point of the bridge is that it was independently observed from the original UE4 object. A
// replay that derived the bridge from the package would be comparing the package with itself.
//
// The sidecar is written by the emulator's identity observer and read here. Every field the
// matcher needs is present, with explicit presence flags, so an unrecovered value stays unknown
// rather than defaulting to zero.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bridge {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// The sidecar schema version. A reader refuses a version it does not understand.
inline constexpr u32 kSchemaVersion = 1;

/// The sidecar filename within a capture directory.
inline constexpr std::string_view kFileName = "index_bridge.json";

/// One persisted bridge record.
struct Record {
    u64 session_id{};
    bool has_session_id{};
    u64 execution_id{};
    bool has_execution_id{};
    u64 provenance_id{};

    u64 original_rhi_object{};
    bool has_original_rhi_object{};
    u64 resource_base{};
    bool has_resource_base{};

    /// Both address candidates, recorded separately so they are never conflated.
    u64 address_candidate_a{};
    bool has_address_candidate_a{};
    u64 address_candidate_b{};
    bool has_address_candidate_b{};

    /// The slot the original predicate selected, and the address in it.
    std::string selected_slot;
    u64 selected_guest_address{};
    bool has_selected_guest_address{};

    u32 index_element_width{};
    bool has_index_element_width{};

    /// Only present when genuinely recovered. Never invented.
    u64 allocation_size{};
    bool has_allocation_size{};
    u64 allocation_offset{};
    bool has_allocation_offset{};
    u64 resource_generation{};
    bool has_resource_generation{};

    /// Whether the predicate was determinable at capture time.
    bool predicate_determinable{};
    /// The evidence class of the selected address.
    std::string address_evidence;
};

/// Why a sidecar could not be read.
enum class Problem : u8 {
    None = 0,
    Missing,
    Unreadable,
    BadJson,
    SchemaMismatch,
    NoRecords,
    DuplicateRecord,
    TruncatedRecord,
};

[[nodiscard]] std::string_view ProblemName(Problem problem);

/// Reads the sidecar from a capture directory.
[[nodiscard]] bool Read(const std::string& directory, std::vector<Record>& out, Problem& problem,
                        std::string& error);

/// Finds the record for a provenance id. Returns false when there is none, or more than one.
[[nodiscard]] bool FindByProvenance(const std::vector<Record>& records, u64 provenance_id,
                                    Record& out, bool& ambiguous);

} // namespace bridge
