#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

struct Token { int id; };
struct Submission {
  unsigned queue;
  uint64_t value;
  std::vector<std::shared_ptr<Token>> holds;
};
class Retirement {
  std::vector<uint64_t> submitted_, completed_;
  std::vector<Submission> pending_;
public:
  explicit Retirement(unsigned queues) : submitted_(queues,0), completed_(queues,0) {}
  uint64_t submit(unsigned q, std::vector<std::shared_ptr<Token>> holds) {
    if(q>=submitted_.size() || submitted_[q]==UINT64_MAX) throw std::invalid_argument("invalid queue/value");
    auto value=++submitted_[q];
    pending_.push_back({q,value,std::move(holds)});
    return value;
  }
  void observe(unsigned q,uint64_t value) {
    if(q>=submitted_.size() || value<completed_[q] || value>submitted_[q]) throw std::invalid_argument("invalid completion");
    completed_[q]=value;
    for(auto it=pending_.begin();it!=pending_.end();) {
      if(it->queue==q && it->value<=value) it=pending_.erase(it);
      else ++it;
    }
  }
  size_t pending() const {return pending_.size();}
};
int main(){
  Retirement r(2);
  auto a=std::make_shared<Token>(Token{1});
  auto b=std::make_shared<Token>(Token{2});
  std::weak_ptr<Token> wa=a,wb=b;
  assert(r.submit(0,{a})==1);
  assert(r.submit(1,{a,b})==1);
  a.reset(); b.reset();
  assert(!wa.expired()&&!wb.expired());
  r.observe(0,1);
  assert(r.pending()==1&&!wa.expired()&&!wb.expired());
  r.observe(1,0);
  assert(!wa.expired());
  bool rejected=false;
  try{r.observe(1,2);}catch(const std::invalid_argument&){rejected=true;}
  assert(rejected&&!wa.expired());
  r.observe(1,1);
  assert(r.pending()==0&&wa.expired()&&wb.expired());
  auto c=std::make_shared<Token>(Token{3});
  std::weak_ptr<Token> wc=c;
  assert(r.submit(0,{c})==2);
  c.reset();
  r.observe(1,1);
  assert(!wc.expired());
  r.observe(0,2);
  assert(wc.expired());
  std::cout<<"PASS 12 multi-queue ownership assertions\n";
}
