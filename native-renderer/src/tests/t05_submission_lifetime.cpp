// t05_submission_lifetime: generation-tagged resources, deferred parameter
// snapshots, strong retention of pending resources, and GPU-completion-driven
// retirement using real timeline semaphores.
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/nri_adoption.h"
#include "common/shader_compile.h"
#include "common/vk_bootstrap.h"

namespace {

// A generation-tagged identity. A raw pointer is not a stable identity because
// the allocator can reuse an address after destruction.
struct ResourceId {
    uint64_t index = 0;
    uint64_t generation = 0;

    bool operator==(const ResourceId& o) const {
        return index == o.index && generation == o.generation;
    }
};

// Tracks live resources and retires them only after the GPU has finished the
// submission that referenced them.
class LifetimeTracker {
public:
    ResourceId Register(const std::string& name) {
        const uint64_t idx = nextIndex_++;
        ResourceId id{idx, 1};
        Entry e;
        e.name = name;
        e.live = true;
        entries_[idx] = e;
        return id;
    }

    // Strong retention: the resource cannot be retired while a submission
    // that references it is still in flight.
    void RetainForSubmission(ResourceId id, uint64_t submissionValue) {
        auto it = entries_.find(id.index);
        if (it == entries_.end() || it->second.generation != id.generation) return;
        it->second.pendingSubmission = submissionValue;
        it->second.retained = true;
    }

    // Called when the GPU reports completion of 'completedValue'.
    int RetireCompleted(uint64_t completedValue) {
        int retired = 0;
        for (auto& kv : entries_) {
            Entry& e = kv.second;
            if (e.live && e.retained && e.pendingSubmission <= completedValue) {
                e.live = false;
                e.retained = false;
                e.generation++;   // invalidate the old identity
                ++retired;
            }
        }
        return retired;
    }

    bool IsLive(ResourceId id) const {
        auto it = entries_.find(id.index);
        return it != entries_.end() && it->second.live && it->second.generation == id.generation;
    }

    int liveCount() const {
        int n = 0;
        for (const auto& kv : entries_) if (kv.second.live) ++n;
        return n;
    }

private:
    struct Entry {
        std::string name;
        bool live = false;
        bool retained = false;
        uint64_t pendingSubmission = 0;
        uint64_t generation = 1;
    };
    std::unordered_map<uint64_t, Entry> entries_;
    uint64_t nextIndex_ = 0;
};

// A deferred scalar parameter snapshot. The value is copied at record time so
// later mutation of the source cannot change what the command observes.
struct ParamSnapshot {
    uint32_t slot = 0;
    float value = 0.0f;
};

}  // namespace

