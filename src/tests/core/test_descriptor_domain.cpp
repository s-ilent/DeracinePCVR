// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for descriptor-domain validation.
//
// The defect these exist to prevent: the validator resolved every descriptor class through
// `package.bindings`, but images live in `package.textures` and samplers in `package.samplers`. An
// image descriptor was therefore looked for in the buffer collection and never found, so a package
// whose images were captured perfectly was rejected with "a descriptor interface declares a binding
// with no captured resource".
//
// The second defect: a binding existing is not the same as its bytes being captured. A binding that
// names no resource, or a resource with no bytes, is not readiness.

#include <gtest/gtest.h>

#include "core/draw_package.h"

namespace {

using Core::DrawPackage::Binding;
using Core::DrawPackage::Package;
using Core::DrawPackage::Resource;
using Core::DrawPackage::Sampler;
using Core::DrawPackage::SchemaVersion;
using Core::DrawPackage::ShaderDescriptor;
using Core::DrawPackage::ShaderInterface;
using Core::DrawPackage::Texture;

/// The permutation hash, as the validator recomputes it.
u64 HashCombine(u64 seed, u64 value) {
    return seed ^ (value + 0x9e3779b9ull + (seed << 12) + (seed >> 4));
}

/// SHA-256 as lowercase hex, matching what the validator checks for a shader stage.
std::string Sha256Hex(const std::vector<u8>& bytes) {
    static constexpr u32 k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<u8> message = bytes;
    const u64 bit_length = static_cast<u64>(bytes.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<u8>((bit_length >> (i * 8)) & 0xFF));
    }
    const auto rotr = [](u32 v, u32 c) { return (v >> c) | (v << (32 - c)); };
    for (size_t offset = 0; offset < message.size(); offset += 64) {
        u32 w[64]{};
        for (u32 i = 0; i < 16; ++i) {
            w[i] = (static_cast<u32>(message[offset + i * 4]) << 24) |
                   (static_cast<u32>(message[offset + i * 4 + 1]) << 16) |
                   (static_cast<u32>(message[offset + i * 4 + 2]) << 8) |
                   static_cast<u32>(message[offset + i * 4 + 3]);
        }
        for (u32 i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3];
        u32 e = h[4], f = h[5], g = h[6], hh = h[7];
        for (u32 i = 0; i < 64; ++i) {
            const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = hh + s1 + ch + k[i] + w[i];
            const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const u32 value : h) {
        for (int i = 3; i >= 0; --i) {
            const u8 byte = static_cast<u8>((value >> (i * 8)) & 0xFF);
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0xF]);
        }
    }
    return out;
}

/// A package that validates, so a test can vary one descriptor and see the verdict change.
Package MakePackage() {
    Package package{};
    package.schema_version = SchemaVersion;
    package.provenance_id = 1;
    package.pipeline_state.present.topology = true;
    package.pipeline_state.has_topology = true;
    package.pipeline_state.topology = 3;
    package.pipeline_state.present.color_formats = true;
    package.pipeline_state.color_formats = {37};
    // The descriptor checks run under the shader-bindings readiness claim, and that claim requires
    // the binaries it depends on: a binding cannot be verified without the module that declares it.
    // Readiness is a chain, not permission to skip verification.
    package.readiness.shader_binaries_ready = true;
    package.readiness.shader_bindings_ready = true;
    // Both stages, because the binaries claim requires a vertex and a fragment stage.
    // The validator treats logical stage 3 as vertex and 0 as fragment, which is the producer's
    // own vocabulary.
    for (const u32 logical : {3u, 0u}) {
        Core::DrawPackage::ShaderStage stage{};
        stage.logical_stage = logical;
        stage.hardware_stage = logical;
        stage.program_hash = 1 + logical;
        stage.permutation_index = 0;
        // The permutation hash must be the combination of its parts, which the validator
        // recomputes.
        stage.permutation_hash = HashCombine(stage.program_hash, 0);
        stage.effective_byte_length = 4;
        stage.effective_bytes = {0x03, 0x02, 0x23, 0x07};
        stage.effective_sha256 = Sha256Hex(stage.effective_bytes);
        package.shader_stages.push_back(stage);
    }
    return package;
}

ShaderDescriptor Descriptor(u32 cls, u32 binding,
                            u32 count = 1) {
    ShaderDescriptor descriptor{};
    descriptor.set = 0;
    descriptor.binding = binding;
    descriptor.descriptor_class = cls;
    descriptor.count = count;
    descriptor.stage_flags = 1;
    descriptor.written = true;
    return descriptor;
}

