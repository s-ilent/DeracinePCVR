// Host-only deferred resource retention model. Build with C++17 + ASan/UBSan.
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
struct Key { uint32_t id, gen; };
struct Resource { Key key; };
struct Registry {
  std::map<uint32_t,uint32_t> generation;
  std::map<uint32_t,std::weak_ptr<Resource>> current;
  std::shared_ptr<Resource> create(uint32_t id) {
    if (!current[id].expired()) throw std::runtime_error("identity retained");
    auto p=std::make_shared<Resource>(Resource{{id,++generation[id]}});
    current[id]=p; return p;
  }
  std::shared_ptr<Resource> resolve(Key k) const {
    auto it=current.find(k.id);
    if(it==current.end()) throw std::runtime_error("missing");
    auto p=it->second.lock();
    if(!p || p->key.gen!=k.gen) throw std::runtime_error("stale");
    return p;
  }
};
struct List {
  std::vector<std::shared_ptr<Resource>> retained;
  void bind(const Registry& r,Key k) { retained.push_back(r.resolve(k)); }
};
struct Submission { uint64_t epoch; std::vector<std::shared_ptr<Resource>> retained; };
struct Queue {
  uint64_t next=0, completed=0;
  std::vector<Submission> pending;
  uint64_t submit(List&& list) {
    auto e=++next; pending.push_back({e,std::move(list.retained)}); return e;
  }
  void complete(uint64_t e) {
    if(e<completed || e>next) throw std::runtime_error("bad epoch");
    completed=e;
    auto it=pending.begin();
    while(it!=pending.end() && it->epoch<=e) ++it;
    pending.erase(pending.begin(),it);
  }
};
int main() {
  Registry r; Queue q; int checks=0;
  auto owner=r.create(7); Key old=owner->key; std::weak_ptr<Resource> weak=owner;
  List list; list.bind(r,old); owner.reset();
  assert(!weak.expired()); ++checks;
  bool rejected=false; try {r.create(7);} catch(const std::runtime_error&) {rejected=true;}
  assert(rejected); ++checks;
  auto epoch=q.submit(std::move(list));
  assert(!weak.expired() && q.pending.size()==1); ++checks;
  q.complete(epoch); assert(weak.expired()); ++checks;
  auto newer=r.create(7); assert(newer->key.gen==old.gen+1); ++checks;
  rejected=false; try {List stale; stale.bind(r,old);} catch(const std::runtime_error&) {rejected=true;}
  assert(rejected); ++checks;
  std::weak_ptr<Resource> discarded;
  {auto p=r.create(8); discarded=p; List l; l.bind(r,p->key); p.reset();
   assert(!discarded.expired()); ++checks;}
  assert(discarded.expired()); ++checks;
  auto p=r.create(9); std::weak_ptr<Resource> later=p;
  List first,second; first.bind(r,p->key); second.bind(r,p->key); p.reset();
  auto e1=q.submit(std::move(first)),e2=q.submit(std::move(second));
  q.complete(e1); assert(!later.expired()); ++checks;
  q.complete(e2); assert(later.expired()); ++checks;
  std::cout<<"PASS "<<checks<<" retention assertions\n";
}
