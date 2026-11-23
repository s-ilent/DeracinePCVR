// Host-only semantic command recorder regression. No UE/PS4/OpenXR dependencies.
// Build: c++ -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer command_recorder_test.cpp -o command_recorder_test
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
struct Key { uint32_t id, gen; };
struct Registry {
  std::map<uint32_t,uint32_t> live;
  bool valid(Key k) const { auto it=live.find(k.id); return it!=live.end() && it->second==k.gen; }
};
struct Command {
  enum Kind { Constant, Texture, Local, Draw } kind;
  int slot=0;
  std::vector<uint8_t> bytes;
  Key key{}, layout{};
};
struct Recorder {
  std::vector<Command> cmds;
  void constant(int slot,const uint8_t* p,size_t n) {
    Command c; c.kind=Command::Constant; c.slot=slot; c.bytes.assign(p,p+n); cmds.push_back(c);
  }
  void texture(int slot,Key k) {
    Command c; c.kind=Command::Texture; c.slot=slot; c.key=k; cmds.push_back(c);
  }
  void local(Key layout,const uint8_t* p,size_t n) {
    Command c; c.kind=Command::Local; c.layout=layout; c.bytes.assign(p,p+n); cmds.push_back(c);
  }
  void draw() { Command c; c.kind=Command::Draw; cmds.push_back(c); }
};
struct Exec {
  Registry textures,layouts;
  std::map<int,std::vector<uint8_t>> constants;
  std::map<int,Key> bound;
  int draws=0;
  void run(const Recorder& r) {
    for(const auto& c:r.cmds) {
      switch(c.kind) {
        case Command::Constant: constants[c.slot]=c.bytes; break;
        case Command::Texture:
          if(!textures.valid(c.key)) throw std::runtime_error("stale texture");
          bound[c.slot]=c.key; break;
        case Command::Local:
          if(!layouts.valid(c.layout)) throw std::runtime_error("stale layout");
          break;
        case Command::Draw: ++draws; break;
      }
    }
  }
};
int main() {
  int pass=0;
  auto check=[&](bool b) { if(!b) throw std::runtime_error("assertion failed"); ++pass; };
  uint8_t b[4]={1,2,3,4};
  Recorder r; r.constant(0,b,4); std::memset(b,9,4);
  Exec e; e.run(r);
  check(e.constants[0]==std::vector<uint8_t>({1,2,3,4}));
  Recorder stale; stale.texture(1,{7,1});
  Exec e2; e2.textures.live[7]=2;
  bool rejected=false;
  try { e2.run(stale); } catch(const std::runtime_error&) { rejected=true; }
  check(rejected);
  uint8_t local[16]={3};
  Recorder l; l.local({8,1},local,16); local[0]=9;
  Exec e3; e3.layouts.live[8]=1; e3.run(l);
  check(l.cmds[0].bytes[0]==3);
  e3.layouts.live[8]=2; rejected=false;
  try { e3.run(l); } catch(const std::runtime_error&) { rejected=true; }
  check(rejected);
  Recorder ordered; uint8_t a=1,z=2;
  ordered.constant(0,&a,1); ordered.texture(1,{7,2});
  ordered.constant(0,&z,1); ordered.draw();
  Exec e4; e4.textures.live[7]=2; e4.run(ordered);
  check(e4.draws==1 && e4.constants[0][0]==2 && e4.bound[1].gen==2);
  std::cout<<"PASS "<<pass<<" host-only semantic assertions\n";
}
