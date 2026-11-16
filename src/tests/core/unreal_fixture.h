// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/unreal/guest_memory.h"

namespace Core::Unreal::Test {

// A synthetic guest image laid out like the structures the offline dumper looks for: an
// FNamePool whose block 0 starts with "None"/"ByteProperty", a chunked FChunkedFixedUObjectArray,
// and UObjects whose class, name and outer fields are filled in. It is not a real game: it exists
// so snapshot and discovery code can be exercised without a running title.
class Image final : public GuestMemory {
public:
    static constexpr VAddr Base = 0x800000000;

    static constexpr VAddr NamePoolOffset = 0x1000;
    static constexpr VAddr NameBlockOffset = 0x2000;
    static constexpr VAddr ObjectArrayOffset = 0x3000;
    static constexpr VAddr ChunkListOffset = 0x4000;
    static constexpr VAddr ItemChunkOffset = 0x8000;
    static constexpr VAddr ObjectPoolOffset = 0x100000;
    static constexpr u64 ImageSize = 0x200000;

    static constexpr u32 PerChunk = 0x8000;
    static constexpr u32 MaxChunks = 8;
    static constexpr u32 MaxObjects = PerChunk * MaxChunks;
    static constexpr u32 ObjectCount = 0x900;
    static constexpr u64 ItemSize = 16;
    static constexpr u64 ObjectStride = 0x100;

    // Offsets inside a UObject, as the offline tools use them.
    static constexpr u64 ObjectFlags = 0x08;
    static constexpr u64 ObjectIndex = 0x0C;
    static constexpr u64 ObjectClass = 0x10;
    static constexpr u64 ObjectName = 0x18;
    static constexpr u64 ObjectNumber = 0x1C;
    static constexpr u64 ObjectOuter = 0x20;

    struct Region {
        VAddr start;
        u64 size;
        u32 prot;
        std::string name;
    };

    Image() : bytes(ImageSize, 0) {
        regions.push_back({Base, 0x1E0000, 3, "executable"});
        // A second, read-only mapping with a gap before it, to exercise sparse captures.
        regions.push_back({Base + 0x1F0000, 0x1000, 1, "anon"});
        BuildNames();
        BuildObjectArray();
        BuildObjects();
    }

    const std::vector<Region>& Regions() const {
        return regions;
    }

    u64 Size() const {
        return bytes.size();
    }

    bool Read(VAddr address, void* out, u64 size) const override {
        const auto* source = Find(address, size);
        if (!source) {
            return false;
        }
        std::memcpy(out, source, size);
        return true;
    }

    VAddr NamePoolAddress() const {
        return Base + NamePoolOffset;
    }

    VAddr ObjectArrayAddress() const {
        return Base + ObjectArrayOffset;
    }

    VAddr ObjectAddress(u32 index) const {
        return Base + ObjectPoolOffset + index * ObjectStride;
    }

    // Name indices, in the order BuildNames() writes them.
    enum NameIndex : u32 {
        None = 0,
        ByteProperty = 1,
        Class = 2,
        Object = 3,
        World = 4,
        Pawn = 5,
        PlayerController = 6,
        SkeletalMeshComponent = 7,
        AnimBlueprintGeneratedClass = 8,
        SkeletalMesh = 9,
        Skeleton = 10,
        HandMeshLeft = 11,
        HandMeshRight = 12,
        HandAnimBlueprint = 13,
        Count = 14,
    };

    static const char* NameText(u32 index) {
        switch (index) {
        case None:
            return "None";
        case ByteProperty:
            return "ByteProperty";
        case Class:
            return "Class";
        case Object:
            return "Object";
        case World:
            return "World";
        case Pawn:
            return "Pawn";
        case PlayerController:
            return "PlayerController";
        case SkeletalMeshComponent:
            return "SkeletalMeshComponent";
        case AnimBlueprintGeneratedClass:
            return "AnimBlueprintGeneratedClass";
        case SkeletalMesh:
            return "SkeletalMesh";
        case Skeleton:
            return "Skeleton";
        case HandMeshLeft:
            return "HandMeshLeft";
        case HandMeshRight:
            return "HandMeshRight";
        case HandAnimBlueprint:
            return "HandAnimBlueprint";
        default:
            return "";
        }
    }

