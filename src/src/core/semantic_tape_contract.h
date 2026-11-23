// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <string_view>

#include "common/types.h"

namespace Core::SemanticTape {

/// Version of the semantic command tape. A reader refuses a version it does not understand rather
/// than reinterpreting fields whose meaning may have changed.
///
/// Version 1 is the first tape: one UE4-origin indexed draw with the state that precedes it.
inline constexpr u32 SchemaVersion = 1;

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
    std::array<Resource, 32> resources{};
    u32 resource_count{};
    std::array<UnresolvedDependency, 16> unresolved{};
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
[[nodiscard]] inline bool AppendResource(Tape& tape, const Resource& resource) noexcept {
    if (tape.resource_count >= tape.resources.size())
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

} // namespace Core::SemanticTape
