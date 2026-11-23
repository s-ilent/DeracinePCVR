#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
#include <iostream>
struct Resource { int id; explicit Resource(int n):id(n){} };
struct Submission { uint64_t value; std::vector<std::shared_ptr<Resource>> resources; };
class RetirementQueue {
  uint64_t lastSubmitted=0, lastObserved=0;
  std::map<uint64_t,Submission> pending;
public:
  uint64_t submit(std::vector<std::shared_ptr<Resource>> r) {
    if(lastSubmitted==UINT64_MAX) throw std::overflow_error("timeline exhausted");
    uint64_t v=++lastSubmitted;
    pending.emplace(v,Submission{v,std::move(r)});
    return v;
  }
  void observe(uint64_t gpuCounter) {
    if(gpuCounter<lastObserved || gpuCounter>lastSubmitted)
      throw std::logic_error("invalid counter observation");
    lastObserved=gpuCounter;
    auto end=pending.upper_bound(gpuCounter);
    pending.erase(pending.begin(),end);
  }
  void abandonUnsubmitted(std::vector<std::shared_ptr<Resource>>& recorded) {recorded.clear();}
  size_t outstanding() const {return pending.size();}
  void teardownAfterDeviceIdle() { observe(lastSubmitted); } // ONLY after externally verified idle
};
int main(){
  int checks=0;
  RetirementQueue q;
  auto a=std::make_shared<Resource>(1); std::weak_ptr<Resource> wa=a;
  std::vector<std::shared_ptr<Resource>> first{a};
  uint64_t v1=q.submit(std::move(first)); a.reset();
  assert(!wa.expired()); ++checks;
  auto b=std::make_shared<Resource>(2); std::weak_ptr<Resource> wb=b;
  uint64_t v2=q.submit({b}); b.reset();
  q.observe(0); assert(!wa.expired() && !wb.expired()); ++checks;
  q.observe(v1); assert(wa.expired() && !wb.expired()); ++checks;
  q.observe(v1); assert(q.outstanding()==1); ++checks;
  bool threw=false; try {q.observe(v2+1);}catch(const std::logic_error&){threw=true;}
  assert(threw && !wb.expired()); ++checks;
  threw=false; try {q.observe(0);}catch(const std::logic_error&){threw=true;}
  assert(threw && !wb.expired()); ++checks;
  std::vector<std::shared_ptr<Resource>> discarded;
  auto c=std::make_shared<Resource>(3); std::weak_ptr<Resource> wc=c;
  discarded.push_back(c); c.reset(); q.abandonUnsubmitted(discarded);
  assert(wc.expired()); ++checks;
  q.observe(v2); assert(wb.expired() && q.outstanding()==0); ++checks;
  auto d=std::make_shared<Resource>(4); std::weak_ptr<Resource> wd=d;
  q.submit({d}); d.reset(); assert(!wd.expired()); ++checks;
  // Simulates a teardown caller that has independently established GPU idle.
  q.teardownAfterDeviceIdle(); assert(wd.expired()); ++checks;
  std::cout << "PASS " << checks << " deterministic retirement checks\n";
}
