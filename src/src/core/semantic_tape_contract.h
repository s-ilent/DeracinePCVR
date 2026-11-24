// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include "common/types.h"
#include "core/rhi_command_capture_contract.h"

namespace Core::SemanticTape {

/// Version of the semantic command tape. A reader refuses a version it does not understand rather
/// than reinterpreting fields whose meaning may have changed.
///
/// Version 1 is the first tape: one UE4-origin indexed draw with the state that precedes it.
/// Version 2 adds the capture session identity, so a tape can be shown to come from the same
/// execution as the resource package and the index bridge. A version 1 tape is refused in
/// verified-original mode because it carries no session provenance.
inline constexpr u32 SchemaVersion = 2;

/// Opt-in for writing a semantic command tape. Separate from every other capture flag, so a tape is
/// only produced when explicitly asked for. Only the exact value "1" enables it.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_SEMANTIC_TAPE";

/// Output directory for the tape. Required when the tape is enabled; the directory must exist.
inline constexpr std::string_view OutputDirectoryEnvironmentVariable =
    "SHADPS4_DERACINE_SEMANTIC_TAPE_DIR";

[[nodiscard]] inline bool IsOptedIn(const char* value) noexcept {
    return value != nullptr && std::string_view{value} == "1";
}

/// Optional explicit draw selection, by the provenance record id the draw was captured under. When
/// absent, the tape selects the draw that the draw package in the same run describes, so the tape
/// and the package are provably about the same execution.
inline constexpr std::string_view DrawSelectionEnvironmentVariable =
    "SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW";

/// The tape is a bounded record of one draw and its preceding state. A larger bound is refused
/// rather than clamped, so a typo cannot silently produce a truncated tape.
inline constexpr u32 MaxTapeOperations = 256;

/// Bounded resource table. This is one contract shared by both producers and by the reader: the
/// live C++ exporter, the offline Python builder and the standalone interpreter all enforce it.
///
/// The bound is 64 because the observed classroom draw needs 33 (one index buffer, one colour
/// target, one depth target and thirty vertex-stream references), and the widest draw the recovered
/// command family can describe needs at most 34 (index + depth + 8 colour targets + 8 UAVs + 16
/// vertex streams). 64 is roughly twice that worst case, so a real draw fits with headroom while a
/// corrupt or runaway capture still fails at a fixed, compile-time bound.
///
/// Exceeding it is a hard failure, never a silent truncation: a tape that dropped a resource would
/// still validate structurally while referencing an identity it no longer carries.
inline constexpr u32 MaxTapeResources = 64;

/// Bounded unresolved-dependency table. A tape reports what it could not recover; the bound keeps
/// that report a fixed array rather than an allocation.
inline constexpr u32 MaxTapeUnresolved = 16;

/// Backend-neutral operation vocabulary.
///
/// Only operations whose source and semantics are established are emitted as a typed operation.
/// Anything else is an Opaque record or an explicit unresolved dependency, never a guess.
enum class OpKind : u8 {
    BeginFrame = 1,
    SetViewport = 2,
    SetRenderTargets = 3,
    BindVertexBuffers = 4,
    BindIndexBuffer = 5,
    SetGraphicsPipeline = 6,
    BindShaderResources = 7,
    DrawIndexed = 8,
    EndFrame = 9,
    /// A captured command whose semantics are not established. Carried so the tape is honest about
    /// what it saw rather than silently dropping it.
    Opaque = 10,
};

/// How strongly an operation's meaning is supported. Every operation carries one, so a reader can
/// tell a verified fact from a tentative attribution without reading the report.
enum class Evidence : u8 {
    /// Established from the target binary: disassembly, payload offsets, call contract.
    VerifiedTargetBinary = 1,
    /// Matches the pinned public UE4.18.3 source, but not independently confirmed in this build.
    PublicSourceCorrespondence = 2,
    /// Observed executing at runtime with the expected arguments.
    RuntimeConfirmed = 3,
    /// Plausible by shape only. Never used to claim semantic completeness.
    Tentative = 4,
};

/// Where a resource identity came from. This is the distinction the phase exists to make: a command
/// recovered from UE4 objects is not the same claim as a resource recovered through the downstream
/// execution oracle.
enum class ResourceProvenance : u8 {
    /// The identity was read from the UE4 command payload or the RHI context.
    Ue4Object = 1,
    /// The identity was recovered from the downstream GNM/PM4 execution.
    DownstreamOracle = 2,
    /// Referenced but not resolved.
    Unresolved = 3,
};

/// What a resource is used as. Kept separate from provenance so the two never get conflated.
enum class ResourceKind : u8 {
    IndexBuffer = 1,
    VertexBuffer = 2,
    RenderTarget = 3,
    DepthTarget = 4,
    ShaderResource = 5,
    Unknown = 6,
};

/// The original UE4 semantic payload of FRHICommandDrawIndexedPrimitive.
///
/// These are the values UE4 asked its RHI to draw, captured at the deferred command's Execute
/// wrapper before any GNM command was generated. They are NOT the GNM index count and hardware
/// address, and the conversion between the two is explicit and tested.
struct DrawIndexedPayload {
    /// Backend-neutral resource id of the bound index buffer.
    u32 index_buffer_resource{};
    /// UE4 EPrimitiveType, verbatim from the command payload.
    u32 primitive_type{};
    /// Signed base vertex index, verbatim.
    s32 base_vertex_index{};
    /// First instance, verbatim.
    u32 first_instance{};
    /// NumVertices, verbatim.
    u32 num_vertices{};
    /// StartIndex, verbatim. An index offset, not a byte offset.
    u32 start_index{};
    /// NumPrimitives, verbatim.
    u32 num_primitives{};
    /// NumInstances, verbatim.
    u32 num_instances{};
};

/// UE4 EPrimitiveType values this tape can convert. Anything else is refused rather than guessed.
///
/// These are the UE4 enum's own values, not the AMD hardware topology. UE4's EPrimitiveType begins
/// PT_TriangleList = 0, PT_TriangleStrip = 1; the GPU's VGT topology numbers triangle list as 4.
/// SC-07R established that conflating the two is a real defect, and the captured classroom draw
/// proves the distinction: its payload carries 0, and 1012 primitives x 3 = 3036 indices, which is
/// exactly the index count the downstream GNM draw reported. Treating 0 as an unknown topology
/// would have refused a draw whose conversion is in fact confirmed.
inline constexpr u32 PrimitiveTypeTriangleList = 0;
inline constexpr u32 PrimitiveTypeTriangleStrip = 1;

/// Indices consumed by one primitive of the given topology, or 0 when the topology is not one this
/// tape converts.
[[nodiscard]] constexpr u32 IndicesPerPrimitive(u32 primitive_type) noexcept {
    switch (primitive_type) {
    case PrimitiveTypeTriangleList:
        return 3;
    case PrimitiveTypeTriangleStrip:
        // A strip of N primitives consumes N + 2 indices.
        return 1;
    default:
        return 0;
    }
}

/// The derived values a backend needs, computed from the semantic payload.
///
/// The conversion is deliberately explicit and lives in one place so it can be tested against the
/// downstream oracle rather than re-derived at each call site.
struct DerivedDraw {
    /// Indices the draw consumes.
    u32 index_count{};
    /// Index offset into the bound index buffer. Equal to StartIndex: the PS4 RHI multiplies it by
    /// the element size when it forms the hardware address, so it is not folded into the base.
    u32 first_index{};
    /// Vertex offset applied to every fetched index.
    s32 vertex_offset{};
    /// Instances to draw.
    u32 instance_count{};
    /// Byte offset into the index buffer, once the element width is known.
    u64 index_byte_offset{};
    /// False when the topology or the element width made the conversion impossible.
    bool valid{};
    /// Why the conversion failed, when it did.
    std::string_view reason;
};

/// Converts the semantic payload into the values a backend consumes.
///
/// param index_element_size 2 or 4. A zero or other value makes the conversion invalid, because
///        the byte offset would be a guess.
[[nodiscard]] inline DerivedDraw DeriveDraw(const DrawIndexedPayload& payload,
                                           u32 index_element_size) noexcept {
    DerivedDraw out{};
    if (index_element_size != 2 && index_element_size != 4) {
        out.reason = "index element width is neither 2 nor 4";
        return out;
    }
    const u32 per_primitive = IndicesPerPrimitive(payload.primitive_type);
    if (per_primitive == 0) {
        out.reason = "primitive topology is not converted by this tape";
        return out;
    }
    if (payload.primitive_type == PrimitiveTypeTriangleStrip) {
        if (payload.num_primitives == 0) {
            out.reason = "a triangle strip with no primitives consumes no indices";
            return out;
        }
        out.index_count = payload.num_primitives + 2;
    } else {
        // Guard the multiply: a corrupt payload must not wrap into a small count.
        if (payload.num_primitives > 0xFFFFFFFFu / per_primitive) {
            out.reason = "primitive count overflows the index count";
            return out;
        }
        out.index_count = payload.num_primitives * per_primitive;
    }
    out.first_index = payload.start_index;
    out.vertex_offset = payload.base_vertex_index;
    out.instance_count = payload.num_instances;
    out.index_byte_offset = static_cast<u64>(payload.start_index) * index_element_size;
    out.valid = true;
    return out;
}

/// The identity a provenance record carries for the draw it observed.
///
/// These are the fields the RHI entry recorded about the invocation, and they are the only fields
/// that may be used to find the deferred command that produced it.
struct DrawIdentity {
    /// The command list the draw was issued on. Read from the command list at the wrapper entry, so
    /// it is the list the command object belonged to.
    u64 command_list{};
    /// The index-buffer resource the RHI invocation was given. This is the RHI entry argument, so
    /// it is the resource the draw actually consumed.
    u64 index_resource{};
    /// UE4 EPrimitiveType from the command payload, when the provenance record carried it.
    u32 primitive_type{};
    /// NumPrimitives from the command payload, when available.
    u32 num_primitives{};
    /// NumInstances from the command payload, when available.
    u32 num_instances{};
    /// NumVertices from the command payload, when available.
    u32 num_vertices{};
    /// StartIndex from the command payload, when available.
    u32 start_index{};