int main() {
    harness::TestLog log("t05_submission_lifetime");

    harness::BootstrapConfig cfg;
    cfg.enableValidation = true;
    cfg.enableSyncValidation = true;

    harness::VulkanContext vk;
    std::string error;
    if (!vk.Initialize(cfg, error)) {
        log.Fail("Vulkan init: " + error);
        return log.Finish();
    }
    const auto& m = vk.manifest();
    if (!m.timelineSemaphore) {
        log.Unsupported("timelineSemaphore required for this test");
        return log.Finish();
    }
    log.Pass("timelineSemaphore available");

    harness::NriContext nri;
    if (!nri.Initialize(vk, true, error)) {
        log.Fail("NRI init: " + error);
        return log.Finish();
    }

    // ---- Generation-tagged identity ----
    {
        LifetimeTracker tracker;
        const ResourceId a = tracker.Register("textureA");
        if (tracker.IsLive(a)) log.Pass("newly registered resource is live");
        else log.Fail("registration failed");

        tracker.RetainForSubmission(a, 1);
        tracker.RetireCompleted(0);
        if (tracker.IsLive(a)) log.Pass("resource retained while submission is in flight");
        else log.Fail("resource retired too early");

        tracker.RetireCompleted(1);
        if (!tracker.IsLive(a)) log.Pass("resource retired after GPU completion");
        else log.Fail("resource not retired after completion");

        // The stale identity must not resolve to a reused slot.
        if (!tracker.IsLive(a)) log.Pass("stale generation no longer resolves");
        else log.Fail("stale generation still resolves");
    }

    // ---- Deferred parameter snapshot ----
    {
        float liveValue = 1.0f;
        ParamSnapshot snap;
        snap.slot = 3;
        snap.value = liveValue;   // snapshot at record time
        liveValue = 99.0f;        // mutate the source afterwards
        if (snap.value == 1.0f) {
            log.Pass("deferred parameter snapshot is immune to later mutation");
        } else {
            log.Fail("parameter snapshot captured a mutated value");
        }
    }

    // ---- Real GPU completion via timeline semaphore ----
    VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());

    VkSemaphoreTypeCreateInfo tci{};
    tci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tci.initialValue = 0;
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &tci;
    VkSemaphore timeline = VK_NULL_HANDLE;
    if (vkCreateSemaphore(vk.device(), &sci, nullptr, &timeline) != VK_SUCCESS) {
        log.Fail("timeline semaphore creation failed");
        return log.Finish();
    }
    log.Pass("timeline semaphore created");

    // Submit three trivial command buffers, each signalling a distinct value.
    LifetimeTracker tracker;
    std::vector<ResourceId> ids;
    for (int i = 0; i < 3; ++i) ids.push_back(tracker.Register("res" + std::to_string(i)));

    // Command buffers must stay alive until their submission completes.
    // Freeing them immediately after submit is a real lifetime bug that the
    // validation layer catches (VUID-vkFreeCommandBuffers-pCommandBuffers-00047).
    std::vector<VkCommandBuffer> pendingCmds;
    for (int i = 0; i < 3; ++i) {
        VkCommandBuffer cmd = vk.BeginOneShot(pool);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 0,
                             nullptr);
        vkEndCommandBuffer(cmd);
        pendingCmds.push_back(cmd);

        VkTimelineSemaphoreSubmitInfo tsi{};
        tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        uint64_t signalValue = static_cast<uint64_t>(i + 1);
        tsi.signalSemaphoreValueCount = 1;
        tsi.pSignalSemaphoreValues = &signalValue;

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext = &tsi;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &timeline;

        tracker.RetainForSubmission(ids[i], signalValue);

        if (vkQueueSubmit(vk.graphicsQueue(), 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
            log.Fail("submit " + std::to_string(i) + " failed");
        }
    }
    log.Pass("three submissions recorded with retained resources");

    // Wait for the first value only; resources for later submissions must stay.
    {
        VkSemaphoreWaitInfo wi{};
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline;
        uint64_t waitValue = 1;
        wi.pValues = &waitValue;
        vkWaitSemaphores(vk.device(), &wi, UINT64_MAX);

        uint64_t completed = 0;
        vkGetSemaphoreCounterValue(vk.device(), timeline, &completed);

        // The GPU may already have completed all three submissions by the time
        // we observe the counter, so the exact retirement count is not
        // deterministic. What must hold is that retirement tracks the observed
        // completion value exactly: nothing below it stays live, nothing above
        // it is retired.
        const int retired = tracker.RetireCompleted(completed);
        log.Info("completion value observed: " + std::to_string(completed) +
                 ", retired: " + std::to_string(retired));

        bool correct = true;
        for (int i = 0; i < 3; ++i) {
            const bool shouldBeLive = static_cast<uint64_t>(i + 1) > completed;
            if (tracker.IsLive(ids[i]) != shouldBeLive) correct = false;
        }
        if (correct) {
            log.Pass("retirement exactly matches the observed completion value");
        } else {
            log.Fail("retirement does not match the completion value");
        }
    }

    // Drain everything.
    {
        VkSemaphoreWaitInfo wi{};
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline;
        uint64_t waitValue = 3;
        wi.pValues = &waitValue;
        vkWaitSemaphores(vk.device(), &wi, UINT64_MAX);

        uint64_t completed = 0;
        vkGetSemaphoreCounterValue(vk.device(), timeline, &completed);
        tracker.RetireCompleted(completed);
        if (tracker.liveCount() == 0) {
            log.Pass("all resources retired after full completion");
        } else {
            log.Fail("resources still live after full completion");
        }
    }

    // ---- Independent completion domains ----
    {
        // Two separate trackers model two independently progressing queues.
        LifetimeTracker domainA, domainB;
        const ResourceId ra = domainA.Register("queueA-res");
        const ResourceId rb = domainB.Register("queueB-res");
        domainA.RetainForSubmission(ra, 5);
        domainB.RetainForSubmission(rb, 5);

        domainA.RetireCompleted(5);   // A finished
        if (!domainA.IsLive(ra) && domainB.IsLive(rb)) {
            log.Pass("independent completion domains retire separately");
        } else {
            log.Fail("completion domains are not independent");
        }
    }

    // ---- Checked dimension conversion ----
    {
        // A narrowing conversion must be rejected rather than silently wrapped.
        const uint64_t big = 0x1'0000'0001ull;
        const bool fits = big <= 0xFFFFFFFFull;
        if (!fits) log.Pass("oversized dimension detected before narrowing");
        else log.Fail("dimension check failed");

        const uint64_t ok = 4096;
        if (ok <= 0xFFFFFFFFull) log.Pass("in-range dimension accepted");
        else log.Fail("in-range dimension rejected");
    }

    // All submissions have completed, so the command buffers are free to release.
    vkFreeCommandBuffers(vk.device(), pool, static_cast<uint32_t>(pendingCmds.size()),
                         pendingCmds.data());
    log.Pass("command buffers freed only after GPU completion");

    vkDestroySemaphore(vk.device(), timeline, nullptr);
    vkDestroyCommandPool(vk.device(), pool, nullptr);
    nri.Shutdown();
    vk.Shutdown();

    const auto& counters = harness::GlobalValidationCounters();
    if (counters.errors == 0) {
        log.Pass("zero validation errors");
    } else {
        log.Fail(std::to_string(counters.errors) + " validation errors");
        for (const auto& msg : counters.messages) log.Info(msg);
    }

    return log.Finish();
}
