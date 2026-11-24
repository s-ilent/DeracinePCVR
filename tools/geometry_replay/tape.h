// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Backend-neutral semantic command tape reader and interpreter.
//
// This module knows nothing about Vulkan. It reads a tape, validates it, and replays it into a
// CommandSink. The Vulkan backend implements the sink. That separation is the point of the phase:
// the tape is the contract, and a different backend (NRI, or a future OpenXR device) can implement
// the same sink without touching the tape or its reader.
//
// The draw arguments come from the tape, which came from the UE4 deferred command. They are never
// reconstructed from a captured PM4 packet.

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

inline constexpr u32 kSchemaVersion = 1;

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
    Opaque = 10,
};

enum class ResourceKind : u8 {
    IndexBuffer = 1,
    VertexBuffer = 2,
    RenderTarget = 3,
    DepthTarget = 4,
    ShaderResource = 5,
    Unknown = 6,
};

enum class ResourceProvenance : u8 {
    Ue4Object = 1,
    DownstreamOracle = 2,
    Unresolved = 3,
};

struct Viewport {
    float min_x{};
    float min_y{};
    float min_z{};
    float max_x{};
    float max_y{};
    float max_z{};
};

/// The original UE4 semantic payload, preserved verbatim.
struct DrawPayload {
    u32 index_buffer_resource{};
    u32 primitive_type{};
    s32 base_vertex_index{};
    u32 first_instance{};
    u32 num_vertices{};
    u32 start_index{};
    u32 num_primitives{};
    u32 num_instances{};
};

/// The values a backend consumes, derived from the payload by the exporter.
struct DerivedDraw {
    bool valid{};
    u32 index_count{};
    u32 first_index{};
    s32 vertex_offset{};
    u32 instance_count{};
    u64 index_byte_offset{};
};

struct Resource {
    u32 id{};
    ResourceKind kind{ResourceKind::Unknown};
    ResourceProvenance provenance{ResourceProvenance::Unresolved};
    u32 package_resource_ref{};
    u64 guest_base{};
    u64 guest_size{};
    u32 element_size{};
    /// Which namespace `guest_base` belongs to. A real tape records the RHI invocation argument,
    /// which is an original UE4 object pointer; a matcher-test fixture may declare a guest address.
    std::string index_domain;
};

struct Operation {
    OpKind kind{OpKind::Opaque};
    std::string evidence;
    u64 sequence{};
    u64 command_capture_id{};
    u64 command_list{};
    u64 rhi_context{};
    Viewport viewport{};
    u32 color_target_count{};
    u32 color_targets[8]{};
    u32 depth_target{};
    u32 slot{};
    u32 resource{};
    DrawPayload draw{};
    DerivedDraw derived{};
    std::string note;
};

struct Unresolved {
    std::string what;
    std::string owner;
    std::string reason;
};

struct Tape {
    u32 schema_version{};
    std::string scene;
    u64 provenance_id{};
    std::string image_sha256;
    u64 module_base{};
    u64 submission_id{};
    u64 packet_offset{};
    bool packet_offset_known{};
    std::vector<Resource> resources;
    std::vector<Operation> operations;
    std::vector<Unresolved> unresolved;
};

/// Why a tape cannot be executed. Mirrors the exporter's vocabulary so a failure names the same
/// condition on both sides.
enum class Problem : u8 {
    None = 0,
    SchemaMismatch,
    NoDraw,
    DrawNotDerivable,
    IndexResourceMissing,
    IndexResourceUnresolved,
    NoViewport,
    NoRenderTargets,
    DuplicateResourceId,
    DanglingResourceReference,
    OperationOutOfOrder,
    /// The stored derived draw contradicts the semantic payload it claims to come from.
    DerivedInconsistent,
    /// The tape carries more resources than the shared capacity contract allows.
    ResourceCapacityExceeded,
    /// The tape carries more operations than the shared capacity contract allows.
    OperationCapacityExceeded,
    ReadFailed,
};

/// Bounded tables, mirroring Core::SemanticTape's contract. The reader enforces the same limits as
/// both producers, so a tape one side accepts is one the others accept.
inline constexpr std::size_t kMaxTapeResources = 64;
inline constexpr std::size_t kMaxTapeOperations = 256;
inline constexpr std::size_t kMaxTapeUnresolved = 16;