    /// Which of the semantic fields above the record actually carried.
    ///
    /// A zero value is not a sentinel for "absent": UE4's `PT_TriangleList` is zero, and a draw
    /// with `StartIndex` zero or `NumInstances` zero is a real draw. Comparing a field only when it
    /// is non-zero therefore skips the comparison for exactly the values a triangle-list draw
    /// usually has, and a decoy with a different primitive type would match.
    ///
    /// Presence is recorded explicitly instead, so a field that was carried is always compared and
    /// a field that was not is never treated as agreeing.
    struct Present {
        bool primitive_type : 1;
        bool num_primitives : 1;
        bool num_instances : 1;
        bool num_vertices : 1;
        bool start_index : 1;
    } present{};
};

/// Why a draw could not be resolved to exactly one deferred command.
enum class DrawMatchOutcome : u8 {
    Matched = 0,
    /// No captured draw command shares the identity.
    NoCandidate = 1,
    /// More than one captured draw command shares the identity, and the semantic fields did not
    /// separate them. Refused rather than resolved by taking the first.
    Ambiguous = 2,
    /// The identity itself is unusable: a zero command list or a zero index resource cannot
    /// identify anything, so matching on it would select an arbitrary command.
    UnusableIdentity = 3,
};

[[nodiscard]] constexpr std::string_view DrawMatchOutcomeName(DrawMatchOutcome outcome) noexcept {
    switch (outcome) {
    case DrawMatchOutcome::Matched:
        return "matched";
    case DrawMatchOutcome::NoCandidate:
        return "no_candidate";
    case DrawMatchOutcome::Ambiguous:
        return "ambiguous";
    case DrawMatchOutcome::UnusableIdentity:
        return "unusable_identity";
    default:
        return "unknown";
    }
}

/// One captured draw command, reduced to the fields the matcher needs.
///
/// This is a plain value rather than the capture record so the matcher can be exercised against
/// synthetic fixtures without a live capture, and so the live exporter and the tests call the same
/// function.
struct CapturedDrawCandidate {
    /// Index into the capture table, so the caller can retrieve the full record.
    u32 capture_index{};
    u64 command_list{};
    u64 index_resource{};
    u32 primitive_type{};
    u32 num_primitives{};
    u32 num_instances{};
    u32 num_vertices{};
    u32 start_index{};
};

/// Resolves a provenance identity to exactly one captured draw command.
///
/// This is the production matcher: the live exporter calls it, and the tests exercise the same
/// function. It deliberately does NOT use the capture-slot sequence number or the deferred-command
/// ordinal. Those are two different counters: the slot ordinal is assigned as index + 1 when a
/// command is recorded, while the deferred ordinal is a per-thread counter incremented at wrapper
/// entry. Comparing them across domains selects the wrong command.
///
/// Matching proceeds in two stages:
///
///   1. Identity: command list and index resource must both agree. A candidate that fails either is
///      not this draw.
///   2. Disambiguation: when more than one candidate survives stage 1, the semantic payload fields
///      that the provenance record independently carries are compared. A candidate that disagrees
///      on any field the record knows is excluded.
///
/// If more than one candidate still survives, the result is Ambiguous. Taking the first would be a
/// guess, and a wrong draw is worse than a refused one.
[[nodiscard]] inline DrawMatchOutcome MatchDrawCommand(
    std::span<const CapturedDrawCandidate> candidates, const DrawIdentity& identity,
    u32& out_capture_index) noexcept {
    out_capture_index = 0;
    if (identity.command_list == 0 || identity.index_resource == 0) {
        return DrawMatchOutcome::UnusableIdentity;
    }

    std::size_t found = 0;
    std::size_t found_count = 0;
    for (const auto& candidate : candidates) {
        if (candidate.command_list != identity.command_list) {
            continue;
        }
        if (candidate.index_resource != identity.index_resource) {
            continue;
        }
        // Stage 2: the semantic fields the record actually carried must agree. Presence is
        // explicit, because a zero value is a real value here: `PT_TriangleList` is zero, and a
        // draw with `StartIndex` zero is ordinary. Testing `!= 0` would skip the comparison for
        // exactly those draws and let a decoy with a different primitive type match.
        if (identity.present.primitive_type &&
            candidate.primitive_type != identity.primitive_type) {
            continue;
        }
        if (identity.present.num_primitives &&
            candidate.num_primitives != identity.num_primitives) {
            continue;
        }
        if (identity.present.num_instances &&
            candidate.num_instances != identity.num_instances) {
            continue;
        }
        if (identity.present.num_vertices && candidate.num_vertices != identity.num_vertices) {
            continue;
        }
        if (identity.present.start_index && candidate.start_index != identity.start_index) {
            continue;
        }
        found = candidate.capture_index;
        ++found_count;
    }

    if (found_count == 1) {
        out_capture_index = static_cast<u32>(found);
        return DrawMatchOutcome::Matched;
    }
    if (found_count > 1) {
        return DrawMatchOutcome::Ambiguous;
    }
    return DrawMatchOutcome::NoCandidate;
}

/// One operation on the tape.
struct Operation {
    OpKind kind{OpKind::Opaque};
    Evidence evidence{Evidence::Tentative};
    /// Capture sequence of the command that produced this operation, for ordering.
    u64 sequence{};
    /// The deferred command's capture id, when the operation came from one.
    u64 command_capture_id{};
    /// The command list the operation was recorded on.
    u64 command_list{};
    /// The RHI context the operation was issued against, when known.
    u64 rhi_context{};

