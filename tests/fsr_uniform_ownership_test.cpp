#define _CRT_SECURE_NO_WARNINGS
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
static void require(bool ok,const char* why) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
struct Allocation { std::vector<uint8_t> cpu,visible; };
using VmaAllocation=Allocation*;
struct { VkDevice device{}; VkQueue queue{}; VkPhysicalDeviceProperties props{}; std::mutex present_mu; } dev;
std::map<VkBuffer,Allocation> buffers;
uintptr_t serial=100;
template<class T> T handle() { return reinterpret_cast<T>(++serial); }
bool coherent=false,fail_buffer=false;
unsigned flushed=0,queued=0;
std::mutex par_mutex;
struct ParLock { std::lock_guard<std::mutex> lock{par_mutex}; };
void logf(const char*,...) {}
bool create_buffer(uint64_t size,VkBufferUsageFlags,bool,VkBuffer& buffer,VmaAllocation& allocation,void** mapped,bool=false) {
    if(fail_buffer) return false;
    buffer=handle<VkBuffer>(); auto& a=buffers[buffer]; a.cpu.assign((size_t)size,0x9a); a.visible.assign((size_t)size,0xc3);
    allocation=&a; *mapped=a.cpu.data(); return true;
}
void destroy_buffer(VkBuffer buffer,VmaAllocation allocation) { require(&buffers.at(buffer)==allocation,"allocation owner"); buffers.erase(buffer); }
void flush_mapped(VmaAllocation allocation,uint64_t size) {
    if(!size)return;
    require(allocation && size<=allocation->cpu.size(),"valid stream flush range");
    std::copy_n(allocation->cpu.begin(),(size_t)size,allocation->visible.begin()); flushed++;
}
/* RES */
std::deque<RecordingRes> res;
thread_local RecordingRes* cur=nullptr;
const uint64_t kStream=1024;
constexpr VkBufferUsageFlags kStreamUse=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
std::atomic<uint64_t> stream_extra{0},stream_grown{0},stream_peak{0};
bool grow_off=false; bool stream_grow_off() { return grow_off; }
/* BUFFER_TYPES */

/* STREAM */
VkCommandBuffer rec_cmd{};
uint64_t rec_value=0,targets_serial=0;
VkBuffer zero_buffer{}; VmaAllocation zero_alloc{};
VkDescriptorPool make_pool() { return handle<VkDescriptorPool>(); }
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice,VkDescriptorPool,VkDescriptorPoolResetFlags) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice,VkDescriptorPool,const VkAllocationCallbacks*) {}
struct Pass {}; Pass& pass() { static Pass p;return p; }
bool work_since_barrier=false,last_dyn_valid=false,last_desc_valid=false,last_vb_valid=false;
VkPipeline last_pipeline{};
struct { bool on=false; } vertex_set,last_ib;
void descriptors_disturbed() {}
std::unordered_map<int,int>& handle_hits() { static std::unordered_map<int,int> h;return h; }
struct BankCache { uint64_t at=0; };
std::array<std::array<BankCache,2>,2>& bank_cache() { static std::array<std::array<BankCache,2>,2> b;return b; }
void make_dummies() {}
/* BEGIN */
struct Use { BufferRef ref;std::vector<uint8_t> expected; };
std::vector<Use> pending;
/* BACKEND */

static void read_pending() {
    for(auto& u:pending) {
        auto& a=buffers.at(u.ref.buffer);auto& seen=coherent?a.cpu:a.visible;
        require(u.ref.offset+u.expected.size()<=seen.size(),"descriptor within allocation");
        require(std::equal(u.expected.begin(),u.expected.end(),seen.begin()+u.ref.offset),"GPU must see this evaluation's authored constants");
    }
}
static void evaluate(unsigned seed) {
    auto u=upload(seed);require(bool(u),"uniform upload succeeds");
    require(u.offset%std::max<uint64_t>(256,dev.props.limits.minUniformBufferOffsetAlignment)==0,"uniform offset aligned");
    pending.push_back({u,std::vector<uint8_t>(sizeof(Constants),uint8_t(seed))});
}
int main(int argc,char**argv) {
    require(argc==3,"mode/cache");std::string mode=argv[1];coherent=std::string(argv[2])=="coherent";
    dev.props.limits.minUniformBufferOffsetAlignment=512;make_uniform_layout();
    if(mode=="no-recording") { require(!upload(1),"no recording returns empty");return 0; }
    draw_begin_recording(0,handle<VkCommandBuffer>(),1);
    if(mode=="initial-missing") { cur->mapped=nullptr;require(!upload(1),"missing mapping is not a slice");return 0; }
    if(mode=="empty") { auto before=cur->used;require(!uniform_upload(0).binding && cur->used==before,"empty upload does not consume space");return 0; }
    if(mode=="admission-failure") {
        cur->used=cur->size;fail_buffer=true;g.lut_uploaded=false;
        auto before=cur->used;Inputs in;g.made[0].image=handle<VkImage>();
        require(!admitted(VK_NULL_HANDLE,in),"failed reservation rejects evaluation");
        require(cur->used==before && g.first_evaluation && g.first_frame && !g.lut_uploaded && !g.sequence && !g.parity,"failure consumes no history, initial-layout/LUT or sequence state");
        fail_buffer=false;require(admitted(VK_NULL_HANDLE,in),"same input can retry reservation");return 0;
    }
    evaluate(1);
    if(mode=="failure" || mode=="growth-off") {
        cur->used=cur->size;auto before=cur->used;fail_buffer=mode=="failure";grow_off=mode=="growth-off";
        require(!upload(2) && cur->used==before,"failed growth preserves current stream and returns empty");
        fail_buffer=false;grow_off=false;evaluate(3);read_pending();return 0;
    }
    if(mode=="workers") {
        std::thread a([]{draw_begin_recording(1,handle<VkCommandBuffer>(),2);evaluate(2);});a.join();
        std::thread b([]{draw_begin_recording(2,handle<VkCommandBuffer>(),3);evaluate(3);});b.join();
        require(cur==&res[0],"main current recording is distinct from worker TLS");evaluate(4);read_pending();return 0;
    }
    for(unsigned i=2;i<=9;i++)evaluate(i);
    if(mode=="growth") { auto more=uniform_upload(3u<<20);require(bool(more.binding),"large upload grows stream");evaluate(10); }
    read_pending();
    if(mode=="reuse") {
        pending.clear(); // Only after the authored GPU reads have completed may this slot reset.
        draw_begin_recording(0,handle<VkCommandBuffer>(),2);evaluate(11);read_pending();
    }
    require(flushed>=9,"native uploads publish their own constants before queue use");
}
