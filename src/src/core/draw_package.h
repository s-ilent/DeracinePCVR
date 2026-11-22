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
inline constexpr u32 SchemaVersion = 1;

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

    /// Set when every required resource has verified host content.
    bool replay_ready{};
    /// Human-readable reason when not replay-ready.
    std::string not_ready_reason;
};

/// Writes a package as JSON plus a sidecar binary blob of resource bytes. Returns false when the
/// package could not be written. `directory` must already exist.
[[nodiscard]] bool WritePackage(const Package& package, const std::string& directory) noexcept;

/// Reads a package back. Used by the independent validator, not by the exporter.
[[nodiscard]] bool ReadPackage(const std::string& directory, Package& out,
                               std::string& error) noexcept;

/// Validates a package's internal consistency. Returns true when the package is well formed and
/// every resource reference resolves. `replay_ready` is only true when all required resources have
/// verified host content.
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
                // A metadata-only binding may legitimately have no resource entry.
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

        // A retained resource must have bytes whose length matches its declared range, and a hash
        // that matches those bytes. This is what makes the package self-verifying.
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

        // Replay readiness is a claim about every required resource, so it is derived here rather
        // than trusted from the caller.
        for (const auto& binding : package.bindings) {
            if (binding.resource_ref == 0)
                continue;
            const auto* resource = by_id[binding.resource_ref];
            if (!IsReplayUsable(resource->status)) {
                error = "a required resource is not host-verified";
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
