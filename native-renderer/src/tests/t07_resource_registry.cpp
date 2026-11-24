// t07_resource_registry: deterministic CPU test of the corrected ownership model.
//
// No GPU. This isolates the registry semantics from any backend behaviour, so a
// failure here is unambiguously a lifetime bug rather than a driver issue.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "common/diagnostics.h"
#include "common/resource_registry.h"

namespace {

struct DestructionCounter {
    int count = 0;
    std::vector<int> order;

    std::function<void(void*)> MakeFn(int tag) {
        return [this, tag](void*) {
            ++count;
            order.push_back(tag);
        };
    }
};

constexpr uint64_t kRec = 100;

}  // namespace

int main() {
    harness::TestLog log("t07_resource_registry");

    // ---- Creation and identity ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0x1000), dc.MakeFn(1));
        if (reg.IsAlive(id) && reg.CanRecord(id)) log.Pass("created resource is live and recordable");
        else log.Fail("created resource not usable");
        if (reg.LiveCount() == 1) log.Pass("live count is 1");
        else log.Fail("live count wrong");
    }

    // ---- GPU completion does not destroy an app-owned resource ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0x2000), dc.MakeFn(2));
        reg.RetainForGpu(id, 0, 1);
        reg.CompleteDomain(0, 1);
        if (dc.count == 0) log.Pass("GPU completion did not destroy an app-owned resource");
        else log.Fail("GPU completion destroyed an app-owned resource");
        if (reg.IsAlive(id)) log.Pass("resource still alive after GPU completion");
        else log.Fail("resource died after GPU completion");
        if (reg.TotalGpuRefs() == 0) log.Pass("GPU reference released");
        else log.Fail("GPU reference not released");
    }

    // ---- Destroy before completion defers physical destruction ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0x3000), dc.MakeFn(3));
        reg.RetainForGpu(id, 0, 5);
        reg.Destroy(id);
        if (dc.count == 0) log.Pass("destruction deferred while GPU reference is outstanding");
        else log.Fail("resource destroyed while still referenced");
        if (reg.PendingDestroyCount() == 1) log.Pass("resource is in pending-destroy state");
        else log.Fail("resource not marked pending");
        if (!reg.CanRecord(id)) log.Pass("pending-destroy resource rejects new work");
        else log.Fail("pending-destroy resource accepted new work");
        reg.CompleteDomain(0, 5);
        if (dc.count == 1) log.Pass("physical destruction occurred after GPU completion");
        else log.Fail("physical destruction did not occur");
        if (!reg.IsAlive(id)) log.Pass("destroyed resource no longer resolves");
        else log.Fail("destroyed resource still resolves");
    }

    // ---- Two in-flight submissions ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0x4000), dc.MakeFn(4));
        reg.RetainForGpu(id, 0, 1);
        reg.RetainForGpu(id, 0, 2);
        reg.Destroy(id);
        reg.CompleteDomain(0, 1);
        if (dc.count == 0) log.Pass("still retained by the second submission");
        else log.Fail("destroyed after only one of two submissions");
        reg.CompleteDomain(0, 2);
        if (dc.count == 1) log.Pass("destroyed after both submissions completed");
        else log.Fail("not destroyed after both completed");
    }

    // ---- Independent completion domains ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0x5000), dc.MakeFn(5));
        reg.RetainForGpu(id, 0, 1);
        reg.RetainForGpu(id, 1, 1);
        reg.Destroy(id);
        reg.CompleteDomain(0, 1);
        if (dc.count == 0) log.Pass("other domain still holds a reference");
        else log.Fail("destroyed while another domain referenced it");
        reg.CompleteDomain(1, 1);
        if (dc.count == 1) log.Pass("destroyed after both domains completed");
        else log.Fail("not destroyed after both domains");
    }

    // ---- Stale generation rejected ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0x6000), dc.MakeFn(6));
        reg.Destroy(id);
        if (!reg.IsAlive(id)) log.Pass("destroyed id does not resolve");
        else log.Fail("destroyed id still resolves");
        if (!reg.CanRecord(id)) log.Pass("destroyed id rejects recording");
        else log.Fail("destroyed id accepted recording");
        if (!reg.RetainForGpu(id, 0, 1)) log.Pass("destroyed id rejects GPU retention");
        else log.Fail("destroyed id accepted GPU retention");
        const auto id2 = reg.Create(harness::ResourceType::kBuffer,
                                    reinterpret_cast<void*>(0x6001), dc.MakeFn(7));
        if (!(id2 == id)) log.Pass("new resource has a distinct identity");
        else log.Fail("identity collision");
    }

    // ---- Recording ownership ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0x7000), dc.MakeFn(8));
        if (reg.RetainForRecording(id, kRec)) log.Pass("recording acquired a reference");
        else log.Fail("recording could not acquire a reference");
        // Destroy while recording: physical destruction must be deferred.
        reg.Destroy(id);
        if (dc.count == 0) log.Pass("recording reference keeps the resource alive after Destroy");
        else log.Fail("resource destroyed while a recording referenced it");
        // The recording can still commit to GPU ownership.
        const int committed = reg.CommitRecording(kRec, 0, 42);
        if (committed == 1) log.Pass("recording committed to GPU ownership");
        else log.Fail("commit transferred " + std::to_string(committed) + " resources");
        if (dc.count == 0) log.Pass("no lifetime gap between recording and GPU ownership");
        else log.Fail("resource died during the recording-to-GPU handoff");
        reg.CompleteDomain(0, 42);
        if (dc.count == 1) log.Pass("destroyed after the committed submission completed");
        else log.Fail("not destroyed after completion");
    }

    // ---- Abandoned recording releases its references ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0x8000), dc.MakeFn(9));
        reg.RetainForRecording(id, kRec);
        const int released = reg.ReleaseRecording(kRec);
        if (released == 1) log.Pass("abandoned recording released its reference");
        else log.Fail("abandoned recording released " + std::to_string(released));
        if (reg.IsAlive(id)) log.Pass("app-owned resource survives an abandoned recording");
        else log.Fail("app-owned resource died on abandon");
    }

    // ---- Duplicate recording retention is idempotent ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0x9000), dc.MakeFn(10));
        reg.RetainForRecording(id, kRec);
        reg.RetainForRecording(id, kRec);
        if (reg.TotalRefs() == 1) log.Pass("duplicate recording retention is idempotent");
        else log.Fail("duplicate retention created " + std::to_string(reg.TotalRefs()) + " refs");
        reg.CommitRecording(kRec, 0, 7);
        reg.Destroy(id);
        reg.CompleteDomain(0, 7);
        if (dc.count == 1) log.Pass("single completion released the resource");
        else log.Fail("completion did not release");
    }

    // ---- Type checking ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto buf = reg.Create(harness::ResourceType::kBuffer,
                                    reinterpret_cast<void*>(0xA000), dc.MakeFn(11));
        harness::LookupError err = harness::LookupError::kOk;
        if (reg.Resolve(buf, harness::ResourceType::kTexture, &err) == nullptr &&
            err == harness::LookupError::kWrongType) {
            log.Pass("wrong-type lookup is rejected as wrong-type");
        } else {
            log.Fail("wrong-type lookup was not rejected correctly");
        }
        if (reg.Resolve(buf, harness::ResourceType::kBuffer, &err) != nullptr) {
            log.Pass("correct-type lookup succeeds");
        } else {
            log.Fail("correct-type lookup failed");
        }
    }

    // ---- Stale generation is distinguishable from wrong type ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0xB000), dc.MakeFn(12));
        harness::ResourceId stale = id;
        stale.generation += 1;
        harness::LookupError err = harness::LookupError::kOk;
        if (reg.Resolve(stale, harness::ResourceType::kBuffer, &err) == nullptr &&
            err == harness::LookupError::kStaleGeneration) {
            log.Pass("stale generation is reported as stale-generation");
        } else {
            log.Fail("stale generation was not reported correctly");
        }
    }

    // ---- Dependency retention ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto tex = reg.Create(harness::ResourceType::kTexture,
                                    reinterpret_cast<void*>(0xC000), dc.MakeFn(13));
        const auto view = reg.Create(harness::ResourceType::kTextureView,
                                     reinterpret_cast<void*>(0xC001), dc.MakeFn(14));
        reg.AddDependency(view, tex);

        reg.Destroy(tex);
        if (dc.count == 0) log.Pass("parent texture survives while a dependent view exists");
        else log.Fail("parent texture destroyed while a view depended on it");

        reg.Destroy(view);
        if (dc.count == 2) log.Pass("both retire once the view is destroyed");
        else log.Fail("expected 2 destructions, saw " + std::to_string(dc.count));
    }

    // ---- Multiple views on one texture ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto tex = reg.Create(harness::ResourceType::kTexture,
                                    reinterpret_cast<void*>(0xD000), dc.MakeFn(15));
        const auto v1 = reg.Create(harness::ResourceType::kTextureView,
                                   reinterpret_cast<void*>(0xD001), dc.MakeFn(16));
        const auto v2 = reg.Create(harness::ResourceType::kTextureView,
                                   reinterpret_cast<void*>(0xD002), dc.MakeFn(17));
        reg.AddDependency(v1, tex);
        reg.AddDependency(v2, tex);
        reg.Destroy(tex);
        reg.Destroy(v1);
        if (dc.count == 1) log.Pass("texture survives while the second view exists");
        else log.Fail("texture retired too early");
        reg.Destroy(v2);
        if (dc.count == 3) log.Pass("texture retires after the last view");
        else log.Fail("expected 3 destructions, saw " + std::to_string(dc.count));
    }

    // ---- DestroyAll drains everything ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto a = reg.Create(harness::ResourceType::kBuffer,
                                  reinterpret_cast<void*>(0xE000), dc.MakeFn(18));
        const auto b = reg.Create(harness::ResourceType::kBuffer,
                                  reinterpret_cast<void*>(0xE001), dc.MakeFn(19));
        reg.RetainForGpu(a, 0, 100);
        const int n = reg.DestroyAll();
        if (n == 2) log.Pass("DestroyAll drained both resources");
        else log.Fail("DestroyAll drained " + std::to_string(n) + " of 2");
        if (dc.count == 2) log.Pass("both destructors ran");
        else log.Fail("destructor count wrong");
        if (!reg.IsAlive(a) && !reg.IsAlive(b)) log.Pass("both ids invalidated");
        else log.Fail("ids still resolve after DestroyAll");
    }

    // ---- Out-of-order completion ----
    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(harness::ResourceType::kBuffer,
                                   reinterpret_cast<void*>(0xF000), dc.MakeFn(20));
        reg.RetainForGpu(id, 0, 10);
        reg.RetainForGpu(id, 0, 20);
        reg.Destroy(id);
        reg.CompleteDomain(0, 20);
        if (dc.count == 1) log.Pass("a later completion releases earlier references");
        else log.Fail("later completion did not release earlier refs");
    }

    return log.Finish();
}
