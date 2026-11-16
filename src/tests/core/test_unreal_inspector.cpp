// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <string>

#include "core/unreal/inspector.h"
#include "tests/core/unreal_fixture.h"

namespace Core::Unreal {
namespace {

ScanRanges RangesOf(const Core::Unreal::Test::Image& image) {
    ScanRanges ranges;
    for (const auto& region : image.Regions()) {
        ranges.all.push_back({region.start, region.start + region.size});
        if (region.name == "executable" && (region.prot & 2) != 0) {
            ranges.writable.push_back({region.start, region.start + region.size});
        }
    }
    return ranges;
}

class FixtureInspector : public ::testing::Test {
protected:
    void SetUp() override {
        image = std::make_unique<Core::Unreal::Test::Image>();
        inspector = std::make_unique<Inspector>(*image, RangesOf(*image));
        inspector->Discover();
    }

    std::unique_ptr<Core::Unreal::Test::Image> image;
    std::unique_ptr<Inspector> inspector;
};

TEST_F(FixtureInspector, FindsTheNameTableAndObjectArray) {
    const auto& result = inspector->Result();
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.name_table_kind, "FNamePool");
    EXPECT_EQ(result.object_array_kind, "FChunkedFixedUObjectArray");
    EXPECT_EQ(result.names_address, image->NamePoolAddress());
    EXPECT_EQ(result.objects_address, image->ObjectArrayAddress());
    EXPECT_EQ(result.object_count, Core::Unreal::Test::Image::ObjectCount);
    EXPECT_EQ(inspector->ObjectCount(), Core::Unreal::Test::Image::ObjectCount);
}

TEST_F(FixtureInspector, ReadsObjectNamesAndClasses) {
    const std::array<std::pair<const char*, const char*>, 8> expected{{
        {"Class", "Class"},
        {"World", "Class"},
        {"Pawn", "Class"},
        {"PlayerController", "Class"},
        {"SkeletalMeshComponent", "Class"},
        {"HandMeshLeft", "SkeletalMeshComponent"},
        {"HandMeshRight", "SkeletalMeshComponent"},
        {"HandAnimBlueprint", "Class"},
    }};

    for (u32 index = 0; index < expected.size(); ++index) {
        ObjectInfo info;
        ASSERT_TRUE(inspector->GetObject(index, info)) << "object " << index;
        EXPECT_EQ(info.name, expected[index].first) << "object " << index;
        EXPECT_EQ(info.class_name, expected[index].second) << "object " << index;
        EXPECT_EQ(info.index, index);
        EXPECT_EQ(info.address, image->ObjectAddress(index));
    }
}

TEST_F(FixtureInspector, SearchesByNameAndClass) {
    const auto hands = inspector->SearchByName("HandMesh", 16);
    ASSERT_EQ(hands.size(), 2u);
    EXPECT_EQ(hands[0].name, "HandMeshLeft");
    EXPECT_EQ(hands[1].name, "HandMeshRight");

    // Class search matches the object's class, not its name: the hands are instances of a
    // component class, while "PlayerController" appears here only as an instance name.
    const auto components = inspector->SearchByClass("SkeletalMeshComponent", 16);
    ASSERT_EQ(components.size(), 2u);
    EXPECT_EQ(components[0].name, "HandMeshLeft");
    EXPECT_TRUE(inspector->SearchByClass("PlayerController", 16).empty());

    EXPECT_TRUE(inspector->SearchByName("NoSuchObject", 16).empty());
}

TEST_F(FixtureInspector, FollowsValidatedReferences) {
    ObjectInfo hand;
    ASSERT_TRUE(inspector->GetObject(5, hand));

    ObjectInfo outer;
    ASSERT_TRUE(inspector->FollowReference(hand, Core::Unreal::Test::Image::ObjectOuter, outer));
    EXPECT_EQ(outer.name, "PlayerController");
    EXPECT_EQ(outer.address, image->ObjectAddress(3));

    // A zero pointer is not a reference, and neither is a pointer to unmapped guest memory.
    ObjectInfo none;
    ASSERT_TRUE(inspector->GetObject(0, none));
    EXPECT_FALSE(inspector->FollowReference(none, Core::Unreal::Test::Image::ObjectOuter, outer));

    image->SetObjectOuter(5, 0x900000000);
    ASSERT_TRUE(inspector->GetObject(5, hand));
    EXPECT_FALSE(inspector->FollowReference(hand, Core::Unreal::Test::Image::ObjectOuter, outer));
}

TEST_F(FixtureInspector, DetectsRecycledSlots) {
    ObjectInfo hand;
    ASSERT_TRUE(inspector->GetObject(5, hand));
    EXPECT_TRUE(inspector->IsLive(5, hand));

    // The slot now holds an object that reports another index: reading it must fail, not mislead.
    image->SetObjectIndex(5, 9);
    ObjectInfo stale;
    EXPECT_FALSE(inspector->GetObject(5, stale));
    EXPECT_FALSE(inspector->IsLive(5, hand));

    image->SetObjectIndex(5, 5);
    EXPECT_TRUE(inspector->GetObject(5, stale));
    EXPECT_TRUE(inspector->IsLive(5, hand));
}

TEST_F(FixtureInspector, ReportsMissingTablesWithoutCrashing) {
    std::vector<u8> zeros(0x2000, 0);
    class Buffer final : public GuestMemory {
    public:
        explicit Buffer(std::vector<u8> bytes) : bytes{std::move(bytes)} {}
        bool Read(VAddr address, void* out, u64 size) const override {
            if (address < 0x100000 || address + size > 0x100000 + bytes.size()) {
                return false;
            }
            std::memcpy(out, bytes.data() + (address - 0x100000), size);
            return true;
        }

    private:
        std::vector<u8> bytes;
    } buffer{zeros};

    const ScanRanges ranges{{{{0x100000, 0x102000}}}, {{{0x100000, 0x102000}}}};
    Inspector empty{buffer, ranges};
    const auto& result = empty.Discover();
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.names_found);
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(empty.ObjectCount(), 0u);
}

TEST_F(FixtureInspector, RefusesReadsOutsideTheScannedRanges) {
    const ScanRanges ranges{{{{Core::Unreal::Test::Image::Base, Core::Unreal::Test::Image::Base + 0x400}}},
                            {{{Core::Unreal::Test::Image::Base, Core::Unreal::Test::Image::Base + 0x400}}}};
    Inspector narrow{*image, ranges};
    const auto& result = narrow.Discover();
    EXPECT_FALSE(result.objects_found);
    ObjectInfo info;
    EXPECT_FALSE(narrow.GetObject(0, info));
}

} // Anonymous namespace
} // namespace Core::Unreal