void AddInterface(Package& package, const ShaderDescriptor& descriptor) {
    // Every stage needs an interface for its own permutation, so one is added per stage the fixture
    // declares. The descriptor goes on the fragment stage, which is where a sampled image or a
    // uniform buffer is normally visible.
    for (const auto& stage : package.shader_stages) {
        ShaderInterface interface{};
        interface.program_hash = stage.program_hash;
        interface.permutation_index = stage.permutation_index;
        interface.permutation_hash = stage.permutation_hash;
        interface.logical_stage = stage.logical_stage;
        if (stage.logical_stage == 0) {
            interface.descriptors.push_back(descriptor);
        }
        package.shader_interfaces.push_back(interface);
    }
}

/// The validator's own content hash, so a fixture's bytes and hash agree.
u64 Fnv1a(const std::vector<u8>& bytes) {
    u64 hash = 0xcbf29ce484222325ull;
    for (const u8 byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

void AddBuffer(Package& package, u32 binding, bool with_bytes) {
    Resource resource{};
    resource.id = 1;
    resource.clamped_size = 16;
    resource.status = 1; // HostContentVerified
    if (with_bytes) {
        resource.bytes.resize(16, 0xAB);
    }
    resource.content_hash = Fnv1a(resource.bytes);
    package.resources.push_back(resource);

    Binding record{};
    record.vk_binding = binding;
    record.resource_ref = 1;
    record.status = 1;
    package.bindings.push_back(record);
}

void AddImage(Package& package, u32 binding, bool with_content, u32 element = 0,
              u32 array_count = 1) {
    Texture texture{};
    texture.id = 1;
    texture.vk_binding = binding;
    texture.array_element = element;
    texture.array_count = array_count;
    texture.width = 4;
    texture.height = 4;
    if (with_content) {
        Core::DrawPackage::TextureSubresource sub{};
        sub.mip = 0;
        sub.layer = 0;
        sub.width = 4;
        sub.height = 4;
        sub.byte_length = 64;
        texture.subresources.push_back(sub);
    }
    package.textures.push_back(texture);
}

void AddSampler(Package& package, u32 binding) {
    Sampler sampler{};
    sampler.id = 1;
    // A sampler's identity is the binding it fills, not merely that one exists.
    sampler.vk_binding = binding;
    sampler.has_vk_binding = true;
    package.samplers.push_back(sampler);
}

TEST(DescriptorDomain, AUniformBufferWithBytesIsAccepted) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0));
    AddBuffer(package, 0, /*with_bytes=*/true);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << "validator said: " << error;
}

TEST(DescriptorDomain, AUniformBufferWithNoBytesIsRefused) {
    // A binding existing is not the same as its bytes being captured.
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0));
    AddBuffer(package, 0, /*with_bytes=*/false);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    // An earlier check catches this case with a more specific message, which is better than the
    // descriptor check reaching it. Either refusal is correct; what matters is that a resource
    // claiming verified content with no bytes is not accepted.
    const bool refused_for_bytes =
        error.find("no captured bytes") != std::string::npos ||
        error.find("retains no bytes") != std::string::npos;
    EXPECT_TRUE(refused_for_bytes) << "validator said: " << error;
}

TEST(DescriptorDomain, AUniformBufferWithNoBindingIsRefused) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0));
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("no captured binding record"), std::string::npos);
}

TEST(DescriptorDomain, ASampledImageIsResolvedInTheTextureCollection) {
    // The defect: this was looked for in `bindings` and never found.
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 1));
    AddImage(package, 1, /*with_content=*/true);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << "validator said: " << error;
}

TEST(DescriptorDomain, ASampledImageWithNoContentIsRefused) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 1));
    AddImage(package, 1, /*with_content=*/false);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("no captured image content"), std::string::npos);
}

TEST(DescriptorDomain, ASampledImageWithNoTextureIsRefused) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 1));
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
}

TEST(DescriptorDomain, AStorageImageIsResolvedInTheTextureCollection) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(4, 2));
    AddImage(package, 2, /*with_content=*/true);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << "validator said: " << error;
}

TEST(DescriptorDomain, ASamplerIsResolvedInTheSamplerCollection) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(5, 3));
    AddSampler(package, 3);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << "validator said: " << error;
}

TEST(DescriptorDomain, ASamplerWithNoCapturedSamplerIsRefused) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(5, 3));
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("no captured sampler"), std::string::npos);
}

TEST(DescriptorDomain, ACombinedImageSamplerNeedsBothHalves) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(6, 4));
    AddImage(package, 4, /*with_content=*/true);
    std::string error;
    // The image is present but the sampler is not.
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("no captured image and sampler pair"), std::string::npos);

    AddSampler(package, 4);
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << "validator said: " << error;
}

TEST(DescriptorDomain, AnUnknownDescriptorClassIsRefused) {
    // A class this validator cannot classify cannot be shown to be satisfied.
    auto package = MakePackage();
    AddInterface(package, Descriptor(99, 0));
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("unknown descriptor class"), std::string::npos);
}

