#include "common/resource_registry.h"

#include <algorithm>

namespace harness {

ResourceId ResourceRegistry::Create(void* backendObject,
                                    std::function<void(void*)> destroyFn) {
    const uint64_t index = nextIndex_++;
    ResourceRecord rec;
    rec.id = ResourceId{index, 1};
    rec.state = ResourceState::kLive;
    rec.appOwned = true;
    rec.backendObject = backendObject;
    rec.destroyFn = std::move(destroyFn);
    records_[index] = std::move(rec);
    return records_[index].id;
}

ResourceRecord* ResourceRegistry::Resolve(ResourceId id) {
    auto it = records_.find(id.index);
    if (it == records_.end()) return nullptr;
    if (it->second.id.generation != id.generation) return nullptr;
    if (it->second.state == ResourceState::kDestroyed) return nullptr;
    return &it->second;
}

const ResourceRecord* ResourceRegistry::Resolve(ResourceId id) const {
    auto it = records_.find(id.index);
    if (it == records_.end()) return nullptr;
    if (it->second.id.generation != id.generation) return nullptr;
    if (it->second.state == ResourceState::kDestroyed) return nullptr;
    return &it->second;
}

bool ResourceRegistry::IsAlive(ResourceId id) const { return Resolve(id) != nullptr; }

bool ResourceRegistry::CanRecord(ResourceId id) const {
    const ResourceRecord* r = Resolve(id);
    // A resource pending destruction must not accept new GPU work, even though
    // it is still physically alive for the submissions already in flight.
    return r != nullptr && r->state == ResourceState::kLive;
}

bool ResourceRegistry::RetainForGpu(ResourceId id, uint32_t domain, uint64_t value) {
    ResourceRecord* r = Resolve(id);
    if (r == nullptr) return false;
    if (r->state != ResourceState::kLive) return false;

    // One entry per (domain, value). Re-retaining the same submission is a
    // no-op rather than a duplicate, so a resource referenced twice by one
    // command buffer is released once.
    for (const auto& ref : r->gpuRefs) {
        if (ref.domain == domain && ref.value == value) return true;
    }
    r->gpuRefs.push_back(ResourceRecord::GpuRef{domain, value});
    return true;
}

int ResourceRegistry::CompleteDomain(uint32_t domain, uint64_t value) {
    int destroyed = 0;
    for (auto& kv : records_) {
        ResourceRecord& r = kv.second;
        if (r.state == ResourceState::kDestroyed) continue;

        // Release every reference in this domain at or below the completed value.
        const size_t before = r.gpuRefs.size();
        r.gpuRefs.erase(
            std::remove_if(r.gpuRefs.begin(), r.gpuRefs.end(),
                           [&](const ResourceRecord::GpuRef& ref) {
                               return ref.domain == domain && ref.value <= value;
                           }),
            r.gpuRefs.end());

        if (r.gpuRefs.size() != before) {
            if (MaybeDestroy(kv.first)) ++destroyed;
        }
    }
    return destroyed;
}

bool ResourceRegistry::Destroy(ResourceId id) {
    ResourceRecord* r = Resolve(id);
    if (r == nullptr) return false;

    // Revoke application ownership. The resource may still be referenced by
    // GPU work that has already been submitted.
    r->appOwned = false;
    if (r->state == ResourceState::kLive) r->state = ResourceState::kPendingDestroy;

    MaybeDestroy(id.index);
    return true;
}

void ResourceRegistry::AbandonGpuRefs(ResourceId id, uint32_t domain, uint64_t value) {
    ResourceRecord* r = Resolve(id);
    if (r == nullptr) return;
    r->gpuRefs.erase(std::remove_if(r->gpuRefs.begin(), r->gpuRefs.end(),
                                    [&](const ResourceRecord::GpuRef& ref) {
                                        return ref.domain == domain && ref.value == value;
                                    }),
                     r->gpuRefs.end());
    MaybeDestroy(id.index);
}

int ResourceRegistry::DestroyAll() {
    int destroyed = 0;
    for (auto& kv : records_) {
        ResourceRecord& r = kv.second;
        if (r.state == ResourceState::kDestroyed) continue;
        r.appOwned = false;
        r.gpuRefs.clear();
        if (r.destroyFn && r.backendObject != nullptr) {
            r.destroyFn(r.backendObject);
        }
        r.backendObject = nullptr;
        r.state = ResourceState::kDestroyed;
        r.id.generation++;   // invalidate the old identity
        ++destroyed;
    }
    return destroyed;
}

int ResourceRegistry::LiveCount() const {
    int n = 0;
    for (const auto& kv : records_) {
        if (kv.second.state == ResourceState::kLive) ++n;
    }
    return n;
}

int ResourceRegistry::PendingDestroyCount() const {
    int n = 0;
    for (const auto& kv : records_) {
        if (kv.second.state == ResourceState::kPendingDestroy) ++n;
    }
    return n;
}

int ResourceRegistry::TotalGpuRefs() const {
    int n = 0;
    for (const auto& kv : records_) n += static_cast<int>(kv.second.gpuRefs.size());
    return n;
}

ResourceState ResourceRegistry::StateOf(ResourceId id) const {
    const ResourceRecord* r = Resolve(id);
    return r == nullptr ? ResourceState::kDestroyed : r->state;
}

bool ResourceRegistry::MaybeDestroy(uint64_t index) {
    auto it = records_.find(index);
    if (it == records_.end()) return false;
    ResourceRecord& r = it->second;
    if (r.state == ResourceState::kDestroyed) return false;

    // Both conditions must hold: no application ownership, no GPU references.
    if (r.appOwned) return false;
    if (!r.gpuRefs.empty()) return false;

    if (r.destroyFn && r.backendObject != nullptr) {
        r.destroyFn(r.backendObject);
    }
    r.backendObject = nullptr;
    r.state = ResourceState::kDestroyed;
    r.id.generation++;   // a reused slot must not resolve the old id
    return true;
}

}  // namespace harness
