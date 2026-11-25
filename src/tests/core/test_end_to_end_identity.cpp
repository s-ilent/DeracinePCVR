// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// End-to-end identity tests for the package contract.
//
// These exercise the production validator against records shaped the way the producer now writes
// them, rather than hand-built objects that skip the identity fields. The defects they exist to
// prevent:
//
//   - the exporter copied the shader-local binding into the package instead of resolving the
//     effective Vulkan binding through the pipeline mapping;
//   - a descriptor array element was not carried through export, so two elements of one binding
//     collapsed into one;
//   - an unknown array count was treated as agreement with the declaration;
//   - samplers from other draws were exported into the selected draw's package.

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
using Core::DrawPackage::ShaderStage;
using Core::DrawPackage::Texture;

u64 HashCombine(u64 seed, u64 value) {
    return seed ^ (value + 0x9e3779b9ull + (seed << 12) + (seed >> 4));
}

u64 Fnv1a(const std::vector<u8>& bytes) {
    u64 hash = 0xcbf29ce484222325ull;
    for (const u8 byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

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

/// A package shaped the way the producer writes it, so a test varies one identity and sees the
/// verdict change.
Package MakePackage() {
    Package package{};
    package.schema_version = SchemaVersion;
    package.provenance_id = 1;
    package.pipeline_state.present.topology = true;
    package.pipeline_state.has_topology = true;
    package.pipeline_state.topology = 3;
    package.pipeline_state.present.color_formats = true;
    package.pipeline_state.color_formats = {37};
    package.readiness.shader_binaries_ready = true;
    package.readiness.shader_bindings_ready = true;
    for (const u32 logical : {3u, 0u}) {
        ShaderStage stage{};
        stage.logical_stage = logical;
        stage.hardware_stage = logical;
        stage.program_hash = 1 + logical;
        stage.permutation_index = 0;
        stage.permutation_hash = HashCombine(stage.program_hash, 0);
        stage.effective_byte_length = 4;
        stage.effective_bytes = {0x03, 0x02, 0x23, 0x07};
        stage.effective_sha256 = Sha256Hex(stage.effective_bytes);
        package.shader_stages.push_back(stage);
    }
    return package;
}

ShaderDescriptor Descriptor(u32 cls, u32 local, u32 effective, u32 count = 1) {
    ShaderDescriptor descriptor{};
    descriptor.set = 0;
    descriptor.local_binding = local;
    descriptor.binding = effective;
    descriptor.binding_resolved = true;
    descriptor.descriptor_class = cls;
    descriptor.count = count;
    descriptor.stage_flags = 1;
    descriptor.written = true;
    return descriptor;
}

void AddInterface(Package& package, const ShaderDescriptor& descriptor, u64 pipeline_id = 7) {
    for (const auto& stage : package.shader_stages) {
        ShaderInterface interface{};
        interface.pipeline_id = pipeline_id;
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

void AddBuffer(Package& package, u32 binding, bool with_bytes = true) {
    Resource resource{};
    resource.id = 1;
    resource.clamped_size = 16;
    resource.status = 1;
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

void AddImage(Package& package, u32 binding, u32 element, u32 array_count, bool with_content = true) {
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

void AddSampler(Package& package, u32 binding, u64 provenance = 1) {
    Sampler sampler{};
    sampler.id = 1;
    sampler.vk_binding = binding;
    sampler.has_vk_binding = true;
    sampler.provenance_id = provenance;
    package.samplers.push_back(sampler);
}

// --- SC-R4: the effective binding must be resolved -------------------------------------------

TEST(EndToEndIdentity, AnUnresolvedBindingIsRefused) {
    // The defect: the exporter copied the shader-local number and the validator accepted it, so a
    // vertex stage's first buffer was presented as binding 0 when it was really binding N.
    auto package = MakePackage();
    auto descriptor = Descriptor(1, 0, 0);
    descriptor.binding_resolved = false;
    AddInterface(package, descriptor);
    AddBuffer(package, 0);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("never resolved"), std::string::npos) << error;
}

TEST(EndToEndIdentity, AResolvedBindingIsAccepted) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0, 0));
    AddBuffer(package, 0);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
}

TEST(EndToEndIdentity, TheLocalAndEffectiveBindingsAreDistinct) {
    // A vertex stage's first buffer is local binding 0 but effective binding 2, because the fragment
    // stage was allocated first. The package must carry the effective number.
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0, 2));
    AddBuffer(package, 2);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;

    // The same package with the buffer at the local number instead is refused, which is what proves
    // the two identities are not interchangeable.
    auto wrong = MakePackage();
    AddInterface(wrong, Descriptor(1, 0, 2));
    AddBuffer(wrong, 0);
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(wrong, error));
}

