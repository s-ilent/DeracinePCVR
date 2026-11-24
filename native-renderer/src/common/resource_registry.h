// A resource registry with correct ownership semantics.
//
// The distinction this file exists to enforce:
//
//   GPU submission completion releases a GPU reference.
//   It does NOT destroy an application-owned resource.
//
// A resource therefore has two independent reasons to stay alive:
//   1. the application still owns it (Destroy has not been called), and
//   2. one or more GPU submissions still reference it.
//
// Physical destruction happens only when both are gone.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace harness {

// A generation-tagged identity. A raw pointer is not a stable identity because
// an allocator can reuse an address after destruction.
struct ResourceId {
    uint64_t index = 0;
    uint64_t generation = 0;

    bool operator==(const ResourceId& o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const ResourceId& o) const { return !(*this == o); }
    bool IsValid() const { return index != 0 || generation != 0; }
};

// Why a resource is still alive.
enum class ResourceState {
    kLive,               // application owns it, no pending destruction
    kPendingDestroy,     // Destroy called, GPU references remain
    kDestroyed,          // physically gone, slot reusable
};

// Per-resource bookkeeping.
struct ResourceRecord {
    ResourceId id;
    ResourceState state = ResourceState::kLive;

    // Application ownership. Cleared by Destroy().
    bool appOwned = true;

    // GPU references, keyed by completion domain and submission value.
    // A resource is retained until every entry here has completed.
    struct GpuRef {
        uint32_t domain = 0;
        uint64_t value = 0;
    };
    std::vector<GpuRef> gpuRefs;

    // Backend payload. Opaque to the registry.
    void* backendObject = nullptr;

    // Called exactly once, when the resource is physically destroyed.
    std::function<void(void*)> destroyFn;
};

// Tracks resources across one or more independent completion domains.
//
// Domains model independently progressing queues. A resource referenced by
// submissions on two queues is not destroyed until both have completed.
class ResourceRegistry {
public:
    // Creates a resource. The caller owns it.
    ResourceId Create(void* backendObject, std::function<void(void*)> destroyFn);

    // Resolves an id to its record, or nullptr if stale/destroyed.
    ResourceRecord* Resolve(ResourceId id);
    const ResourceRecord* Resolve(ResourceId id) const;

    // True when the id resolves to a live or pending-destroy record.
    bool IsAlive(ResourceId id) const;

    // True when new GPU work may reference this resource.
    // False once Destroy has been called.
    bool CanRecord(ResourceId id) const;

    // Records that GPU work in 'domain' at 'value' references this resource.
    // Returns false if the resource cannot be referenced (destroyed or stale).
    bool RetainForGpu(ResourceId id, uint32_t domain, uint64_t value);

    // Reports that 'domain' reached 'value'. Releases every GPU reference at or
    // below it, and physically destroys resources that are pending and now
    // unreferenced.
    // Returns the number of resources physically destroyed.
    int CompleteDomain(uint32_t domain, uint64_t value);

    // Revokes application ownership. Physical destruction is deferred while
    // GPU references remain. Returns false if the id is stale.
    bool Destroy(ResourceId id);

    // Releases GPU references held by a recording that was abandoned before
    // submission. Does not destroy the resource if the application owns it.
    void AbandonGpuRefs(ResourceId id, uint32_t domain, uint64_t value);

    // Physically destroys everything, regardless of outstanding references.
    // Only for shutdown after the device is idle. Returns the count.
    int DestroyAll();

    // Diagnostics.
    int LiveCount() const;
    int PendingDestroyCount() const;
    int TotalGpuRefs() const;
    ResourceState StateOf(ResourceId id) const;

private:
    bool MaybeDestroy(uint64_t index);

    std::unordered_map<uint64_t, ResourceRecord> records_;
    uint64_t nextIndex_ = 1;   // 0 is reserved for an invalid id
};

}  // namespace harness
