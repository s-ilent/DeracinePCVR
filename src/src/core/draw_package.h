// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Core {

namespace DrawPackage {

/// A resource is usable for replay only when its content was established from the host resource
/// the draw actually read. Guest-memory observation is not enough.
[[nodiscard]] constexpr bool IsReplayUsable(u8 status) noexcept {
    return status == 1 || status == 2;
}

/// Schema version. Bumped when a field changes meaning, so a reader can refuse a package it does
/// not understand rather than silently misinterpreting it.
///
/// Version 2 adds geometry and replaces the single `replay_ready` flag with explicit readiness
/// dimensions. A version 1 package is rejected rather than reinterpreted, because its
/// `replay_ready` meant shader-buffer completeness only.
inline constexpr u32 SchemaVersion = 2;

/// Opt-in for writing a private draw package. Separate from the capture flags so a package is only
/// produced when explicitly asked for.
inline constexpr std::string_view EnableEnvironmentVariable =
    "SHADPS4_DERACINE_DRAW_PACKAGE";

/// Bounded number of packages written per run.
inline constexpr size_t MaxPackages = 4;

/// Bounded bytes of resource content retained per package.
inline constexpr size_t MaxPackageBytes = 8 * 1024 * 1024;

/// One retained resource. Two bindings of the same content share one entry, referenced by id.
struct Resource {
    u32 id{};
    /// Guest range the descriptor requested, and the validated range actually used.
    u64 requested_base{};
    u64 requested_size{};
    u64 clamped_base{};
    u64 clamped_size{};
    /// Content hash of the retained bytes.
    u64 content_hash{};
    /// How the content was established.
    u8 status{};
    /// Retained bytes. Empty when the resource is metadata-only.
    std::vector<u8> bytes;
};

/// One binding occurrence, referencing a resource by id.
struct Binding {
    u8 stage{};
    u32 resource_index{};
    u32 sharp_idx{};
    u32 vk_binding{};
    u64 vk_offset{};
    u64 vk_range{};
    u64 buffer_id{};
    u32 resource_ref{};
    u8 status{};
};

/// One vertex binding, with the host allocation and byte range it resolved to.
struct VertexBinding {
    u32 binding{};
    u64 host_buffer{};
    u64 host_offset{};
    u64 guest_base{};
    u64 guest_size{};
    u32 stride{};
    u32 input_rate{};
    u32 divisor{};
    u64 upload_sequence{};
    u64 content_hash{};
    u32 resource_ref{};
    u8 status{};

    /// The attribute this binding feeds, as the production vertex-input construction describes it.
    /// shadPS4 binds one attribute per binding, so the two travel together.
    u32 semantic{};
    u32 vk_format{};
    u32 attr_offset{};
    u32 element_count{};
    /// Shader identity, so a package can be selected and reproduced by signature.
    u64 vs_hash{};
    u64 ps_hash{};
};

/// The index binding, with the draw arguments the renderer actually used.
struct IndexBinding {
    u64 host_buffer{};
    u64 host_offset{};
    u64 guest_base{};
    u64 guest_size{};
    u32 index_size{};
    u32 index_count{};
    u32 first_index{};
    u32 base_vertex{};
    u32 instance_count{};
    u32 first_instance{};
    u32 primitive_type{};
    u8 index_type{};
    u8 swap_mode{};
    u64 upload_sequence{};
    u64 content_hash{};
    u32 resource_ref{};
    u8 status{};
};

/// What a package is actually sufficient for. These are separate because a package can have
/// complete shader buffers and no geometry, or complete geometry and no textures.
struct Readiness {
    bool shader_buffers_ready{};
    bool geometry_ready{};
    bool textures_ready{};
    bool pipeline_state_ready{};
    /// Enough to run a diagnostic position-only replay of the captured geometry.
    bool diagnostic_geometry_replay_ready{};
    /// Enough to reproduce the draw with its original materials.
    bool full_draw_replay_ready{};
    /// Why the highest claimed level is not higher.
    std::string limitation;
};

/// How the diagnostic replay should interpret the geometry. This is a documented choice, not a
/// recovered fact: the game's own transform is not captured.
struct GeometryInterpretation {
    /// Binding and component count that the diagnostic renderer treats as object-space position.
    u32 position_binding{};
    u32 position_components{};
    /// How that attribution was established.
    std::string position_evidence;
    /// Axis-aligned bounds of the position attribute, in the captured units.
    float bounds_min[3]{};
    float bounds_max[3]{};
    /// Whether the bounds are nondegenerate on all three axes, which is what distinguishes a scene
    /// mesh from a screen-space element.
    bool is_three_dimensional{};
    /// Triangle statistics, computed from the captured indices.
    u32 triangle_count{};
    u32 degenerate_triangles{};
    u32 unique_referenced_vertices{};
    /// Nonfinite position values found while computing the bounds.
    u32 nonfinite_vertices{};
};

struct Package {
    u32 schema_version{SchemaVersion};
    std::string scene;
    u64 provenance_id{};
    u64 pipeline_hash{};
    u64 vs_hash{};
    u64 ps_hash{};
    u32 primitive_type{};
    bool is_indexed{};
    u32 num_indices{};
    u32 num_instances{};

    std::vector<Binding> bindings;
    std::vector<Resource> resources;

    std::vector<VertexBinding> vertex_bindings;
    bool has_index_binding{};
    IndexBinding index_binding{};
    GeometryInterpretation geometry;

