// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Structural readers for the capture artifacts the finalizer consumes.
//
// The previous finalizer searched the serialized package for the literal text "guest_base" and took
// the first match. The package writes `vertex_bindings[]` before `index_binding`, and both carry a
// field with that name, so the search returned a vertex-buffer address rather than the index-buffer
// address. That is a silent wrong answer, not a parse failure.
//
// These readers parse the JSON structurally: they locate the named object, read the named field from
// within it, and distinguish a missing object, a missing field, a zero value, a wrong type and a
// malformed number. A duplicate identity-critical key is rejected rather than resolved by position.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Core::CaptureArtifacts {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// Why a field could not be read.
enum class FieldProblem : u8 {
    None = 0,
    /// The file could not be opened or is not valid JSON.
    Unreadable,
    /// The containing object is absent.
    MissingObject,
    /// The object is present but the field is absent.
    MissingField,
    /// The field is present but is not a number.
    WrongType,
    /// The field is a number but is negative, fractional or out of range.
    MalformedNumber,
    /// The identity-critical key appears more than once, so its value is ambiguous.
    DuplicateKey,
    /// The artifact schema is not one this reader understands.
    SchemaMismatch,
};

[[nodiscard]] std::string_view FieldProblemName(FieldProblem problem) noexcept;

/// The index-binding facts the finalizer needs.
struct PackageIndexBinding {
    /// Whether the package carries an index binding at all.
    bool present{};
    /// The guest GPU address of the index buffer. Zero is a legitimate value, so presence is
    /// tracked separately.
    u64 guest_base{};
    bool has_guest_base{};
    u64 guest_size{};
    bool has_guest_size{};
    u32 index_size{};
    bool has_index_size{};
    u32 index_count{};
    bool has_index_count{};
};

/// The package facts the finalizer needs.
struct PackageMetadata {
    u32 schema_version{};
    u64 provenance_id{};
    bool has_provenance_id{};
    u64 session_id{};
    bool has_session_id{};
    PackageIndexBinding index;
    /// The first vertex binding's guest base, kept only so a test can prove the reader does not
    /// confuse it with the index address.
    u64 first_vertex_guest_base{};
    bool has_first_vertex_guest_base{};
};

/// Reads the package metadata structurally.
///
/// Returns false and sets `problem` when the file cannot be read or a required field is malformed.
/// A missing optional field is reported through its presence flag rather than as a failure.
[[nodiscard]] bool ReadPackageMetadata(const std::string& path, PackageMetadata& out,
                                       FieldProblem& problem, std::string& error);

/// One bridge record, as the finalizer needs it.
struct BridgeRecord {
    u64 provenance_id{};
    u64 session_id{};
    bool has_session_id{};
    u64 execution_id{};
    u64 original_rhi_object{};
    bool has_original_rhi_object{};
    u64 selected_guest_address{};
    bool has_selected_guest_address{};
    u32 index_element_width{};
    bool has_index_element_width{};
    u64 allocation_size{};
    bool has_allocation_size{};
    u64 allocation_offset{};
    bool has_allocation_offset{};
    u64 resource_generation{};
    bool has_resource_generation{};
    bool predicate_determinable{};
    std::string selected_slot;
    std::string address_evidence;
};

/// Reads the bridge sidecar structurally.
[[nodiscard]] bool ReadBridgeRecords(const std::string& path, std::vector<BridgeRecord>& out,
                                     FieldProblem& problem, std::string& error);

/// Selects the bridge record for a session and provenance.
///
/// The sidecar holds an array of records, so the first `provenance_id` in the file is not
/// necessarily the selected draw. A duplicate match is reported as ambiguous rather than resolved
/// by position.
[[nodiscard]] bool SelectBridgeRecord(const std::vector<BridgeRecord>& records, u64 session_id,
                                      u64 provenance_id, BridgeRecord& out, bool& ambiguous,
                                      std::string& error);

} // namespace Core::CaptureArtifacts