    // FName index of a name, as the name pool assigned it (byte offset in block 0 divided by 2).
    u32 NameIndexOf(u32 name_index) const {
        return name_indices[name_index];
    }

    // The object table: every entry has a name index and the class it is an instance of.
    struct ObjectDesc {
        u32 name_index;
        u32 class_index;
        u32 flags;
    };

    const std::vector<ObjectDesc>& Objects() const {
        return objects;
    }

private:
    const u8* Find(VAddr address, u64 size) const {
        for (const auto& region : regions) {
            if (address >= region.start && address + size <= region.start + region.size) {
                return bytes.data() + (address - Base);
            }
        }
        return nullptr;
    }

    template <typename T>
    void Store(VAddr address, T value) {
        std::memcpy(bytes.data() + (address - Base), &value, sizeof(T));
    }

    void StoreString(VAddr address, const char* text) {
        std::memcpy(bytes.data() + (address - Base), text, std::strlen(text) + 1);
    }

    void BuildNames() {
        const VAddr pool = Base + NamePoolOffset;
        const VAddr block = Base + NameBlockOffset;
        // {Lock, CurrentBlock, CurrentByteCursor, Blocks[8192]}
        Store<u64>(pool, 0);
        Store<u32>(pool + 8, 0);
        Store<u64>(pool + 0x10, block);

        // An FName index is the entry's byte offset into the block divided by two, so the enum
        // order here is not the index order: each entry is recorded as it is laid out.
        u32 cursor = 0;
        for (u32 index = 0; index < Count; ++index) {
            const char* text = NameText(index);
            const auto length = static_cast<u16>(std::strlen(text));
            const u16 header = static_cast<u16>(length << 6);
            Store<u16>(block + cursor, header);
            StoreString(block + cursor + 2, text);
            name_indices[index] = cursor / 2;
            cursor += static_cast<u32>(2 + length);
            cursor = (cursor + 1) & ~1u;
        }
        Store<u32>(pool + 12, cursor);
    }

    void BuildObjectArray() {
        const VAddr array = Base + ObjectArrayOffset;
        const VAddr chunk_list = Base + ChunkListOffset;
        const VAddr chunk0 = Base + ItemChunkOffset;
        // FChunkedFixedUObjectArray: objects, max, num, max_chunks, num_chunks.
        Store<u64>(array + 0x00, chunk_list);
        Store<u32>(array + 0x10, MaxObjects);
        Store<u32>(array + 0x14, ObjectCount);
        Store<u32>(array + 0x18, MaxChunks);
        Store<u32>(array + 0x1C, 1);
        Store<u64>(chunk_list, chunk0);
    }

    void BuildObjects() {
        // class_index is the index of the class object in this table, not an FName.
        objects = {
            {Class, 0, 0},
            {World, 0, 0},
            {Pawn, 0, 0},
            {PlayerController, 0, 0},
            {SkeletalMeshComponent, 0, 0},
            {HandMeshLeft, 4, 0},
            {HandMeshRight, 4, 0},
            {HandAnimBlueprint, 0, 0},
        };

        // The rest of the array is filled with plain Objects, named with an FName number, so the
        // table looks like a populated game array to the offline tools' plausibility checks.
        while (objects.size() < ObjectCount) {
            objects.push_back({Object, 0, 0});
        }

        const VAddr chunk0 = Base + ItemChunkOffset;
        for (u32 index = 0; index < objects.size(); ++index) {
            const VAddr object = ObjectAddress(index);
            Store<u64>(object, 0);
            Store<u32>(object + ObjectFlags, objects[index].flags);
            Store<u32>(object + ObjectIndex, index);
            Store<u64>(object + ObjectClass, ObjectAddress(objects[index].class_index));
            Store<u32>(object + ObjectName, name_indices[objects[index].name_index]);
            Store<u32>(object + ObjectNumber, index < 8 ? 0 : index);
            Store<u64>(object + ObjectOuter, 0);
            // FUObjectItem: the UObject pointer first.
            Store<u64>(chunk0 + index * ItemSize, object);
        }
    }

    std::vector<u8> bytes;
    std::vector<Region> regions;
    std::vector<ObjectDesc> objects;
    std::array<u32, Count> name_indices{};
};

} // namespace Core::Unreal::Test
