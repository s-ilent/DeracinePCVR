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

TEST(DrawPackageValidator, RejectsPartialMipChainClaimingContentReadiness) {
    // A view that asks for four mip levels and got one is not complete. Claiming content readiness
    // for it would describe a partial capture as a complete original resource.
    auto package = MakeValidPackage();
    AddGeometry(package);
    Texture texture{};
    texture.id = 1;
    texture.width = 64;
    texture.height = 64;
    texture.mip_first = 0;
    texture.mip_count = 4;
    texture.layer_first = 0;
    texture.layer_count = 1;
    texture.block_columns = 16;
    texture.block_rows = 16;
    texture.block_texels_x = 4;
    texture.block_texels_y = 4;
    texture.bytes_per_block = 16;
    texture.transfer_id = 1;
    TextureSubresource sub{};
    sub.mip = 0;
    sub.layer = 0;
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

TEST(DrawPackageValidator, AcceptsCompleteMipChainClaimingContentReadiness) {
    auto package = MakeValidPackage();
    AddGeometry(package);
    Texture texture{};
    texture.id = 1;
    texture.width = 64;
    texture.height = 64;
    texture.mip_first = 0;
    texture.mip_count = 2;
    texture.layer_first = 0;
    texture.layer_count = 1;
    texture.block_columns = 16;
    texture.block_rows = 16;
    texture.block_texels_x = 4;
    texture.block_texels_y = 4;
    texture.bytes_per_block = 16;
    texture.transfer_id = 1;
    for (u32 mip = 0; mip < 2; ++mip) {
        const u32 w = std::max(1u, 64u >> mip);
        const u32 h = std::max(1u, 64u >> mip);
        TextureSubresource sub{};
        sub.mip = mip;
        sub.layer = 0;
        sub.width = w;
        sub.height = h;
        sub.row_pitch = ((w + 3) / 4) * 16;
        sub.byte_length = sub.row_pitch * ((h + 3) / 4);
        sub.content_hash = 1;
        texture.subresources.push_back(sub);
    }
    package.textures.push_back(texture);
    package.readiness.texture_binding_ready = true;
    package.readiness.texture_contents_ready = true;
    std::string error;
    EXPECT_TRUE(ValidatePackage(package, error)) << error;
}

TEST(DrawPackageValidator, RejectsMissingArrayLayerClaimingContentReadiness) {
    auto package = MakeValidPackage();
    AddGeometry(package);
    Texture texture{};
    texture.id = 1;
    texture.width = 64;
    texture.height = 64;
    texture.mip_first = 0;
    texture.mip_count = 1;
    texture.layer_first = 0;
    texture.layer_count = 2;
    texture.block_columns = 16;
    texture.block_rows = 16;
    texture.block_texels_x = 4;
    texture.block_texels_y = 4;
    texture.bytes_per_block = 16;
    texture.transfer_id = 1;
    TextureSubresource sub{};
    sub.mip = 0;
    sub.layer = 0;
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

TEST(DrawPackageValidator, RejectsSchemaVersionOne) {
    // A version 1 package's replay_ready meant shader-buffer completeness only, so it must not be
    // reinterpreted under the version 2 contract.
    auto package = MakeValidPackage();
    package.schema_version = 1;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}


// ---------------------------------------------------------------------------------------------
// Original-shader readiness
//
// These cover the dimensions SC-11 adds. The point of each is that a package cannot claim it can
// run the draw on its original shaders while one of the inputs is missing.
// ---------------------------------------------------------------------------------------------

/// A shader stage whose permutation hash is the combination of its program hash and index, which
/// is what the pipeline key carries.
Core::DrawPackage::ShaderStage MakeStage(u32 logical_stage, u64 program_hash, u32 perm_idx) {
    Core::DrawPackage::ShaderStage stage{};
    stage.logical_stage = logical_stage;
    stage.hardware_stage = logical_stage == 3 ? 1u : 0u;
    stage.program_hash = program_hash;
    stage.permutation_index = perm_idx;
    stage.permutation_hash = HashCombine(program_hash, static_cast<u64>(perm_idx));
    stage.patched = false;
    stage.entry_point = "main";
    // Four SPIR-V words is enough to exercise the length and hash checks.
    stage.effective_bytes = {0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x01, 0x00,
                             0x0B, 0x00, 0x0D, 0x00, 0x01, 0x00, 0x00, 0x00};
    stage.effective_byte_length = stage.effective_bytes.size();
    sha256::SHA256 hasher;
    hasher.update(stage.effective_bytes.data(), stage.effective_bytes.size());
    const auto digest = hasher.final();
    static constexpr char kHex[] = "0123456789abcdef";
    stage.effective_sha256.clear();
    for (const u8 byte : digest) {
        stage.effective_sha256.push_back(kHex[byte >> 4]);
        stage.effective_sha256.push_back(kHex[byte & 0x0F]);
    }
    stage.generated_sha256 = stage.effective_sha256;
    stage.effective_file = "shader.spv";
    return stage;
}

/// A package that carries everything the original-shader claim needs.
/// A descriptor interface for one stage, declaring the uniform buffer the fixture binds.
Core::DrawPackage::ShaderInterface MakeInterface(u64 program_hash, u32 perm_idx,
                                                 u32 logical_stage) {
    Core::DrawPackage::ShaderInterface interface{};
    interface.program_hash = program_hash;
    interface.permutation_index = perm_idx;
    interface.permutation_hash = HashCombine(program_hash, static_cast<u64>(perm_idx));
    interface.hardware_stage = logical_stage == 3 ? 1u : 0u;
    interface.logical_stage = logical_stage;
    interface.patched = false;
    interface.push_constant_offset = 0;
    interface.push_constant_size = 0;
    interface.push_constant_stage_flags = 1u << logical_stage;
    interface.unified_binding_count = 1;
    Core::DrawPackage::ShaderDescriptor descriptor{};
    descriptor.set = 0;
    descriptor.binding = 0;
    descriptor.descriptor_class = 1; // uniform buffer
    descriptor.count = 1;
    descriptor.stage_flags = 1u << logical_stage;
    descriptor.guest_resource_index = 0;
    descriptor.written = false;
    interface.descriptors.push_back(descriptor);
    return interface;
}

Package MakeOriginalShaderPackage() {
    auto package = MakeValidPackage();
    AddGeometry(package);
    package.readiness.geometry_ready = true;
    package.shader_stages.push_back(MakeStage(3, 0x1111, 0));
    package.shader_stages.push_back(MakeStage(0, 0x2222, 0));
    package.shader_interfaces.push_back(MakeInterface(0x1111, 0, 3));
    package.shader_interfaces.push_back(MakeInterface(0x2222, 0, 0));
    package.readiness.shader_binaries_ready = true;
    package.readiness.shader_bindings_ready = true;
    package.readiness.shader_parameters_ready = true;
    package.pipeline_state.has_topology = true;
    package.pipeline_state.topology = 4;
    package.pipeline_state.color_formats = {37};
    package.pipeline_state.push_constant_size = 0;
    package.readiness.pipeline_state_ready_original = true;
    package.reference_output.width = 64;
    package.reference_output.height = 64;
    package.reference_output.post_sha256 = "abc";
    package.readiness.reference_output_ready = true;
    package.readiness.original_shader_replay_ready = true;
    return package;
}

TEST(DrawPackageOriginalShader, AcceptsACompleteOriginalShaderPackage) {
    auto package = MakeOriginalShaderPackage();
    std::string error;
    EXPECT_TRUE(ValidatePackage(package, error)) << error;
}

TEST(DrawPackageOriginalShader, RejectsBinariesWithoutBothStages) {
    auto package = MakeOriginalShaderPackage();
    package.shader_stages.pop_back(); // drop the fragment stage
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("both a vertex and a fragment"), std::string::npos) << error;
}

TEST(DrawPackageOriginalShader, RejectsAStageWithNoEffectiveHash) {
    auto package = MakeOriginalShaderPackage();
    package.shader_stages[0].effective_sha256.clear();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageOriginalShader, RejectsAStageWithNoBytes) {
    auto package = MakeOriginalShaderPackage();
    package.shader_stages[0].effective_byte_length = 0;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageOriginalShader, RejectsAPermutationHashThatDoesNotMatchItsParts) {
    // The identity is (program hash, permutation index). A hash that is not their combination is
    // internally inconsistent, which is what a mis-associated module looks like.
    auto package = MakeOriginalShaderPackage();
    package.shader_stages[0].permutation_hash = 0xDEADBEEF;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("permutation hash"), std::string::npos) << error;
}