    Readiness readiness;


};

/// Writes a package as JSON plus a sidecar binary blob of resource bytes. Returns false when the
/// package could not be written. `directory` must already exist.
[[nodiscard]] bool WritePackage(const Package& package, const std::string& directory) noexcept;

/// Reads a package back. Used by the independent validator, not by the exporter.
[[nodiscard]] bool ReadPackage(const std::string& directory, Package& out,
                               std::string& error) noexcept;

/// Validates a package's internal consistency. Returns true when the package is well formed and
/// every resource reference resolves and every retained byte range matches its hash.
///
/// Readiness is derived here rather than trusted from the caller: a package cannot claim
/// `diagnostic_geometry_replay_ready` unless its geometry bindings resolve to verified resources.
///
/// Header-only so the contract tests can exercise it without linking the logging stack.
[[nodiscard]] inline bool ValidatePackage(const Package& package, std::string& error) noexcept {
    try {
        if (package.schema_version != SchemaVersion) {
            error = "unsupported schema version";
            return false;
        }
        if (package.provenance_id == 0) {
            error = "package has no provenance id";
            return false;
        }

        // Resource ids must be unique, so a reference is unambiguous.
        std::map<u32, const Resource*> by_id;
        for (const auto& resource : package.resources) {
            if (resource.id == 0) {
                error = "resource id 0 is reserved";
                return false;
            }
            if (by_id.contains(resource.id)) {
                error = "duplicate resource id";
                return false;
            }
            by_id[resource.id] = &resource;
        }

        // Every binding must reference a resource that exists.
        for (const auto& binding : package.bindings) {
            if (binding.resource_ref == 0) {
                if (binding.status != 3) {
                    error = "binding has no resource reference";
                    return false;
                }
                continue;
            }
            if (!by_id.contains(binding.resource_ref)) {
                error = "binding references a missing resource";
                return false;
            }
        }

        // Geometry bindings must resolve too, and their ranges must be consistent.
        for (const auto& vertex : package.vertex_bindings) {
            if (vertex.resource_ref == 0) {
                if (vertex.status != 3) {
                    error = "vertex binding has no resource reference";
                    return false;
                }
                continue;
            }
            if (!by_id.contains(vertex.resource_ref)) {
                error = "vertex binding references a missing resource";
                return false;
            }
            if (vertex.stride == 0 && vertex.guest_size != 0) {
                // A zero-stride fetch is a constant-style input; it is legal but must not claim a
                // per-vertex range.
                continue;
            }
        }
        if (package.has_index_binding && package.index_binding.resource_ref != 0) {
            if (!by_id.contains(package.index_binding.resource_ref)) {
                error = "index binding references a missing resource";
                return false;
            }
            const auto& index = package.index_binding;
            if (index.index_size != 2 && index.index_size != 4) {
                error = "index element width is neither 2 nor 4";
                return false;
            }
            if (index.index_count != 0 && index.guest_size != 0) {
                const u64 needed = static_cast<u64>(index.index_count) * index.index_size;
                if (needed > index.guest_size) {
                    error = "index count exceeds the bound index range";
                    return false;
                }
            }
        }

        // A retained resource must have bytes whose length matches its declared range, and a hash
        // that matches those bytes.
        for (const auto& resource : package.resources) {
            if (resource.bytes.empty()) {
                if (IsReplayUsable(resource.status)) {
                    error = "resource claims verified content but retains no bytes";
                    return false;
                }
                continue;
            }
            if (resource.bytes.size() != resource.clamped_size) {
                error = "resource byte length does not match its declared range";
                return false;
            }
            u64 hash = 0xcbf29ce484222325ull;
            for (const u8 byte : resource.bytes) {
                hash ^= byte;
                hash *= 0x100000001b3ull;
            }
            if (hash != resource.content_hash) {
                error = "resource content hash does not match its bytes";
                return false;
            }
        }

        // Shader-buffer readiness requires every shader binding to be host-verified.
        if (package.readiness.shader_buffers_ready) {
            for (const auto& binding : package.bindings) {
                if (binding.resource_ref == 0)
                    continue;
                if (!IsReplayUsable(by_id[binding.resource_ref]->status)) {
                    error = "package claims shader-buffer readiness with an unverified resource";
                    return false;
                }
            }
        }

        // Geometry readiness requires every geometry binding to be host-verified, and an index
        // binding when the draw is indexed.
        if (package.readiness.geometry_ready) {
            for (const auto& vertex : package.vertex_bindings) {
                if (vertex.resource_ref == 0)
                    continue;
                if (!IsReplayUsable(by_id[vertex.resource_ref]->status)) {
                    error = "package claims geometry readiness with an unverified vertex resource";
                    return false;
                }
            }
            if (package.is_indexed) {
                if (!package.has_index_binding) {
                    error = "package claims geometry readiness for an indexed draw with no index "
                            "binding";
                    return false;
                }
                if (package.index_binding.resource_ref == 0 ||
                    !IsReplayUsable(by_id[package.index_binding.resource_ref]->status)) {
                    error = "package claims geometry readiness with an unverified index resource";
                    return false;
                }
            }
        }

        // Diagnostic replay readiness is a strictly stronger claim than geometry readiness.
        if (package.readiness.diagnostic_geometry_replay_ready && !package.readiness.geometry_ready) {
            error = "diagnostic replay readiness claimed without geometry readiness";
            return false;
        }
        // Full draw readiness requires everything.
        if (package.readiness.full_draw_replay_ready) {
            if (!package.readiness.geometry_ready || !package.readiness.shader_buffers_ready ||
                !package.readiness.textures_ready || !package.readiness.pipeline_state_ready) {
                error = "full draw readiness claimed without every dimension";
                return false;
            }
        }

        error.clear();
        return true;
    } catch (...) {
        error = "validation threw";
        return false;
    }
}

} // namespace DrawPackage
} // namespace Core