TEST(DescriptorDomain, AZeroDescriptorCountIsRefused) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0, /*count=*/0));
    AddBuffer(package, 0, /*with_bytes=*/true);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("zero descriptor count"), std::string::npos);
}

TEST(DescriptorDomain, AnImageArrayNeedsEveryElementToCarryContent) {
    // A descriptor with `descriptorCount = N` occupies ONE binding with N array elements. It is not
    // N consecutive bindings. One texture with several mip levels is not an array of several
    // elements, and a duplicated element cannot stand in for a missing one.
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 1, /*count=*/2));
    AddImage(package, 1, /*with_content=*/true, /*element=*/0, /*array_count=*/2);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("array element 1"), std::string::npos) << error;

    // The second element of the same binding makes it complete.
    AddImage(package, 1, /*with_content=*/true, /*element=*/1, /*array_count=*/2);
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
}

TEST(DescriptorDomain, ADuplicatedArrayElementDoesNotSatisfyAMissingOne) {
    // Two records for element 0 are one element, not two.
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 1, /*count=*/2));
    AddImage(package, 1, /*with_content=*/true, /*element=*/0, /*array_count=*/2);
    AddImage(package, 1, /*with_content=*/true, /*element=*/0, /*array_count=*/2);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("array element 1"), std::string::npos) << error;
}

TEST(DescriptorDomain, AnArrayElementWithNoContentIsRefused) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 1, /*count=*/2));
    AddImage(package, 1, /*with_content=*/true, /*element=*/0, /*array_count=*/2);
    AddImage(package, 1, /*with_content=*/false, /*element=*/1, /*array_count=*/2);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
}

TEST(DescriptorDomain, ConsecutiveBindingsAreNotAnArray) {
    // This is the defect in its smallest form: bindings 1 and 2 with one element each is two
    // descriptors, not one two-element array at binding 1.
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 1, /*count=*/2));
    AddImage(package, 1, /*with_content=*/true, /*element=*/0, /*array_count=*/1);
    AddImage(package, 2, /*with_content=*/true, /*element=*/0, /*array_count=*/1);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("array element 1"), std::string::npos) << error;
}

TEST(DescriptorDomain, AnArrayCountThatDisagreesWithTheDeclarationIsRefused) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 1, /*count=*/2));
    AddImage(package, 1, /*with_content=*/true, /*element=*/0, /*array_count=*/4);
    AddImage(package, 1, /*with_content=*/true, /*element=*/1, /*array_count=*/4);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("array count disagrees"), std::string::npos) << error;
}

TEST(DescriptorDomain, AnArrayFollowedByAnotherBindingBothResolve) {
    // An array at binding 1 with two elements, then a separate descriptor at binding 2.
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 1, /*count=*/2));
    AddInterface(package, Descriptor(3, 2, /*count=*/1));
    AddImage(package, 1, /*with_content=*/true, /*element=*/0, /*array_count=*/2);
    AddImage(package, 1, /*with_content=*/true, /*element=*/1, /*array_count=*/2);
    AddImage(package, 2, /*with_content=*/true, /*element=*/0, /*array_count=*/1);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
}

TEST(DescriptorDomain, ASamplerAtTheWrongBindingIsRefused) {
    // An unrelated sampler from another stage or draw must not satisfy the declaration.
    auto package = MakePackage();
    AddInterface(package, Descriptor(5, 3));
    AddSampler(package, 7);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("no captured sampler at that binding"), std::string::npos) << error;
}

TEST(DescriptorDomain, ASamplerWithoutABindingIsRefused) {
    // A sampler with state but no binding identity cannot be shown to occupy the declaration.
    auto package = MakePackage();
    AddInterface(package, Descriptor(5, 3));
    Sampler sampler{};
    sampler.id = 1;
    sampler.has_vk_binding = false;
    package.samplers.push_back(sampler);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
}

TEST(DescriptorDomain, TwoSamplersAtDistinctBindingsBothResolve) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(5, 3));
    AddInterface(package, Descriptor(5, 4));
    AddSampler(package, 3);
    AddSampler(package, 4);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
}

TEST(DescriptorDomain, ACompletePackageWithEveryClassValidates) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0));
    AddInterface(package, Descriptor(3, 1));
    AddInterface(package, Descriptor(5, 2));
    AddBuffer(package, 0, /*with_bytes=*/true);
    AddImage(package, 1, /*with_content=*/true);
    AddSampler(package, 2);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << "validator said: " << error;
}

TEST(DescriptorDomain, OneMissingClassRefusesTheWholePackage) {
    // Negative control: a package with three satisfied classes and one unsatisfied is not ready.
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0));
    AddInterface(package, Descriptor(3, 1));
    AddInterface(package, Descriptor(5, 2));
    AddBuffer(package, 0, /*with_bytes=*/true);
    AddImage(package, 1, /*with_content=*/true);
    // No sampler.
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
}

} // namespace