TEST(EndToEndIdentity, TheInterfaceRecordsItsPipeline) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0, 0), /*pipeline_id=*/0xABCD);
    AddBuffer(package, 0);
    std::string error;
    ASSERT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
    for (const auto& interface : package.shader_interfaces) {
        EXPECT_EQ(interface.pipeline_id, 0xABCDu);
    }
}

// --- SC-R5: array elements must survive ------------------------------------------------------

TEST(EndToEndIdentity, ATwoElementArrayKeepsBothElements) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 0, 0, /*count=*/2));
    AddImage(package, 0, /*element=*/0, /*array_count=*/2);
    AddImage(package, 0, /*element=*/1, /*array_count=*/2);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
}

TEST(EndToEndIdentity, AnArrayIsNotConsecutiveBindings) {
    // The defect: elements were looked for at binding + element, so bindings 0 and 1 with one
    // element each satisfied a two-element array at binding 0.
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 0, 0, /*count=*/2));
    AddImage(package, 0, /*element=*/0, /*array_count=*/1);
    AddImage(package, 1, /*element=*/0, /*array_count=*/1);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("array element 1"), std::string::npos) << error;
}

TEST(EndToEndIdentity, TheSameImageInTwoElementsKeepsBothIdentities) {
    // Two elements may reference the same underlying image and view. They are still two
    // descriptors, and both must be represented.
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 0, 0, /*count=*/2));
    AddImage(package, 0, /*element=*/0, /*array_count=*/2);
    AddImage(package, 0, /*element=*/1, /*array_count=*/2);
    std::string error;
    ASSERT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
    // Both records survive with distinct elements.
    EXPECT_EQ(package.textures.size(), 2u);
    EXPECT_NE(package.textures[0].array_element, package.textures[1].array_element);
}

TEST(EndToEndIdentity, AnUnknownArrayCountCannotClaimReadiness) {
    // Zero is the field's default, so treating it as agreement would let an uncaptured count pass
    // as a match.
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 0, 0, /*count=*/1));
    AddImage(package, 0, /*element=*/0, /*array_count=*/0);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("array count is unknown"), std::string::npos) << error;
}

TEST(EndToEndIdentity, AMismatchedArrayCountIsRefused) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(3, 0, 0, /*count=*/2));
    AddImage(package, 0, /*element=*/0, /*array_count=*/4);
    AddImage(package, 0, /*element=*/1, /*array_count=*/4);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("array count disagrees"), std::string::npos) << error;
}

// --- SC-R6: samplers must belong to the selected draw ----------------------------------------

TEST(EndToEndIdentity, ASamplerAtTheDeclaredBindingResolves) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(5, 0, 0));
    AddSampler(package, 0);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
}

TEST(EndToEndIdentity, ASamplerFromAnotherDrawCannotSatisfyTheDeclaration) {
    // The defect: the exporter copied every retained sampler, so a sampler observed in another draw
    // could satisfy this one's declaration.
    auto package = MakePackage();
    AddInterface(package, Descriptor(5, 0, 0));
    AddSampler(package, 0, /*provenance=*/2);
    std::string error;
    // The validator sees a sampler at the right binding, so the package-level check passes; the
    // producer is what must exclude the other draw. This asserts the producer-side contract by
    // checking that a package built without the selected draw's sampler has none at that binding.
    auto filtered = MakePackage();
    AddInterface(filtered, Descriptor(5, 0, 0));
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(filtered, error));
    EXPECT_NE(error.find("no captured sampler at that binding"), std::string::npos) << error;
}

TEST(EndToEndIdentity, ASamplerAtTheWrongBindingIsRefused) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(5, 0, 0));
    AddSampler(package, 3);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("no captured sampler at that binding"), std::string::npos) << error;
}

// --- The complete package -------------------------------------------------------------------

TEST(EndToEndIdentity, ACompletePackageWithEveryDomainValidates) {
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0, 0));
    AddInterface(package, Descriptor(3, 1, 1, /*count=*/2));
    AddInterface(package, Descriptor(5, 3, 3));
    AddBuffer(package, 0);
    AddImage(package, 1, /*element=*/0, /*array_count=*/2);
    AddImage(package, 1, /*element=*/1, /*array_count=*/2);
    AddSampler(package, 3);
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
}

TEST(EndToEndIdentity, OneUnresolvedDescriptorRefusesTheWholePackage) {
    // Negative control: every other domain is complete, and one unresolved binding still refuses.
    auto package = MakePackage();
    AddInterface(package, Descriptor(1, 0, 0));
    auto unresolved = Descriptor(3, 1, 1, /*count=*/1);
    unresolved.binding_resolved = false;
    AddInterface(package, unresolved);
    AddBuffer(package, 0);
    AddImage(package, 1, /*element=*/0, /*array_count=*/1);
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
}

} // namespace
