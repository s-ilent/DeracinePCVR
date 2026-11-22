// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/draw_package.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using Core::DrawPackage::Binding;
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

    package.replay_ready = true;
    return package;
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

} // namespace