/// UE4 EPrimitiveType values this reader converts. These are the UE4 enum's own values, not the AMD
/// hardware topology: UE4 begins PT_TriangleList = 0, while the GPU numbers triangle list as 4.
inline constexpr u32 kPrimitiveTypeTriangleList = 0;
inline constexpr u32 kPrimitiveTypeTriangleStrip = 1;

/// Indices consumed by one primitive of the given topology, or 0 when it is not converted.
[[nodiscard]] constexpr u32 IndicesPerPrimitive(u32 primitive_type) {
    switch (primitive_type) {
    case kPrimitiveTypeTriangleList:
        return 3;
    case kPrimitiveTypeTriangleStrip:
        return 1;
    default:
        return 0;
    }
}

/// Recomputes the effective draw arguments from the authoritative semantic payload.
///
/// The reader does not trust the serialized derived values. It recomputes them here and compares,
/// so a tape whose stored values contradict its own payload is refused rather than executed on a
/// number the tape disproves.
[[nodiscard]] DerivedDraw DeriveDraw(const DrawPayload& payload, u32 index_element_size);

[[nodiscard]] std::string_view ProblemName(Problem problem);

/// Reads a tape from a directory containing tape.json.
[[nodiscard]] bool ReadTape(const std::string& directory, Tape& out, std::string& error);

/// Structural validation. Mirrors the exporter's ValidateTape.
[[nodiscard]] Problem ValidateTape(const Tape& tape);

/// Whether the tape carries everything needed to execute the draw.
[[nodiscard]] Problem CheckExecutable(const Tape& tape);

/// Finds a resource by id, or null.
[[nodiscard]] const Resource* FindResource(const Tape& tape, u32 id);

/// The backend a tape is interpreted into.
///
/// Every method is a semantic operation, not an API call. A Vulkan backend maps them to Vulkan; an
/// NRI backend would map them to NRI. The interpreter never sees a graphics API.
class CommandSink {
public:
    virtual ~CommandSink() = default;

    virtual void BeginFrame() = 0;
    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetRenderTargets(const u32* color_targets, u32 color_target_count,
                                  u32 depth_target) = 0;
    virtual void BindVertexBuffers(const u32* resources, u32 count) = 0;
    virtual void BindIndexBuffer(u32 resource, u32 element_size) = 0;
    virtual void SetGraphicsPipeline(u32 pipeline_id) = 0;
    virtual void BindShaderResources(const u32* resources, u32 count) = 0;
    virtual void DrawIndexed(const DrawPayload& payload, const DerivedDraw& derived) = 0;
    virtual void EndFrame() = 0;
    /// An operation the tape could not type. Reported, never silently skipped.
    virtual void Opaque(u64 sequence, std::string_view note) = 0;
};

/// What the interpreter did, so a caller can report it rather than infer it.
struct ExecutionReport {
    u32 operations_executed{};
    u32 opaque_operations{};
    u32 draws{};
    /// The draw arguments the interpreter actually used. These come from the tape.
    DrawPayload draw_payload{};
    DerivedDraw derived{};
    Viewport viewport{};
    bool saw_viewport{};
    bool saw_render_targets{};
    /// State the tape did not carry and the caller supplied. Empty when the tape was complete.
    std::vector<std::string> substituted;
};

/// A state value the tape does not carry, supplied by the caller instead.
///
/// This exists so a partial tape can still be executed without pretending it is complete. Every
/// substitution is named in the report, so a reader can tell which parts of the execution came from
/// the UE4 command record and which did not.
struct Substitutions {
    /// Use this viewport when the tape has none. has_viewport must be set for it to apply.
    bool has_viewport{};
    Viewport viewport{};
    /// Use these render targets when the tape has no render-target operation.
    bool has_render_targets{};
    u32 color_target_count{};
    u32 depth_target{};
};

/// Replays a tape into a sink.
///
/// Returns false when the tape is not executable and the substitutions do not cover what is
/// missing. The report is filled either way, so a partial execution says how far it got.
[[nodiscard]] bool Execute(const Tape& tape, CommandSink& sink, ExecutionReport& report,
                           std::string& error, const Substitutions& substitutions = {});

} // namespace tape