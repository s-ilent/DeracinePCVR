// Standalone host-only queue arbitration contract; no Vulkan/OpenXR dependency.
#include <atomic>
#include <cassert>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

class QueueArbiter {
    std::mutex mutex_;
    static thread_local const QueueArbiter* active_;
public:
    template<class F> auto run(F&& operation) -> decltype(operation()) {
        if (active_ == this) throw std::logic_error("recursive queue access");
        std::lock_guard<std::mutex> lock(mutex_);
        struct Guard {
            const QueueArbiter*& slot;
            const QueueArbiter* prior;
            Guard(const QueueArbiter*& s, const QueueArbiter* current) : slot(s), prior(s) { slot = current; }
            ~Guard() { slot = prior; }
        } guard(active_, this);
        return operation();
    }
};
thread_local const QueueArbiter* QueueArbiter::active_ = nullptr;

int main() {
    QueueArbiter arbiter;
    std::atomic<int> inside{0}, overlap{0}, calls{0};
    auto operation = [&] {
        if (inside.fetch_add(1) != 0) ++overlap;
        std::this_thread::yield();
        ++calls;
        inside.fetch_sub(1);
    };
    std::vector<std::thread> threads;
    // Models NRI submit and four XR operations sharing one queue.
    for (int i=0;i<6;++i) threads.emplace_back([&] { for (int j=0;j<1000;++j) arbiter.run(operation); });
    for (auto& t:threads) t.join();
    assert(calls == 6000 && overlap == 0 && inside == 0);
    bool failed=false;
    try { arbiter.run([&] { throw std::runtime_error("mock XR failure"); }); }
    catch (const std::runtime_error&) { failed=true; }
    assert(failed);
    arbiter.run(operation);
    bool recursive=false;
    try { arbiter.run([&] { arbiter.run(operation); }); }
    catch (const std::logic_error&) { recursive=true; }
    assert(recursive);
    arbiter.run(operation);
    assert(calls == 6002 && overlap == 0);
    std::cout << "PASS: 6000 contended operations; zero overlap; exception unlock; recursive rejection; recovery\n";
}