TEST(DrawPackageOriginalShader, RejectsBytesThatDoNotMatchTheirHash) {
    auto package = MakeOriginalShaderPackage();
    package.shader_stages[0].effective_bytes[0] ^= 0xFF;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("content hash"), std::string::npos) << error;
}

TEST(DrawPackageOriginalShader, RejectsBindingsWithoutBinaries) {
    auto package = MakeOriginalShaderPackage();
    package.readiness.shader_binaries_ready = false;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageOriginalShader, RejectsParametersWithoutBindings) {
    auto package = MakeOriginalShaderPackage();
    package.readiness.shader_bindings_ready = false;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageOriginalShader, RejectsAPushConstantRangeThatDoesNotMatchItsBytes) {
    auto package = MakeOriginalShaderPackage();
    package.pipeline_state.push_constant_size = 16;
    package.pipeline_state.push_constant_bytes.clear();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("push-constant"), std::string::npos) << error;
}

TEST(DrawPackageOriginalShader, RejectsPipelineStateWithoutTopology) {
    // Zero is a legitimate topology, so the presence flag is what marks it missing.
    auto package = MakeOriginalShaderPackage();
    package.pipeline_state.has_topology = false;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageOriginalShader, RejectsReferenceOutputWithoutAPostImage) {
    auto package = MakeOriginalShaderPackage();
    package.reference_output.post_sha256.clear();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

TEST(DrawPackageOriginalShader, RejectsTheStrongestClaimWhenOneDimensionIsMissing) {
    // Each dimension in turn: the strongest claim must fail when any one is absent.
    const auto check = [](auto mutate) {
        auto package = MakeOriginalShaderPackage();
        mutate(package);
        std::string error;
        return !ValidatePackage(package, error);
    };
    EXPECT_TRUE(check([](Package& p) { p.readiness.shader_binaries_ready = false; }));
    EXPECT_TRUE(check([](Package& p) { p.readiness.shader_bindings_ready = false; }));
    EXPECT_TRUE(check([](Package& p) { p.readiness.shader_parameters_ready = false; }));
    EXPECT_TRUE(check([](Package& p) { p.readiness.pipeline_state_ready_original = false; }));
    EXPECT_TRUE(check([](Package& p) { p.readiness.reference_output_ready = false; }));
    EXPECT_TRUE(check([](Package& p) { p.readiness.geometry_ready = false; }));
}

TEST(DrawPackageOriginalShader, RejectsTheStrongestClaimWithoutGeometry) {
    auto package = MakeOriginalShaderPackage();
    package.readiness.geometry_ready = false;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("without geometry"), std::string::npos) << error;
}

