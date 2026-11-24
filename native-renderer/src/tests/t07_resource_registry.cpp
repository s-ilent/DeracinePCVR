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

}  // namespace

int main() {
    harness::TestLog log("t07_resource_registry");

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(reinterpret_cast<void*>(0x1000), dc.MakeFn(1));
        if (reg.IsAlive(id) && reg.CanRecord(id)) log.Pass("created resource is live and recordable");
        else log.Fail("created resource not usable");
        if (reg.LiveCount() == 1) log.Pass("live count is 1");
        else log.Fail("live count wrong");
    }

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(reinterpret_cast<void*>(0x2000), dc.MakeFn(2));
        reg.RetainForGpu(id, 0, 1);
        reg.CompleteDomain(0, 1);
        if (dc.count == 0) log.Pass("GPU completion did not destroy an app-owned resource");
        else log.Fail("GPU completion destroyed an app-owned resource");
        if (reg.IsAlive(id)) log.Pass("resource still alive after GPU completion");
        else log.Fail("resource died after GPU completion");
        if (reg.TotalGpuRefs() == 0) log.Pass("GPU reference released");
        else log.Fail("GPU reference not released");
    }

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(reinterpret_cast<void*>(0x3000), dc.MakeFn(3));
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

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(reinterpret_cast<void*>(0x4000), dc.MakeFn(4));
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

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(reinterpret_cast<void*>(0x5000), dc.MakeFn(5));
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

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(reinterpret_cast<void*>(0x6000), dc.MakeFn(6));
        reg.Destroy(id);
        if (!reg.IsAlive(id)) log.Pass("destroyed id does not resolve");
        else log.Fail("destroyed id still resolves");
        if (!reg.CanRecord(id)) log.Pass("destroyed id rejects recording");
        else log.Fail("destroyed id accepted recording");
        if (!reg.RetainForGpu(id, 0, 1)) log.Pass("destroyed id rejects GPU retention");
        else log.Fail("destroyed id accepted GPU retention");
        const auto id2 = reg.Create(reinterpret_cast<void*>(0x6001), dc.MakeFn(7));
        if (!(id2 == id)) log.Pass("new resource has a distinct identity");
        else log.Fail("identity collision");
    }

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(reinterpret_cast<void*>(0x7000), dc.MakeFn(8));
        reg.RetainForGpu(id, 0, 9);
        reg.AbandonGpuRefs(id, 0, 9);
        if (reg.TotalGpuRefs() == 0) log.Pass("abandoned recording released its reference");
        else log.Fail("abandoned recording leaked a reference");
        if (reg.IsAlive(id)) log.Pass("app-owned resource survives an abandoned recording");
        else log.Fail("app-owned resource died on abandon");
    }

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto a = reg.Create(reinterpret_cast<void*>(0x8000), dc.MakeFn(9));
        const auto b = reg.Create(reinterpret_cast<void*>(0x8001), dc.MakeFn(10));
        reg.RetainForGpu(a, 0, 100);
        const int n = reg.DestroyAll();
        if (n == 2) log.Pass("DestroyAll drained both resources");
        else log.Fail("DestroyAll drained " + std::to_string(n) + " of 2");
        if (dc.count == 2) log.Pass("both destructors ran");
        else log.Fail("destructor count wrong");
        if (!reg.IsAlive(a) && !reg.IsAlive(b)) log.Pass("both ids invalidated");
        else log.Fail("ids still resolve after DestroyAll");
    }

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(reinterpret_cast<void*>(0x9000), dc.MakeFn(11));
        reg.RetainForGpu(id, 0, 3);
        reg.RetainForGpu(id, 0, 3);
        if (reg.TotalGpuRefs() == 1) log.Pass("duplicate retention is idempotent");
        else log.Fail("duplicate retention created refs");
        reg.Destroy(id);
        reg.CompleteDomain(0, 3);
        if (dc.count == 1) log.Pass("single completion released the resource");
        else log.Fail("completion did not release");
    }

    {
        harness::ResourceRegistry reg;
        DestructionCounter dc;
        const auto id = reg.Create(reinterpret_cast<void*>(0xA000), dc.MakeFn(12));
        reg.RetainForGpu(id, 0, 10);
        reg.RetainForGpu(id, 0, 20);
        reg.Destroy(id);
        reg.CompleteDomain(0, 20);
        if (dc.count == 1) log.Pass("a later completion releases earlier references");
        else log.Fail("later completion did not release earlier refs");
    }

    return log.Finish();
}