    // SetViewport
    float viewport_min_x{};
    float viewport_min_y{};
    float viewport_min_z{};
    float viewport_max_x{};
    float viewport_max_y{};
    float viewport_max_z{};

    // SetRenderTargets
    std::array<u32, 8> color_targets{};
    u32 color_target_count{};
    u32 depth_target{};

    // BindVertexBuffers / BindIndexBuffer
    u32 resource{};
    u32 slot{};

    // DrawIndexed
    DrawIndexedPayload draw{};
    DerivedDraw derived{};

    /// Why this operation is opaque or unresolved, when it is.
    std::string_view note;
};

/// One backend-neutral resource the tape references.
struct Resource {
    u32 id{};
    ResourceKind kind{ResourceKind::Unknown};
    ResourceProvenance provenance{ResourceProvenance::Unresolved};
    /// The resource id in the associated draw package, or 0 when the tape does not reference one.
    /// This is the bridge between command provenance and resource provenance, and it is recorded
    /// rather than assumed.
    u32 package_resource_ref{};
    /// Guest range the resource was described by. Informational: it is not a portable identity.
    u64 guest_base{};
    u64 guest_size{};
    /// Element width for an index buffer, or the stride for a vertex buffer.
    u32 element_size{};
};

/// A dependency the tape could not resolve. Reported rather than fabricated.
struct UnresolvedDependency {
    /// What is missing, in the tape's own vocabulary.
    std::string_view what;
    /// Which operation or object owns the missing state.
    std::string_view owner;
    /// Why it could not be resolved.
    std::string_view reason;
};

/// The whole tape.
struct Tape {
    u32 schema_version{SchemaVersion};
    std::string_view scene;
    /// Provenance record id of the draw this tape describes.
    u64 provenance_id{};
    /// The capture session this tape was exported in. Read from the one CaptureSession identity, so
    /// every artifact of one execution carries the same value. Zero means the tape predates session
    /// provenance and cannot qualify as a verified original capture.
    u64 session_id{};
    /// The exact executable the tape was captured from.
    std::string_view image_sha256;
    u64 module_base{};
    /// The submission identity of the draw's downstream execution, when correlated.
    u64 submission_id{};
    /// The packet offset within that submission. Zero is a valid offset.
    u64 packet_offset{};
    /// Whether the packet offset is known at all, so zero is not overloaded as "unknown".
    bool packet_offset_known{};

