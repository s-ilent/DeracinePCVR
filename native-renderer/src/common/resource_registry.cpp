#include "common/resource_registry.h"

#include <algorithm>

namespace harness {

const char* ResourceTypeName(ResourceType t) {
    switch (t) {
        case ResourceType::kBuffer: return "buffer";
        case ResourceType::kTexture: return "texture";
        case ResourceType::kTextureView: return "texture-view";
        case ResourceType::kShader: return "shader";
        case ResourceType::kPipeline: return "pipeline";
        default: return "unknown";
    }
}

const char* LookupErrorName(LookupError e) {
    switch (e) {
        case LookupError::kOk: return "ok";
        case LookupError::kInvalidId: return "invalid-id";
        case LookupError::kStaleGeneration: return "stale-generation";
        case LookupError::kWrongType: return "wrong-type";
        case LookupError::kDestroyed: return "destroyed";
        case LookupError::kPendingDestroy: return "pending-destroy";
        case LookupError::kNotRecording: return "not-recording";
    }
    return "unknown";
}

ResourceId ResourceRegistry::Create(ResourceType type, void* backendObject,
                                    std::function<void(void*)> destroyFn) {
    const uint64_t index = nextIndex_++;
    ResourceRecord rec;
    rec.id = ResourceId{index, 1};
    rec.type = type;
    rec.state = ResourceState::kLive;
    rec.appOwned = true;
    rec.backendObject = backendObject;
    rec.destroyFn = std::move(destroyFn);
    records_[index] = std::move(rec);
    return records_[index].id;
}

ResourceRecord* ResourceRegistry::Resolve(ResourceId id, ResourceType expected,
                                          LookupError* error) {
    auto set = [&](LookupError e) { if (error) *error = e; };

    if (!id.IsValid()) {
        set(LookupError::kInvalidId);
        return nullptr;
    }
    auto it = records_.find(id.index);
    if (it == records_.end()) {
        set(LookupError::kInvalidId);
        return nullptr;
    }
    ResourceRecord& r = it->second;
    // Generation is checked before anything else: a reused slot must never
    // resolve an older identity, even if the type happens to match.
    if (r.id.generation != id.generation) {
        set(LookupError::kStaleGeneration);
        return nullptr;
    }
    if (r.state == ResourceState::kDestroyed) {
        set(LookupError::kDestroyed);
        return nullptr;
    }
    if (expected != ResourceType::kUnknown && r.type != expected) {
        set(LookupError::kWrongType);
        return nullptr;
    }
    set(LookupError::kOk);
    return &r;
}

const ResourceRecord* ResourceRegistry::Resolve(ResourceId id, ResourceType expected,
                                               LookupError* error) const {
    return const_cast<ResourceRegistry*>(this)->Resolve(id, expected, error);
}

bool ResourceRegistry::IsAlive(ResourceId id) const {
    return Resolve(id, ResourceType::kUnknown) != nullptr;
}

bool ResourceRegistry::CanRecord(ResourceId id) const {
    const ResourceRecord* r = Resolve(id, ResourceType::kUnknown);
    // A resource pending destruction must not accept new work, even though it
    // is still physically alive for the submissions already in flight.
    return r != nullptr && r->state == ResourceState::kLive;
}

ResourceState ResourceRegistry::StateOf(ResourceId id) const {
    const ResourceRecord* r = Resolve(id, ResourceType::kUnknown);
    return r == nullptr ? ResourceState::kDestroyed : r->state;
}

bool ResourceRegistry::RetainForRecording(ResourceId id, uint64_t recordingToken) {
    ResourceRecord* r = Resolve(id, ResourceType::kUnknown);
    if (r == nullptr) return false;
    // Recording is new work, so a pending-destroy resource is refused here.
    if (r->state != ResourceState::kLive) return false;

    for (const auto& ref : r->refs) {
        if (ref.domain == kRecordingDomain && ref.value == recordingToken) return true;
    }
    r->refs.push_back(ResourceRecord::Ref{kRecordingDomain, recordingToken});
    return true;
}

int ResourceRegistry::CommitRecording(uint64_t recordingToken, uint32_t domain,
                                      uint64_t value) {
    int committed = 0;
    for (auto& kv : records_) {
        ResourceRecord& r = kv.second;
        if (r.state == ResourceState::kDestroyed) continue;

        bool held = false;
        for (const auto& ref : r.refs) {
            if (ref.domain == kRecordingDomain && ref.value == recordingToken) {
                held = true;
                break;
            }
        }
        if (!held) continue;

        // Remove the recording reference and add the GPU one in the same step,
        // so there is no window in which the resource is unreferenced.
        r.refs.erase(std::remove_if(r.refs.begin(), r.refs.end(),
                                    [&](const ResourceRecord::Ref& ref) {
                                        return ref.domain == kRecordingDomain &&
                                               ref.value == recordingToken;
                                    }),
                     r.refs.end());

        bool already = false;
        for (const auto& ref : r.refs) {
            if (ref.domain == domain && ref.value == value) {
                already = true;
                break;
            }
        }
        if (!already) r.refs.push_back(ResourceRecord::Ref{domain, value});
        ++committed;
    }
    return committed;
}

int ResourceRegistry::ReleaseRecording(uint64_t recordingToken) {
    int released = 0;
    for (auto& kv : records_) {
        ResourceRecord& r = kv.second;
        if (r.state == ResourceState::kDestroyed) continue;
        const size_t before = r.refs.size();
        r.refs.erase(std::remove_if(r.refs.begin(), r.refs.end(),
                                    [&](const ResourceRecord::Ref& ref) {
                                        return ref.domain == kRecordingDomain &&
                                               ref.value == recordingToken;
                                    }),
                     r.refs.end());
        if (r.refs.size() != before) {
            ++released;
            MaybeDestroy(kv.first);
        }
    }
    return released;
}

bool ResourceRegistry::RetainForGpu(ResourceId id, uint32_t domain, uint64_t value) {
    ResourceRecord* r = Resolve(id, ResourceType::kUnknown);
    if (r == nullptr) return false;
    if (r->state != ResourceState::kLive) return false;

    for (const auto& ref : r->refs) {
        if (ref.domain == domain && ref.value == value) return true;
    }
    r->refs.push_back(ResourceRecord::Ref{domain, value});
    return true;
}

int ResourceRegistry::CompleteDomain(uint32_t domain, uint64_t value) {
    int destroyed = 0;
    for (auto& kv : records_) {
        ResourceRecord& r = kv.second;
        if (r.state == ResourceState::kDestroyed) continue;

        const size_t before = r.refs.size();
        r.refs.erase(std::remove_if(r.refs.begin(), r.refs.end(),
                                    [&](const ResourceRecord::Ref& ref) {
                                        return ref.domain == domain && ref.value <= value;
                                    }),
                     r.refs.end());

        if (r.refs.size() != before) {
            if (MaybeDestroy(kv.first)) ++destroyed;
        }
    }
    return destroyed;
}

bool ResourceRegistry::AddDependency(ResourceId dependent, ResourceId dependency) {
    ResourceRecord* d = Resolve(dependent, ResourceType::kUnknown);
    ResourceRecord* p = Resolve(dependency, ResourceType::kUnknown);
    if (d == nullptr || p == nullptr) return false;

    auto& list = dependents_[dependency.index];
    for (const auto& existing : list) {
        if (existing == dependent) return true;
    }
    list.push_back(dependent);
    d->dependency = dependency;
    d->hasDependency = true;
    return true;
}

void ResourceRegistry::ReleaseDependency(ResourceId dependent, ResourceId dependency) {
    auto it = dependents_.find(dependency.index);
    if (it == dependents_.end()) return;
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const ResourceId& r) { return r == dependent; }),
               list.end());
    if (list.empty()) dependents_.erase(it);
    MaybeDestroy(dependency.index);
}

