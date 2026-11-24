// A resource registry with correct ownership semantics.
//
// The distinction this file exists to enforce:
//
//   GPU submission completion releases a GPU reference.
//   It does NOT destroy an application-owned resource.
//
// A resource therefore has several independent reasons to stay alive:
//   1. the application still owns it (Destroy has not been called),
//   2. a command buffer being recorded references it,
//   3. one or more submitted GPU submissions reference it, and
//   4. a dependent resource (e.g. a view's parent texture) needs it.
//
// Physical destruction happens only when all of them are gone.
//
// This registry is the single authority for resource identity. Backend payloads
// live behind it, so an index-only lookup cannot reach a freed object.
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
    bool IsValid() const { return index != 0; }
};

// The concrete kind of a resource. A lookup that asks for the wrong type must
// fail rather than reinterpret the payload.
enum class ResourceType {
    kUnknown = 0,
    kBuffer,
    kTexture,
    kTextureView,
    kShader,
    kPipeline,
};

const char* ResourceTypeName(ResourceType t);

// Why a resource is still alive.
enum class ResourceState {
    kLive,               // application owns it, no pending destruction
    kPendingDestroy,     // Destroy called, references remain
    kDestroyed,          // physically gone, slot reusable
};

// Reserved reference domains. Real GPU completion domains are small integers.
constexpr uint32_t kRecordingDomain = 0xFFFFFFFEu;
constexpr uint32_t kDependencyDomain = 0xFFFFFFFFu;

// Why a lookup failed. Reported to the caller instead of a silent no-op.
enum class LookupError {
    kOk = 0,
    kInvalidId,          // index 0 or never allocated
    kStaleGeneration,    // index reused by a newer resource
    kWrongType,          // payload is a different resource kind
    kDestroyed,          // physically gone
    kPendingDestroy,     // exists but accepts no new work
    kNotRecording,       // operation requires an active recording
};

const char* LookupErrorName(LookupError e);

// Per-resource bookkeeping.
struct ResourceRecord {
    ResourceId id;
    ResourceType type = ResourceType::kUnknown;
    ResourceState state = ResourceState::kLive;

    // Application ownership. Cleared by Destroy().
    bool appOwned = true;

    // References, keyed by domain and value. A resource is retained until every
    // entry here has been released.
    struct Ref {
        uint32_t domain = 0;
        uint64_t value = 0;
    };
    std::vector<Ref> refs;

    // The resource this one depends on, if any. Destroy() releases the edge so
    // the parent can retire once its last dependent is gone.
    ResourceId dependency;
    bool hasDependency = false;

    // Backend payload. Opaque to the registry.
    void* backendObject = nullptr;

    // Called exactly once, when the resource is physically destroyed.
    std::function<void(void*)> destroyFn;
};

// Tracks resources across one or more independent completion domains.
class ResourceRegistry {
public:
    // Creates a resource. The caller owns it.
    ResourceId Create(ResourceType type, void* backendObject,
                      std::function<void(void*)> destroyFn);

    // Resolves an id, validating index, generation, type and lifecycle state.
    // Returns nullptr and fills 'error' on any mismatch.
    ResourceRecord* Resolve(ResourceId id, ResourceType expected,
                             LookupError* error = nullptr);
    const ResourceRecord* Resolve(ResourceId id, ResourceType expected,
                                  LookupError* error = nullptr) const;

    // Typed payload access. Returns nullptr unless every check passes.
    template <typename T>
    T* Payload(ResourceId id, ResourceType expected, LookupError* error = nullptr) {
        ResourceRecord* r = Resolve(id, expected, error);
        return r == nullptr ? nullptr : static_cast<T*>(r->backendObject);
    }

    bool IsAlive(ResourceId id) const;
    bool CanRecord(ResourceId id) const;
    ResourceState StateOf(ResourceId id) const;

    // --- Recording ownership ---
    // A command buffer being recorded retains its resources before any backend
    // handle is passed to the GPU API.
    bool RetainForRecording(ResourceId id, uint64_t recordingToken);
    // Transfers every reference held by 'recordingToken' into GPU domain 'value'.
    // This is the gap-free handoff from recording to submitted.
    int CommitRecording(uint64_t recordingToken, uint32_t domain, uint64_t value);
    // Releases every reference held by 'recordingToken' without submitting.
    int ReleaseRecording(uint64_t recordingToken);

    // --- GPU ownership ---
    bool RetainForGpu(ResourceId id, uint32_t domain, uint64_t value);
    // Reports that 'domain' reached 'value'. Releases every reference at or
    // below it and physically destroys resources that are now unreferenced.
    int CompleteDomain(uint32_t domain, uint64_t value);

    // --- Dependencies ---
    // 'dependent' needs 'dependency' to stay alive. The dependency cannot be
    // physically destroyed while the dependent exists.
    bool AddDependency(ResourceId dependent, ResourceId dependency);
    // Releases the dependency edge. Called when the dependent is destroyed.
    void ReleaseDependency(ResourceId dependent, ResourceId dependency);

    // --- Destruction ---
    // Revokes application ownership. Physical destruction is deferred while any
    // reference remains. Returns false if the id is stale.
    bool Destroy(ResourceId id);
    // Physically destroys everything. Only for shutdown after the device is idle.
    int DestroyAll();

    // Diagnostics.
    int LiveCount() const;
    int PendingDestroyCount() const;
    int TotalRefs() const;
    int TotalGpuRefs() const;
    int CountOfType(ResourceType t) const;

private:
    bool MaybeDestroy(uint64_t index);
    void ReleaseAllRefsOf(uint64_t index);

    std::unordered_map<uint64_t, ResourceRecord> records_;
    // dependency index -> the dependents that hold it
    std::unordered_map<uint64_t, std::vector<ResourceId>> dependents_;
    uint64_t nextIndex_ = 1;   // 0 is reserved for an invalid id
};

}  // namespace harness