    std::array<Operation, MaxTapeOperations> operations{};
    u32 operation_count{};
    std::array<Resource, MaxTapeResources> resources{};
    u32 resource_count{};
    std::array<UnresolvedDependency, MaxTapeUnresolved> unresolved{};
    u32 unresolved_count{};
};

/// Appends an operation, refusing to exceed the bound. Returns false when the tape is full.
[[nodiscard]] inline bool AppendOperation(Tape& tape, const Operation& operation) noexcept {
    if (tape.operation_count >= MaxTapeOperations)
        return false;
    tape.operations[tape.operation_count++] = operation;
    return true;
}

/// Appends a resource, refusing to exceed the bound.
///
/// A caller must treat a false return as a hard failure. Returning a zero resource id instead would
/// produce a tape that still validates structurally while referencing an identity it does not
/// carry, which is worse than refusing the capture.
[[nodiscard]] inline bool AppendResource(Tape& tape, const Resource& resource) noexcept {
    if (tape.resource_count >= MaxTapeResources)
        return false;
    tape.resources[tape.resource_count++] = resource;
    return true;
}

/// Appends an unresolved dependency, refusing to exceed the bound.
[[nodiscard]] inline bool AppendUnresolved(Tape& tape, const UnresolvedDependency& dependency) noexcept {
    if (tape.unresolved_count >= tape.unresolved.size())
        return false;
    tape.unresolved[tape.unresolved_count++] = dependency;
    return true;
}

/// Finds a resource by id, or null.
[[nodiscard]] inline const Resource* FindResource(const Tape& tape, u32 id) noexcept {
    for (u32 i = 0; i < tape.resource_count; ++i) {
        if (tape.resources[i].id == id)
            return &tape.resources[i];
    }
    return nullptr;
}

/// Whether a stored derived draw agrees with the semantic payload it claims to come from.
///
/// The UE4 semantic payload is authoritative. The derived values are a convenience for a backend,
/// and a tape whose stored derived values contradict its own payload is inconsistent data. Executing
/// it would mean trusting a number that the tape itself disproves, so it is refused.
///
/// The comparison is exact on every field the derivation produces. A tape written by either
/// producer passes because both call DeriveDraw; a tape edited to change one side fails.
[[nodiscard]] inline bool DerivedMatchesPayload(const DrawIndexedPayload& payload,
                                                const DerivedDraw& stored,
                                                u32 index_element_size) noexcept {
    const DerivedDraw expected = DeriveDraw(payload, index_element_size);
    if (expected.valid != stored.valid) {
        return false;
    }
    if (!expected.valid) {
        // An invalid derivation carries no values to compare; agreement on invalidity is enough.
        return true;
    }
    return expected.index_count == stored.index_count &&
           expected.first_index == stored.first_index &&
           expected.vertex_offset == stored.vertex_offset &&
           expected.instance_count == stored.instance_count &&
           expected.index_byte_offset == stored.index_byte_offset;
}

/// Why a tape is not executable. Reported so a partial tape says what it is missing.
enum class TapeProblem : u8 {
    None = 0,
    SchemaMismatch = 1,
    NoDraw = 2,
    DrawNotDerivable = 3,
    IndexResourceMissing = 4,
    IndexResourceUnresolved = 5,
    NoViewport = 6,
    NoRenderTargets = 7,
    DuplicateResourceId = 8,
    DanglingResourceReference = 9,
    OperationOutOfOrder = 10,
    /// The stored derived draw contradicts the semantic payload it claims to come from.
    DerivedInconsistent = 11,
    /// The tape carries more resources than the shared capacity contract allows.
    ResourceCapacityExceeded = 12,
    /// The tape carries more operations than the shared capacity contract allows.
    OperationCapacityExceeded = 13,
    /// The tape carries no graphics-pipeline operation. The bound-shader-state command is not in
    /// the recovered command cluster, so this is currently always the case.
    NoGraphicsPipeline = 14,
};

[[nodiscard]] constexpr std::string_view TapeProblemName(TapeProblem problem) noexcept {
    switch (problem) {
    case TapeProblem::None:
        return "none";
    case TapeProblem::SchemaMismatch:
        return "schema_mismatch";
    case TapeProblem::NoDraw:
        return "no_draw";
    case TapeProblem::DrawNotDerivable:
        return "draw_not_derivable";
    case TapeProblem::IndexResourceMissing:
        return "index_resource_missing";
    case TapeProblem::IndexResourceUnresolved:
        return "index_resource_unresolved";
    case TapeProblem::NoViewport:
        return "no_viewport";
    case TapeProblem::NoRenderTargets:
        return "no_render_targets";
    case TapeProblem::DuplicateResourceId:
        return "duplicate_resource_id";
    case TapeProblem::DanglingResourceReference:
        return "dangling_resource_reference";
    case TapeProblem::OperationOutOfOrder:
        return "operation_out_of_order";
    case TapeProblem::DerivedInconsistent:
        return "derived_inconsistent";
    case TapeProblem::ResourceCapacityExceeded:
        return "resource_capacity_exceeded";
    case TapeProblem::OperationCapacityExceeded:
        return "operation_capacity_exceeded";
    case TapeProblem::NoGraphicsPipeline:
        return "no_graphics_pipeline";
    default:
        return "unknown";
    }
}

/// Structural validation, independent of whether the tape is complete enough to execute.
///
/// This is the production check: the exporter calls it before writing, and the tests exercise the
/// same function. It refuses a tape whose identities are inconsistent rather than trusting them.
[[nodiscard]] inline TapeProblem ValidateTape(const Tape& tape) noexcept {
    if (tape.schema_version != SchemaVersion)
        return TapeProblem::SchemaMismatch;

    // The capacity contract is enforced on read as well as on write. A tape that exceeds it cannot
    // have been produced by a conforming producer, so it is refused rather than partially read.
    if (tape.operation_count > MaxTapeOperations)
        return TapeProblem::OperationCapacityExceeded;
    if (tape.resource_count > MaxTapeResources)
        return TapeProblem::ResourceCapacityExceeded;
    if (tape.unresolved_count > MaxTapeUnresolved)
        return TapeProblem::ResourceCapacityExceeded;

    // Resource ids must be unique and non-zero, so a reference is unambiguous.
    for (u32 i = 0; i < tape.resource_count; ++i) {
        if (tape.resources[i].id == 0)
            return TapeProblem::DuplicateResourceId;
        for (u32 j = i + 1; j < tape.resource_count; ++j) {
            if (tape.resources[i].id == tape.resources[j].id)
                return TapeProblem::DuplicateResourceId;
        }
    }

    // Every resource reference must resolve.
    for (u32 i = 0; i < tape.operation_count; ++i) {
        const auto& operation = tape.operations[i];
        if (operation.kind == OpKind::DrawIndexed) {
            if (operation.draw.index_buffer_resource == 0)
                return TapeProblem::IndexResourceMissing;
            if (FindResource(tape, operation.draw.index_buffer_resource) == nullptr)
                return TapeProblem::DanglingResourceReference;
        }
        if (operation.kind == OpKind::BindIndexBuffer ||
            operation.kind == OpKind::BindVertexBuffers) {
            if (operation.resource != 0 && FindResource(tape, operation.resource) == nullptr)
                return TapeProblem::DanglingResourceReference;
        }
        if (operation.kind == OpKind::SetRenderTargets) {
            for (u32 t = 0; t < operation.color_target_count; ++t) {
                if (operation.color_targets[t] != 0 &&
                    FindResource(tape, operation.color_targets[t]) == nullptr)
                    return TapeProblem::DanglingResourceReference;
            }
            if (operation.depth_target != 0 &&
                FindResource(tape, operation.depth_target) == nullptr)
                return TapeProblem::DanglingResourceReference;
        }
    }

    // A draw must be preceded by its state. The tape is an ordered program, so an operation that
    // appears after the draw cannot be state the draw consumed.
    bool saw_draw = false;
    for (u32 i = 0; i < tape.operation_count; ++i) {
        if (tape.operations[i].kind == OpKind::DrawIndexed) {
            if (saw_draw)
                return TapeProblem::OperationOutOfOrder;
            saw_draw = true;
        }
    }
    if (!saw_draw)
        return TapeProblem::NoDraw;

    // The stored derived draw must agree with the semantic payload. The payload is authoritative, so
    // a tape whose derived values contradict it is inconsistent data and is refused here rather than
    // executed on a number the tape itself disproves.
    for (u32 i = 0; i < tape.operation_count; ++i) {
        const auto& operation = tape.operations[i];
        if (operation.kind != OpKind::DrawIndexed)
            continue;
        const Resource* index = FindResource(tape, operation.draw.index_buffer_resource);
        if (index == nullptr)
            continue; // reported as a dangling reference above
        if (!DerivedMatchesPayload(operation.draw, operation.derived, index->element_size))
            return TapeProblem::DerivedInconsistent;
    }

    return TapeProblem::None;
}

/// Whether the tape carries everything the interpreter needs to execute the draw.
///
/// This is deliberately stricter than ValidateTape: a structurally sound tape can still be missing
/// the viewport or the render targets, and that must be reported rather than defaulted.
[[nodiscard]] inline TapeProblem CheckExecutable(const Tape& tape) noexcept {
    const TapeProblem structural = ValidateTape(tape);
    if (structural != TapeProblem::None)
        return structural;

    bool saw_viewport = false;
    bool saw_render_targets = false;
    for (u32 i = 0; i < tape.operation_count; ++i) {
        const auto& operation = tape.operations[i];
        if (operation.kind == OpKind::DrawIndexed)
            break;
        if (operation.kind == OpKind::SetViewport)
            saw_viewport = true;
        if (operation.kind == OpKind::SetRenderTargets)
            saw_render_targets = true;
    }
    if (!saw_viewport)
        return TapeProblem::NoViewport;
    if (!saw_render_targets)
        return TapeProblem::NoRenderTargets;

    for (u32 i = 0; i < tape.operation_count; ++i) {
        const auto& operation = tape.operations[i];
        if (operation.kind != OpKind::DrawIndexed)
            continue;
        if (!operation.derived.valid)
            return TapeProblem::DrawNotDerivable;
        const Resource* index = FindResource(tape, operation.draw.index_buffer_resource);
        if (index == nullptr)
            return TapeProblem::IndexResourceMissing;
        if (index->provenance == ResourceProvenance::Unresolved)
            return TapeProblem::IndexResourceUnresolved;
    }
    return TapeProblem::None;
}

/// Every problem a tape has, not just the first one found.
///
/// CheckExecutable returns a single problem, which is the right shape for a caller that only needs
/// to know whether it can proceed. It is the wrong shape for a report: a tape missing its viewport
/// and its render targets and its pipeline has three problems, and reporting only the first hides
/// the other two. This collects them all so a diagnostic can state the full set.
struct TapeProblems {
    /// One bit per TapeProblem value. Bit 0 is unused because TapeProblem::None is not a problem.
    u32 mask{};