TEST(DrawPackageOriginalShader, AVersionTwoPackageIsRefused) {
    // The schema bump is deliberate: a version 2 package did not distinguish these dimensions, so
    // reinterpreting it would let it claim readiness it never established.
    auto package = MakeValidPackage();
    package.schema_version = 2;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}

// ---------------------------------------------------------------------------------------------
// Descriptor interface (schema 4)
//
// These cover the check that replaced "some binding exists": the effective modules' required
// resources must actually be available.
// ---------------------------------------------------------------------------------------------

TEST(DrawPackageInterface, ACompleteInterfaceIsAccepted) {
    auto package = MakeOriginalShaderPackage();
    std::string error;
    EXPECT_TRUE(ValidatePackage(package, error)) << error;
}

TEST(DrawPackageInterface, BindingsWithoutAnInterfaceAreRejected) {
    auto package = MakeOriginalShaderPackage();
    package.shader_interfaces.clear();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("no descriptor interface"), std::string::npos) << error;
}

TEST(DrawPackageInterface, AStageWithoutAnInterfaceIsRejected) {
    auto package = MakeOriginalShaderPackage();
    package.shader_interfaces.pop_back();
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("no descriptor interface for its permutation"), std::string::npos)
        << error;
}

TEST(DrawPackageInterface, ADescriptorWithNoCapturedResourceIsRejected) {
    // The interface declares a binding the package has no resource for. That is the defect the
    // old check could not see: a binding existed, but not the one the module needs.
    auto package = MakeOriginalShaderPackage();
    package.shader_interfaces[0].descriptors[0].binding = 9;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    // The message now names the domain the descriptor was resolved in, which is what makes a
    // domain mismatch distinguishable from a missing resource.
    const bool named = error.find("no captured binding record") != std::string::npos ||
                       error.find("no captured resource") != std::string::npos ||
                       error.find("no captured image content") != std::string::npos;
    EXPECT_TRUE(named) << error;
}

TEST(DrawPackageInterface, AZeroDescriptorCountIsRejected) {
    auto package = MakeOriginalShaderPackage();
    package.shader_interfaces[0].descriptors[0].count = 0;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("zero descriptor count"), std::string::npos) << error;
}

TEST(DrawPackageInterface, ASamplerNeedsNoCapturedBuffer) {
    // A sampler descriptor legitimately has no buffer, so it must not be required to resolve.
    auto package = MakeOriginalShaderPackage();
    Core::DrawPackage::ShaderDescriptor sampler{};
    sampler.set = 0;
    sampler.binding = 5;
    sampler.descriptor_class = 5; // sampler
    sampler.count = 1;
    sampler.stage_flags = 1u << 0;
    package.shader_interfaces[1].descriptors.push_back(sampler);
    std::string error;
    // A sampler needs no buffer, which was the original point, but it does need a captured sampler
    // identity. A descriptor the module declares must resolve in its own domain.
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("no captured sampler"), std::string::npos) << error;

    Core::DrawPackage::Sampler captured{};
    captured.id = 1;
    package.samplers.push_back(captured);
    EXPECT_TRUE(ValidatePackage(package, error)) << error;
}

TEST(DrawPackageInterface, ZeroTopologyIsAcceptedWhenPresenceIsDeclared) {
    // Zero is a legitimate Vulkan topology value. The presence flag is what distinguishes it from
    // missing data.
    auto package = MakeOriginalShaderPackage();
    package.pipeline_state.topology = 0;
    package.pipeline_state.has_topology = true;
    std::string error;
    EXPECT_TRUE(ValidatePackage(package, error)) << error;
}

TEST(DrawPackageInterface, MissingTopologyPresenceIsRejected) {
    auto package = MakeOriginalShaderPackage();
    package.pipeline_state.has_topology = false;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
    EXPECT_NE(error.find("no topology"), std::string::npos) << error;
}

TEST(DrawPackageInterface, AVersionThreePackageIsRefused) {
    auto package = MakeOriginalShaderPackage();
    package.schema_version = 3;
    std::string error;
    EXPECT_FALSE(ValidatePackage(package, error));
}
} // namespace