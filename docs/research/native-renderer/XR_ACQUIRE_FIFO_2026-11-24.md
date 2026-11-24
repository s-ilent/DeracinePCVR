# OpenXR acquisition FIFO and timeout semantics — 2026-11-24

Scope: independent native renderer; follow-up to XR_IMAGE_STATE_HOST_TICKETS. Live main e2f981c40cba7c8490177c20c179cb3608cc1442; foundation head 6b359f520b9f41f854b2aac41bb74526f6c60e87; parent 3a8f1e997144d159663b8ba95a6e9b1365029f34. No game or emulator changes.

## Pinned evidence
OpenXR-Docs release-1.1.36 resolved to 2989f51fc9630db4fed147cd7864dc48934ae2f7.
[rendering.adoc 405–411](https://github.com/KhronosGroup/OpenXR-Docs/blob/2989f51fc9630db4fed147cd7864dc48934ae2f7/specification/sources/chapters/rendering.adoc#L405-L411): acquire returns an index, not permission to write.
[455–469](https://github.com/KhronosGroup/OpenXR-Docs/blob/2989f51fc9630db4fed147cd7864dc48934ae2f7/specification/sources/chapters/rendering.adoc#L455-L469): wait targets oldest pending acquisition; release required before next successful wait; timeout retries same index. XR_SUCCEEDED is true for XR_TIMEOUT_EXPIRED.
[502–513](https://github.com/KhronosGroup/OpenXR-Docs/blob/2989f51fc9630db4fed147cd7864dc48934ae2f7/specification/sources/chapters/rendering.adoc#L502-L513): release targets FIFO head; end frame selects last released image, one index per swapchain, potentially containing both eyes as array layers.

## Consequence
The previous simplified double-acquire rejection is too restrictive if applied per swapchain rather than per image. Maintain a FIFO per swapchain, not a single acquired index or arbitrary per-image wait. A positive timeout is not permission to render. A stereo array is one acquired image, not two independent acquisitions. Host GPU retirement remains separate. Proposed adapter explicitly handles timeout and session-loss statuses; this experiment only models XR_SUCCESS, timeout and generic failure, not session loss or static swapchains.

## Executable experiment
Locally compiled C++17 with -fsanitize=address,undefined -g; exit 0, PASS. Test source below (integer result codes are synthetic; 0 success, 1 timeout, -1 error). This is a CPU contract test, not runtime conformance.

```cpp
#include <deque>
#include <cassert>
#include <algorithm>
#include <optional>
struct Swapchain {
 std::deque<unsigned> pending; bool ready=false; std::optional<unsigned> last;
 bool acquire(unsigned i){if(std::find(pending.begin(),pending.end(),i)!=pending.end())return false;pending.push_back(i);return true;}
 bool wait(int result){if(pending.empty()||ready||result!=0)return false;ready=true;return true;}
 bool release(){if(!ready)return false;last=pending.front();pending.pop_front();ready=false;return true;}
};
int main(){Swapchain s; assert(!s.wait(0));assert(s.acquire(2));assert(s.acquire(0));assert(!s.acquire(2));assert(!s.wait(1));assert(s.pending.front()==2);assert(!s.release());assert(s.wait(0));assert(!s.wait(0));assert(s.release());assert(s.last==2);assert(s.wait(0));assert(s.release());assert(s.last==0);assert(s.acquire(2));assert(!s.wait(-1));assert(s.wait(0));assert(s.release());}
```

## Next experiment
On actual OpenXR runtime acquire two images where supported, force or observe timeout, retry FIFO head, release and wait next; verify stereo array layer selection and last-released semantics. Local environment has Vulkan loader but no Vulkan headers or ICD directory at /usr/share/vulkan/icd.d; no real GPU/runtime execution attempted. Do not interpret model as NRI integration or binary compatibility. Non-rendering runtime remains separate.