    [[nodiscard]] constexpr bool Has(TapeProblem problem) const noexcept {
        return problem != TapeProblem::None && (mask & (1u << static_cast<u32>(problem))) != 0;
    }
    [[nodiscard]] constexpr bool Any() const noexcept { return mask != 0; }
    [[nodiscard]] constexpr u32 Count() const noexcept {
        u32 count = 0;
        for (u32 bits = mask; bits != 0; bits &= bits - 1) {
            ++count;
        }
        return count;
    }
};

/// Collects every structural and executability problem the tape has.
///
/// This does not replace CheckExecutable: a caller that only needs a yes/no answer still uses that.
/// It exists so a report can name every missing dimension at once rather than stopping at the first.
[[nodiscard]] inline TapeProblems CollectProblems(const Tape& tape) noexcept {
    TapeProblems problems{};
    const auto add = [&problems](TapeProblem problem) {
        if (problem != TapeProblem::None) {
            problems.mask |= 1u << static_cast<u32>(problem);
        }
    };

    add(ValidateTape(tape));

    bool saw_viewport = false;
    bool saw_render_targets = false;
    bool saw_pipeline = false;
    for (u32 i = 0; i < tape.operation_count; ++i) {
        const auto& operation = tape.operations[i];
        if (operation.kind == OpKind::DrawIndexed) {
            break;
        }
        if (operation.kind == OpKind::SetViewport) {
            saw_viewport = true;
        }
        if (operation.kind == OpKind::SetRenderTargets) {
            saw_render_targets = true;
        }
        if (operation.kind == OpKind::SetGraphicsPipeline) {
            saw_pipeline = true;
        }
    }
    if (!saw_viewport) {
        add(TapeProblem::NoViewport);
    }
    if (!saw_render_targets) {
        add(TapeProblem::NoRenderTargets);
    }

    for (u32 i = 0; i < tape.operation_count; ++i) {
        const auto& operation = tape.operations[i];
        if (operation.kind != OpKind::DrawIndexed) {
            continue;
        }
        if (!operation.derived.valid) {
            add(TapeProblem::DrawNotDerivable);
        }
        const Resource* index = FindResource(tape, operation.draw.index_buffer_resource);
        if (index == nullptr) {
            add(TapeProblem::IndexResourceMissing);
        } else if (index->provenance == ResourceProvenance::Unresolved) {
            add(TapeProblem::IndexResourceUnresolved);
        }
    }

    // The pipeline is a dimension the tape cannot currently carry at all, so it is always missing.
    // It is reported here rather than silently assumed, and a caller that substitutes a diagnostic
    // pipeline must say so.
    if (!saw_pipeline) {
        problems.mask |= 1u << static_cast<u32>(TapeProblem::NoGraphicsPipeline);
    }

    return problems;
}

/// One captured command, reduced to the fields the tape builder needs.
///
/// This is a plain value rather than the capture record so the builder can be exercised against
/// synthetic fixtures without a live capture. The live exporter fills it from the capture table and
/// the tests fill it from a fixture; both then call the same BuildTapeFromCapture.
struct CapturedCommandRecord {
    u64 sequence{};
    u8 command_id{};
    u64 command_list{};
    u64 rhi_context{};
    std::array<u64, 8> payload{};
    std::array<u64, 8> render_targets{};
    u64 depth_target{};
    u8 render_target_count{};
};

/// The provenance facts the builder needs about the draw being described.
struct TapeSource {
    u64 provenance_id{};
    u64 command_list{};
    u64 index_resource{};
    u32 ue4_primitive_type{};
    u64 rhi_context{};
    u64 submission_id{};
    u64 packet_offset{};
    bool packet_offset_known{};
    /// The effective viewport read from the RHI context at draw entry, when it was readable.
    bool viewport_valid{};
    float viewport_min_x{};
    float viewport_min_y{};
    float viewport_min_z{};
    float viewport_max_x{};
    float viewport_max_y{};
    float viewport_max_z{};
    /// The index element width, from the draw package when it describes this same draw.
    u32 index_element_size{};
    /// The package resource id for the index buffer, or 0 when no package matched.
    u32 package_index_resource_ref{};
};

/// Why the builder refused to produce a tape.
enum class BuildOutcome : u8 {
    Ok = 0,
    /// The capture table was empty, so no command sequence exists.
    NoCapture = 1,
    /// The draw could not be resolved to exactly one captured command.
    DrawUnresolved = 2,
    /// The tape exceeded the shared resource capacity contract.
    ResourceCapacityExceeded = 3,
    /// The tape exceeded the shared operation capacity contract.
    OperationCapacityExceeded = 4,
    /// The built tape failed its own structural validation.
    InvalidTape = 5,
};

[[nodiscard]] constexpr std::string_view BuildOutcomeName(BuildOutcome outcome) noexcept {
    switch (outcome) {
    case BuildOutcome::Ok:
        return "ok";
    case BuildOutcome::NoCapture:
        return "no_capture";
    case BuildOutcome::DrawUnresolved:
        return "draw_unresolved";
    case BuildOutcome::ResourceCapacityExceeded:
        return "resource_capacity_exceeded";
    case BuildOutcome::OperationCapacityExceeded:
        return "operation_capacity_exceeded";
    case BuildOutcome::InvalidTape:
        return "invalid_tape";
    default:
        return "unknown";
    }
}

/// Command ids, matching Core::RhiCommandCapture.
inline constexpr u8 CaptureCommandDrawIndexedPrimitive = 1;
inline constexpr u8 CaptureCommandSetStreamSource = 3;
inline constexpr u8 CaptureCommandSetRenderTargets = 4;
inline constexpr u8 CaptureCommandSetViewport = 5;

/// Reads a 32-bit field out of a captured payload word.
[[nodiscard]] inline u32 PayloadWord32(const std::array<u64, 8>& payload, u32 index) noexcept {
    return static_cast<u32>(payload[index] & 0xFFFFFFFFull);
}

/// Reads a signed 32-bit field out of a captured payload word.
[[nodiscard]] inline s32 PayloadWordSigned32(const std::array<u64, 8>& payload,
                                             u32 index) noexcept {
    return static_cast<s32>(static_cast<u32>(payload[index] & 0xFFFFFFFFull));
}

/// Reads a float out of a captured payload word.
[[nodiscard]] inline float PayloadWordFloat(const std::array<u64, 8>& payload,
                                            u32 index) noexcept {
    const u32 bits = static_cast<u32>(payload[index] & 0xFFFFFFFFull);
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// Builds a tape from a captured command sequence.
///
/// This is the production builder: the live exporter fills the records from the capture table and
/// calls it, and the tests fill them from a synthetic fixture and call the same function. Keeping it
/// here rather than in the exporter is what makes the live logic testable without a live capture.
///
/// The draw is resolved by MatchDrawCommand, which uses the command list and index resource the RHI
/// entry recorded, disambiguated by the semantic payload. It never compares the capture-slot
/// ordinal against the deferred-command ordinal, because those are different counters.
[[nodiscard]] inline BuildOutcome BuildTapeFromCapture(
    std::span<const CapturedCommandRecord> records, const TapeSource& source, Tape& tape) noexcept {
    tape = {};
    tape.provenance_id = source.provenance_id;
    tape.submission_id = source.submission_id;
    tape.packet_offset = source.packet_offset;
    tape.packet_offset_known = source.packet_offset_known;

    if (records.empty()) {
        return BuildOutcome::NoCapture;
    }

    // Stage 1: collect the draw candidates and resolve the identity.
    //
    // Each candidate carries its own record index, and MatchDrawCommand returns that index directly.
    // There is no second mapping from a candidate slot to a record slot: doing that would resolve
    // correctly only when the draw happened to be the first candidate.
    std::array<CapturedDrawCandidate, MaxTapeOperations> candidates{};
    u32 candidate_count = 0;
    for (u32 i = 0; i < records.size() && candidate_count < MaxTapeOperations; ++i) {
        const auto& record = records[i];
        if (record.command_id != CaptureCommandDrawIndexedPrimitive) {
            continue;
        }
        auto& candidate = candidates[candidate_count];
        candidate.capture_index = i;
        candidate.command_list = record.command_list;
        candidate.index_resource = record.payload[0];
        candidate.primitive_type = PayloadWord32(record.payload, 1);
        candidate.num_primitives = PayloadWord32(record.payload, 6);
        candidate.num_instances = PayloadWord32(record.payload, 7);
        candidate.num_vertices = PayloadWord32(record.payload, 4);
        candidate.start_index = PayloadWord32(record.payload, 5);
        ++candidate_count;
    }

    DrawIdentity identity{};
    identity.command_list = source.command_list;
    identity.index_resource = source.index_resource;
    // The provenance record carries the primitive type, so it is compared. It is recorded as
    // present rather than inferred from being non-zero, because `PT_TriangleList` is zero and a
    // triangle-list draw is the common case.
    identity.primitive_type = source.ue4_primitive_type;
    identity.present.primitive_type = true;

    u32 matched_index = 0;
    const DrawMatchOutcome outcome =
        MatchDrawCommand(std::span<const CapturedDrawCandidate>{candidates.data(), candidate_count},
                         identity, matched_index);
    if (outcome != DrawMatchOutcome::Matched) {
        return BuildOutcome::DrawUnresolved;
    }

    // MatchDrawCommand returns the capture record index, not a candidate slot.
    const u32 draw_record_index = matched_index;
    const auto& draw_record = records[draw_record_index];
    const u64 draw_list = draw_record.command_list;

    // Resource ids are assigned in first-reference order, so the tape is deterministic. A repeated
    // reference to the same guest identity reuses its id and consumes no further slot.
    std::array<u64, MaxTapeResources> resource_guest{};
    u32 resource_count = 0;
    u32 next_resource_id = 1;
    bool capacity_exceeded = false;

    const auto intern = [&](u64 guest, ResourceKind kind, u32 element_size,
                            ResourceProvenance provenance, u32 package_ref) -> u32 {
        if (guest == 0) {
            return 0;
        }
        for (u32 i = 0; i < resource_count; ++i) {
            if (resource_guest[i] == guest) {
                return tape.resources[i].id;
            }
        }
        if (resource_count >= MaxTapeResources) {
            capacity_exceeded = true;
            return 0;
        }
        Resource resource{};
        resource.id = next_resource_id;
        resource.kind = kind;
        resource.provenance = provenance;
        resource.package_resource_ref = package_ref;
        resource.guest_base = guest;
        resource.element_size = element_size;
        if (!AppendResource(tape, resource)) {
            capacity_exceeded = true;
            return 0;
        }
        resource_guest[resource_count] = guest;
        ++resource_count;
        ++next_resource_id;
        return resource.id;
    };

    const u32 index_resource_id =
        intern(draw_record.payload[0], ResourceKind::IndexBuffer, source.index_element_size,
               ResourceProvenance::Ue4Object, source.package_index_resource_ref);

    // State commands on the same list, in capture order, before the draw.
    for (u32 i = 0; i < draw_record_index; ++i) {
        const auto& record = records[i];
        if (record.command_list != draw_list) {
            continue;
        }
        Operation operation{};
        operation.sequence = record.sequence;
        operation.command_list = record.command_list;
        operation.rhi_context = record.rhi_context;

        switch (record.command_id) {
        case CaptureCommandSetViewport:
            operation.kind = OpKind::SetViewport;
            // A4 established the payload order from the Execute wrapper: the first three fields are
            // integers converted to float, the last three are already floats.
            operation.viewport_min_x = static_cast<float>(PayloadWordSigned32(record.payload, 0));
            operation.viewport_min_y = static_cast<float>(PayloadWordSigned32(record.payload, 1));
            operation.viewport_min_z = PayloadWordFloat(record.payload, 2);
            operation.viewport_max_x = static_cast<float>(PayloadWordSigned32(record.payload, 3));
            operation.viewport_max_y = static_cast<float>(PayloadWordSigned32(record.payload, 4));
            operation.viewport_max_z = PayloadWordFloat(record.payload, 5);
            operation.evidence = Evidence::VerifiedTargetBinary;
            break;
        case CaptureCommandSetRenderTargets:
            operation.kind = OpKind::SetRenderTargets;
            operation.color_target_count = record.render_target_count;
            for (u32 t = 0; t < record.render_target_count && t < 8; ++t) {
                operation.color_targets[t] =
                    intern(record.render_targets[t], ResourceKind::RenderTarget, 0,
                           ResourceProvenance::Ue4Object, 0);
            }
            operation.depth_target = intern(record.depth_target, ResourceKind::DepthTarget, 0,
                                            ResourceProvenance::Ue4Object, 0);
            operation.evidence = Evidence::VerifiedTargetBinary;
            break;
        case CaptureCommandSetStreamSource:
            // The stream-source payload is a slot index and an interface pointer, not a resource
            // base, so it is carried as a tentative reference rather than claimed as an identity.
            operation.kind = OpKind::BindVertexBuffers;
            operation.slot = PayloadWord32(record.payload, 0);
            operation.resource = intern(record.payload[1], ResourceKind::VertexBuffer, 0,
                                        ResourceProvenance::Ue4Object, 0);
            operation.evidence = Evidence::Tentative;
            operation.note = "stream-source payload is an interface pointer, not a resource base";
            break;
        default:
            operation.kind = OpKind::Opaque;
            operation.evidence = Evidence::Tentative;
            operation.note = "command semantics not established";
            break;
        }
        if (!AppendOperation(tape, operation)) {
            return BuildOutcome::OperationCapacityExceeded;
        }
    }

    // Capacity exhaustion during interning is a hard failure. A tape that silently dropped a
    // resource would still validate structurally while referencing an identity it does not carry.
    if (capacity_exceeded) {
        return BuildOutcome::ResourceCapacityExceeded;
    }
    if (index_resource_id == 0) {
        return BuildOutcome::DrawUnresolved;
    }

    // The draw itself, with the original semantic payload preserved verbatim.
    Operation draw{};
    draw.kind = OpKind::DrawIndexed;
    draw.evidence = Evidence::VerifiedTargetBinary;
    draw.sequence = draw_record.sequence;
    draw.command_list = draw_list;
    draw.rhi_context = source.rhi_context;
    draw.draw.index_buffer_resource = index_resource_id;
    draw.draw.primitive_type = PayloadWord32(draw_record.payload, 1);
    draw.draw.base_vertex_index = PayloadWordSigned32(draw_record.payload, 2);
    draw.draw.first_instance = PayloadWord32(draw_record.payload, 3);
    draw.draw.num_vertices = PayloadWord32(draw_record.payload, 4);
    draw.draw.start_index = PayloadWord32(draw_record.payload, 5);
    draw.draw.num_primitives = PayloadWord32(draw_record.payload, 6);
    draw.draw.num_instances = PayloadWord32(draw_record.payload, 7);
    draw.derived = DeriveDraw(draw.draw, source.index_element_size);
    if (!AppendOperation(tape, draw)) {
        return BuildOutcome::OperationCapacityExceeded;
    }

    // The viewport the draw consumed. The RHI caches it on the context and only emits a packet on
    // change, so the command list frequently carries no viewport at all. When the context read
    // succeeded, the effective value is recorded as the state the draw actually used.
    bool list_has_viewport = false;
    for (u32 i = 0; i < tape.operation_count; ++i) {
        if (tape.operations[i].kind == OpKind::SetViewport) {
            list_has_viewport = true;
        }
    }
    if (source.viewport_valid) {
        Operation viewport{};
        viewport.kind = OpKind::SetViewport;
        viewport.evidence = Evidence::RuntimeConfirmed;
        viewport.command_list = draw_list;
        viewport.rhi_context = source.rhi_context;
        viewport.viewport_min_x = source.viewport_min_x;
        viewport.viewport_min_y = source.viewport_min_y;
        viewport.viewport_min_z = source.viewport_min_z;
        viewport.viewport_max_x = source.viewport_max_x;
        viewport.viewport_max_y = source.viewport_max_y;
        viewport.viewport_max_z = source.viewport_max_z;
        viewport.note = list_has_viewport
                            ? "effective viewport read from the RHI context at draw entry"
                            : "no viewport command on this list; value read from the RHI context "
                              "cache at draw entry";
        // Insert before the draw so the tape stays an ordered program.
        if (tape.operation_count >= MaxTapeOperations) {
            return BuildOutcome::OperationCapacityExceeded;
        }
        for (u32 i = tape.operation_count; i > tape.operation_count - 1; --i) {
            tape.operations[i] = tape.operations[i - 1];
        }
        tape.operations[tape.operation_count - 1] = viewport;
        ++tape.operation_count;
    }

    if (!list_has_viewport && !source.viewport_valid) {
        AppendUnresolved(tape, {"viewport", "SetViewport command or RHI context",
                                "no viewport was observed for this draw"});
    }

    // Shader and pipeline state are not recovered by this phase. Report them rather than
    // substituting a diagnostic pipeline silently.
    AppendUnresolved(tape, {"graphics pipeline", "FRHICommandSetBoundShaderState",
                            "the bound-shader-state command is not in the recovered cluster"});
    AppendUnresolved(tape, {"shader resources", "RHI shader resource bindings",
                            "texture and sampler binding is SC-10 scope"});

    // Validate the producer output before it is serialized. A tape that fails here is a producer
    // defect, not a consumer problem.
    if (ValidateTape(tape) != TapeProblem::None) {
        return BuildOutcome::InvalidTape;
    }
    return BuildOutcome::Ok;
}


} // namespace Core::SemanticTape