// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "core/memory.h"
#include "core/unreal/guest_memory.h"
#include "core/unreal/snapshot.h"

namespace Core::Unreal {
namespace {

u32 ToPs4Prot(MemoryProt prot) {
    u32 ps4 = 0;
    if (True(prot & MemoryProt::CpuRead)) {
        ps4 |= 1;
    }
    if (True(prot & MemoryProt::CpuWrite)) {
        ps4 |= 2;
    }
    if (True(prot & MemoryProt::CpuExec)) {
        ps4 |= 4;
    }
    return ps4;
}

} // Anonymous namespace

std::vector<SnapshotRegion> EnumerateLiveRegions(const std::string& executable_vma_name,
                                                 const std::string& executable_name) {
    std::vector<SnapshotRegion> regions;
    auto* memory = Core::Memory::Instance();
    if (memory == nullptr) {
        return regions;
    }
    for (const auto& mapped : memory->EnumerateMappedRegions()) {
        const u32 prot = ToPs4Prot(mapped.prot);
        if ((prot & 1) == 0) {
            continue;
        }
        SnapshotRegion region;
        region.start = mapped.address;
        region.size = mapped.size;
        region.prot = prot;
        region.name = mapped.name == executable_vma_name ? executable_name : mapped.name;
        regions.push_back(std::move(region));
    }
    std::ranges::sort(regions, {}, &SnapshotRegion::start);
    return regions;
}

bool ReadLiveRegion(VAddr address, void* out, u64 size) {
    static const LiveGuestMemory memory;
    return memory.Read(address, out, size);
}

} // namespace Core::Unreal