bool ResourceRegistry::Destroy(ResourceId id) {
    ResourceRecord* r = Resolve(id, ResourceType::kUnknown);
    if (r == nullptr) return false;

    // Release any dependency edge this resource holds, so a parent can retire
    // once its last dependent is gone. Doing it here means callers cannot
    // forget, and the backend does not need a special case.
    if (r->hasDependency) {
        const ResourceId parent = r->dependency;
        r->hasDependency = false;
        r->dependency = ResourceId{};
        ReleaseDependency(id, parent);
        // ReleaseDependency may have destroyed this record; re-resolve.
        r = Resolve(id, ResourceType::kUnknown);
        if (r == nullptr) return true;
    }

    r->appOwned = false;
    if (r->state == ResourceState::kLive) r->state = ResourceState::kPendingDestroy;

    MaybeDestroy(id.index);
    return true;
}

int ResourceRegistry::DestroyAll() {
    int destroyed = 0;
    for (auto& kv : records_) {
        ResourceRecord& r = kv.second;
        if (r.state == ResourceState::kDestroyed) continue;
        r.appOwned = false;
        r.refs.clear();
        if (r.destroyFn && r.backendObject != nullptr) {
            r.destroyFn(r.backendObject);
        }
        r.backendObject = nullptr;
        r.state = ResourceState::kDestroyed;
        r.id.generation++;
        ++destroyed;
    }
    dependents_.clear();
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

int ResourceRegistry::TotalRefs() const {
    int n = 0;
    for (const auto& kv : records_) n += static_cast<int>(kv.second.refs.size());
    return n;
}

int ResourceRegistry::TotalGpuRefs() const {
    int n = 0;
    for (const auto& kv : records_) {
        for (const auto& ref : kv.second.refs) {
            if (ref.domain != kRecordingDomain && ref.domain != kDependencyDomain) ++n;
        }
    }
    return n;
}

int ResourceRegistry::CountOfType(ResourceType t) const {
    int n = 0;
    for (const auto& kv : records_) {
        if (kv.second.state != ResourceState::kDestroyed && kv.second.type == t) ++n;
    }
    return n;
}

void ResourceRegistry::ReleaseAllRefsOf(uint64_t index) {
    auto it = records_.find(index);
    if (it != records_.end()) it->second.refs.clear();
}

bool ResourceRegistry::MaybeDestroy(uint64_t index) {
    auto it = records_.find(index);
    if (it == records_.end()) return false;
    ResourceRecord& r = it->second;
    if (r.state == ResourceState::kDestroyed) return false;

    // Every condition must hold.
    if (r.appOwned) return false;
    if (!r.refs.empty()) return false;

    // A dependency cannot die while a dependent still exists.
    auto depIt = dependents_.find(index);
    if (depIt != dependents_.end() && !depIt->second.empty()) return false;

    if (r.destroyFn && r.backendObject != nullptr) {
        r.destroyFn(r.backendObject);
    }
    r.backendObject = nullptr;
    r.state = ResourceState::kDestroyed;
    r.id.generation++;   // a reused slot must not resolve the old id
    return true;
}

}  // namespace harness
