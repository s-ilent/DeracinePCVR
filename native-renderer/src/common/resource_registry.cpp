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
        case ResourceType::kSampler: return "sampler";
        case ResourceType::kDescriptorSet: return "descriptor-set";
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

    // A parent whose application ownership has been revoked must not acquire
    // new dependents. Enforced here, at the ownership boundary, so a caller
    // cannot bypass it.
    if (p->state != ResourceState::kLive) return false;
    if (d->state != ResourceState::kLive) return false;

    // Reject a self-edge and any edge that would close a cycle. A cycle would
    // make topological destruction impossible, so it is refused at registration
    // rather than detected later.
    if (dependent == dependency) return false;
    if (DependsOn(dependency.index, dependent.index)) return false;

    for (const auto& existing : d->dependencies) {
        if (existing == dependency) return true;   // already registered
    }
    d->dependencies.push_back(dependency);
    dependents_[dependency.index].push_back(dependent);
    return true;
}

// True when 'from' transitively depends on 'target'. Used to reject cycles.
bool ResourceRegistry::DependsOn(uint64_t from, uint64_t target) const {
    std::vector<uint64_t> stack{from};
    std::vector<uint64_t> seen;
    while (!stack.empty()) {
        const uint64_t cur = stack.back();
        stack.pop_back();
        if (cur == target) return true;
        bool visited = false;
        for (uint64_t s : seen) {
            if (s == cur) { visited = true; break; }
        }
        if (visited) continue;
        seen.push_back(cur);
        auto it = records_.find(cur);
        if (it == records_.end()) continue;
        for (const auto& dep : it->second.dependencies) stack.push_back(dep.index);
    }
    return false;
}

void ResourceRegistry::ReleaseDependency(ResourceId dependent, ResourceId dependency) {
    auto it = dependents_.find(dependency.index);
    if (it != dependents_.end()) {
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const ResourceId& r) { return r == dependent; }),
                   list.end());
        if (list.empty()) dependents_.erase(it);
    }
    MaybeDestroy(dependency.index);
}

int ResourceRegistry::TotalDependencyEdges() const {
    int n = 0;
    for (const auto& kv : records_) n += static_cast<int>(kv.second.dependencies.size());
    return n;
}

bool ResourceRegistry::Destroy(ResourceId id) {
    ResourceRecord* r = Resolve(id, ResourceType::kUnknown);
    if (r == nullptr) return false;

    // The dependency edge is deliberately NOT released here. Revoking
    // application ownership does not make the dependent object disappear: it
    // may still be referenced by a recording or by submitted GPU work, and its
    // backend object still needs the parent to be alive. The edge is released
    // in MaybeDestroy, immediately before the dependent is physically
    // destroyed.
    r->appOwned = false;
    if (r->state == ResourceState::kLive) r->state = ResourceState::kPendingDestroy;

    MaybeDestroy(id.index);
    return true;
}

int ResourceRegistry::DestroyAll() {
    // A dependent's backend object references its parents, so dependents must be
    // destroyed first. This is a genuine topological sort over the dependency
    // graph, not a repeated scan: selecting a node removes it from the graph, so
    // its parents become eligible in the same pass.
    //
    // Kahn's algorithm on the reversed graph: a node is ready when no live node
    // still depends on it.
    std::vector<uint64_t> nodes;
    nodes.reserve(records_.size());
    for (const auto& kv : records_) {
        if (kv.second.state != ResourceState::kDestroyed) nodes.push_back(kv.first);
    }

    // Count, per node, how many live dependents it still has.
    std::unordered_map<uint64_t, int> liveDependents;
    for (uint64_t n : nodes) liveDependents[n] = 0;
    for (uint64_t n : nodes) {
        auto it = records_.find(n);
        if (it == records_.end()) continue;
        for (const auto& dep : it->second.dependencies) {
            auto pit = liveDependents.find(dep.index);
            if (pit != liveDependents.end()) ++pit->second;
        }
    }

    std::vector<uint64_t> ready;
    for (uint64_t n : nodes) {
        if (liveDependents[n] == 0) ready.push_back(n);
    }

    std::vector<uint64_t> sorted;
    sorted.reserve(nodes.size());
    std::vector<uint64_t> selected;
    while (!ready.empty()) {
        const uint64_t n = ready.back();
        ready.pop_back();
        sorted.push_back(n);
        selected.push_back(n);

        // Selecting n removes it as a dependent of each of its parents.
        auto it = records_.find(n);
        if (it == records_.end()) continue;
        for (const auto& dep : it->second.dependencies) {
            auto pit = liveDependents.find(dep.index);
            if (pit == liveDependents.end()) continue;
            if (--pit->second == 0) ready.push_back(dep.index);
        }
    }

    // A cycle cannot be ordered. AddDependency rejects cycles at registration,
    // so reaching here means the graph was corrupted. Report it rather than
    // destroying in arbitrary order.
    if (sorted.size() != nodes.size()) {
        return -1;
    }

    int destroyed = 0;
    for (uint64_t idx : sorted) {
        auto it = records_.find(idx);
        if (it == records_.end()) continue;
        ResourceRecord& r = it->second;
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

    // Capture the edges before the destructor runs, because the destructor may
    // re-enter the registry.
    const std::vector<ResourceId> parents = r.dependencies;
    const ResourceId selfId = r.id;

    // Destroy the backend object first. Only then is it safe to release the
    // parents, because the object's descriptor referenced them.
    if (r.destroyFn && r.backendObject != nullptr) {
        r.destroyFn(r.backendObject);
    }
    r.backendObject = nullptr;
    r.state = ResourceState::kDestroyed;
    r.id.generation++;   // a reused slot must not resolve the old id

    if (!parents.empty()) {
        auto self = records_.find(index);
        if (self != records_.end()) self->second.dependencies.clear();
        // Releasing an edge may make a parent destroyable. Iterate over a copy
        // so a re-entrant destruction cannot invalidate the loop.
        for (const auto& parent : parents) {
            ReleaseDependency(selfId, parent);
        }
    }
    return true;
}

}  // namespace harness
