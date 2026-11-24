// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/draw_package.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using Core::DrawPackage::Binding;
using Core::DrawPackage::IndexBinding;
using Core::DrawPackage::Texture;
using Core::DrawPackage::TextureSubresource;
using Core::DrawPackage::VertexBinding;
using Core::DrawPackage::Package;
using Core::DrawPackage::Resource;
using Core::DrawPackage::SchemaVersion;
using Core::DrawPackage::ValidatePackage;

/// FNV-1a, matching the exporter, so a fixture can declare a correct hash.
u64 Hash(const std::vector<u8>& bytes) {
    u64 hash = 0xcbf29ce484222325ull;
    for (const u8 byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

Package MakeValidPackage() {
    Package package{};
    package.scene = "classroom-title";
    package.provenance_id = 1;
    package.pipeline_hash = 0x1234;
    package.vs_hash = 0x5678;
    package.ps_hash = 0x9abc;
    package.primitive_type = 4;
    package.is_indexed = true;
    package.num_indices = 6;
    package.num_instances = 1;

    Resource resource{};
    resource.id = 1;
    resource.requested_base = 0x2000000000ull;
    resource.requested_size = 16;
    resource.clamped_base = 0x2000000000ull;
    resource.clamped_size = 16;
    resource.status = 1; // host_content_verified
    resource.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    resource.content_hash = Hash(resource.bytes);
    package.resources.push_back(resource);

    Binding binding{};
    binding.stage = 0;
    binding.resource_index = 0;
    binding.vk_binding = 0;
    binding.resource_ref = 1;
    binding.status = 1;
    package.bindings.push_back(binding);

    package.readiness.shader_buffers_ready = true;
    return package;
}

/// Adds a vertex binding and index binding that reference resource 1, so the geometry checks have
/// something to validate.
void AddGeometry(Package& package) {
    VertexBinding vertex{};
    vertex.binding = 0;
    vertex.stride = 8;
    vertex.guest_size = 16;
    vertex.resource_ref = 1;
    vertex.status = 1;
    vertex.semantic = 0;
    vertex.vk_format = 106;
    vertex.element_count = 3;
    package.vertex_bindings.push_back(vertex);

    IndexBinding index{};
    index.index_size = 2;
    index.index_count = 6;
    index.guest_size = 12;
    index.resource_ref = 1;
    index.status = 1;
    package.index_binding = index;
    package.has_index_binding = true;
}

TEST(DrawPackageValidator, AcceptsAWellFormedPackage) {
    auto package = MakeValidPackage();
    std::string error;
    EXPECT_TRUE(ValidatePackage(package, error)) << error;
}

TEST(DrawPackageValidator, RejectsWrongSchemaVersion) {
    auto package = MakeValidPackage();
    package.schema_version = SchemaVersion + 1;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_FALSE(error.empty());
}

TEST(DrawPackageValidator, RejectsMissingProvenanceId) {
    auto package = MakeValidPackage();
    package.provenance_id = 0;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsDuplicateResourceIds) {
    auto package = MakeValidPackage();
    package.resources.push_back(package.resources.front());
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsDanglingResourceReference) {
    auto package = MakeValidPackage();
    package.bindings.front().resource_ref = 99;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsByteLengthMismatch) {
    auto package = MakeValidPackage();
    package.resources.front().bytes.pop_back();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsContentHashMismatch) {
    auto package = MakeValidPackage();
    package.resources.front().bytes[0] ^= 0xFF;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsVerifiedStatusWithoutBytes) {
    // A resource claiming verified content must retain the bytes; otherwise the claim is
    // unverifiable and the package must be refused.
    auto package = MakeValidPackage();
    package.resources.front().bytes.clear();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsGuestObservedResourceForReplay) {
    // Guest-memory observation is not host-content verification, so a package whose required
    // resource is only guest-observed is not replay-ready.
    auto package = MakeValidPackage();
    package.resources.front().status = 0; // guest_memory_observed
    package.resources.front().bytes.clear();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, AllowsMetadataOnlyBindingWithoutResource) {
    // A metadata-only binding legitimately has no resource entry, and that alone must not make
    // the package invalid.
    auto package = MakeValidPackage();
    Binding metadata{};
    metadata.stage = 3;
    metadata.resource_index = 1;
    metadata.vk_binding = 5;
    metadata.resource_ref = 0;
    metadata.status = 3; // metadata_only
    package.bindings.push_back(metadata);
    std::string error;
    EXPECT_TRUE(ValidatePackage(package, error)) << error;
}

TEST(DrawPackageValidator, SharedResourceIsOneEntry) {
    // Two bindings of the same content must reference one resource, not duplicate the bytes.
    auto package = MakeValidPackage();
    Binding second{};
    second.stage = 3;
    second.resource_index = 0;
    second.vk_binding = 3;
    second.resource_ref = 1;
    second.status = 1;
    package.bindings.push_back(second);
    std::string error;
    EXPECT_TRUE(ValidatePackage(package, error)) << error;
    EXPECT_EQ(package.resources.size(), 1u);
    EXPECT_EQ(package.bindings.size(), 2u);
}

TEST(DrawPackageValidator, RejectsZeroByteResourceClaimingContent) {
    auto package = MakeValidPackage();
    package.resources.front().clamped_size = 0;
    package.resources.front().bytes.clear();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, AcceptsGeometryReadyPackage) {
    auto package = MakeValidPackage();
    AddGeometry(package);
    std::string error;
    EXPECT_TRUE(ValidatePackage(package, error)) << error;
}

TEST(DrawPackageValidator, RejectsGeometryReadinessWithoutVertexBindings) {
    auto package = MakeValidPackage();
    package.readiness.geometry_ready = true;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsGeometryReadinessWithUnverifiedVertexResource) {
    auto package = MakeValidPackage();
    AddGeometry(package);
    package.resources.front().status = 0; // guest_memory_observed
    package.resources.front().bytes.clear();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsIndexedGeometryWithoutIndexBinding) {
    auto package = MakeValidPackage();
    AddGeometry(package);
    package.has_index_binding = false;
    package.readiness.geometry_ready = true;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsBadIndexWidth) {
    auto package = MakeValidPackage();
    AddGeometry(package);
    package.index_binding.index_size = 3;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsIndexCountBeyondCapturedBytes) {
    auto package = MakeValidPackage();
    AddGeometry(package);
    package.index_binding.index_count = 1000;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsDiagnosticReadinessWithoutGeometry) {
    auto package = MakeValidPackage();
    package.readiness.geometry_ready = false;
    package.readiness.diagnostic_geometry_replay_ready = true;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsOriginalMaterialReadinessWithoutTextures) {
    auto package = MakeValidPackage();
    AddGeometry(package);
    package.readiness.original_material_ready = true;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsTextureContentReadinessWithoutBlockGeometry) {
    // A compressed subresource's byte length is derived from its block geometry, so a package that
    // claims captured content without it cannot be checked and must be refused.
    auto package = MakeValidPackage();
    AddGeometry(package);
    Texture texture{};
    texture.id = 1;
    texture.width = 64;
    texture.height = 64;
    texture.block_columns = 16;
    texture.block_rows = 16;
    texture.bytes_per_block = 0;
    TextureSubresource sub{};
    sub.width = 64;
    sub.height = 64;
    sub.row_pitch = 256;
    sub.byte_length = 4096;
    sub.content_hash = 1;
    texture.subresources.push_back(sub);
    package.textures.push_back(texture);
    package.readiness.texture_binding_ready = true;
    package.readiness.texture_contents_ready = true;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsTextureContentReadinessWithShortPayload) {
    // The payload must be exactly the block geometry implies. A shorter one is a partial copy, and
    // a hash of the bytes it did copy still matches itself, so only the length check catches it.
    auto package = MakeValidPackage();
    AddGeometry(package);
    Texture texture{};
    texture.id = 1;
    texture.width = 64;
    texture.height = 64;
    texture.block_columns = 16;
    texture.block_rows = 16;
    texture.block_texels_x = 4;
    texture.block_texels_y = 4;
    texture.bytes_per_block = 16;
    TextureSubresource sub{};
    sub.width = 64;
    sub.height = 64;
    sub.row_pitch = 256;
    sub.byte_length = 1024;
    sub.content_hash = 1;
    texture.subresources.push_back(sub);
    package.textures.push_back(texture);
    package.readiness.texture_binding_ready = true;
    package.readiness.texture_contents_ready = true;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageValidator, RejectsSchemaVersionOne) {
    // A version 1 package's replay_ready meant shader-buffer completeness only, so it must not be
    // reinterpreted under the version 2 contract.
    auto package = MakeValidPackage();
    package.schema_version = 1;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

} // namespace
