// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Same-draw identity between a semantic tape and a resource package.
//
// The two captures are produced by different subsystems and describe different things: the tape
// describes what UE4 asked its RHI to draw, and the package describes what the renderer actually
// bound. Both are valid on their own. Joining them because both are valid would be the defect this
// module exists to prevent.
//
// The join is therefore explicit and evidence-based. Each field is compared in its own identity
// domain, and a field that cannot be compared is reported rather than assumed equal.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tape {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

/// One field of the join, and what comparing it established.
struct IdentityField {
    /// What the field is, in the vocabulary of the domain it belongs to.
    std::string_view name;
    /// The value the tape carries, rendered for the report.
    std::string tape_value;
    /// The value the package carries, rendered for the report.
    std::string package_value;
    /// Whether the two agree.
    bool agrees{};
    /// Whether the comparison was possible at all. A field the package does not carry cannot be
    /// compared, and reporting it as agreeing would be a fabricated match.
    bool comparable{};
};

/// Why a tape and a package could not be joined.
enum class JoinOutcome : u8 {
    /// Every comparable field agrees and at least one identity field was compared.
    Joined = 0,
    /// The two describe different draws.
    Mismatched = 1,
    /// The two might describe the same draw, but no field establishes it.
    InsufficientEvidence = 2,
    /// A field that must agree for the join to be meaningful is absent from one side.
    MissingIdentity = 3,
};

[[nodiscard]] const char* JoinOutcomeName(JoinOutcome outcome) noexcept;

/// The result of attempting the join.
struct JoinResult {
    JoinOutcome outcome{JoinOutcome::InsufficientEvidence};
    /// Every field that was examined, in a stable order, for the report.
    std::vector<IdentityField> fields;
    /// A one-line explanation when the join did not succeed.
    std::string reason;
};

/// The identity facts a tape contributes.
struct TapeIdentity {
    u64 provenance_id{};
    /// The guest address of the index buffer the RHI invocation was given.
    u64 index_guest_base{};
    u64 index_guest_size{};
    /// The index element width the tape resolved, or zero when it did not.
    u32 index_element_size{};
    /// The semantic payload's own fields.
    u32 primitive_type{};
    u32 num_primitives{};
    u32 num_instances{};
    u32 num_vertices{};
    u32 start_index{};
    s32 base_vertex_index{};
    u32 first_instance{};
    /// The derived index count the tape will execute.
    u32 derived_index_count{};
};

/// The identity facts a package contributes.
struct PackageIdentity {
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
    /// Whether the package carries an index binding at all.
    bool has_index_binding{};
};

/// Attempts the join.
///
/// The comparison is deliberately layered, because the fields are not equally strong:
///
/// 1. **Provenance.** Both captures record the provenance id of the draw they observed. This is the
///    strongest available identity: it is the same record id from the same execution, so a
///    disagreement means the two captures are about different draws and the join fails outright.
/// 2. **Index resource.** The guest address and size of the index buffer. The tape reads it from
///    the RHI invocation argument and the package from the production binding, so agreement means
///    both observed the same resource rather than merely a same-sized one.
/// 3. **Element width.** The tape resolves it from the resource and the package from the bound
///    index buffer. A disagreement means one of them is describing a different buffer.
/// 4. **Derived draw.** The index count, first index, base vertex, instance count and first
///    instance. These are compared as a consistency check, not as identity: two different draws
///    can share them, so agreement alone never establishes the join.
///
/// A field the package does not carry is reported as not comparable rather than as agreeing.
[[nodiscard]] JoinResult JoinTapeToPackage(const TapeIdentity& tape_identity,
                                           const PackageIdentity& package_identity) noexcept;

} // namespace tape
