#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fixture {
void require(bool ok, const char* why) { if (!ok) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
struct VmaAllocation_T { std::vector<uint8_t> cpu, gpu; };
using VmaAllocation = VmaAllocation_T*;
using VmaAllocator = int;
VmaAllocator vma = 0;
std::vector<std::unique_ptr<VmaAllocation_T>> allocations;
bool coherent = false, fail_buffer = false;
uint64_t rec_value = 1, completed_value = 0;
struct { VkPhysicalDeviceProperties props{}; } dev;
int flushes = 0;
bool create_buffer(uint64_t n, VkBufferUsageFlags, bool, VkBuffer& b, VmaAllocation& a, void** map) {
    if (fail_buffer) return false;
    allocations.push_back(std::make_unique<VmaAllocation_T>());
    a = allocations.back().get(); a->cpu.resize(n); a->gpu.resize(n);
    b = reinterpret_cast<VkBuffer>(a); *map = a->cpu.data(); return true;
}
VkResult vmaFlushAllocation(VmaAllocator, VmaAllocation a, uint64_t offset, uint64_t size) {
    require(a && offset <= a->cpu.size() && size <= a->cpu.size() - offset, "allocation-relative flush bounds");
    const uint64_t atom = dev.props.limits.nonCoherentAtomSize;
    const uint64_t lo = offset / atom * atom, hi = std::min<uint64_t>(a->cpu.size(), (offset + size + atom - 1) / atom * atom);
    std::copy(a->cpu.begin() + lo, a->cpu.begin() + hi, a->gpu.begin() + lo); flushes++; return VK_SUCCESS;
}
void logf(const char*, ...) {}
void consume(VkCommandBuffer) {
    for (auto& a : allocations) {
        if (coherent) a->gpu = a->cpu;
        require(a->cpu == a->gpu, "GPU texture consumer sees all authored staging bytes");
    }
}
VkCommandBuffer upload_cmd() { return VK_NULL_HANDLE; }
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
bool overlaps(const Ranges* r, uint64_t a, uint64_t b) {
    return !r || std::any_of(r->begin(), r->end(), [=](auto x) { return a < x.second && b > x.first; });
}
struct Texture {
    bool astc = false; uint32_t block_w = 1, block_h = 1;
    VkImageType type = VK_IMAGE_TYPE_2D; VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t layers = 2; struct { uint64_t layer_size = 64; } L;
    uint64_t gpu = 0;
};
struct { uint64_t offset = 0; } geo;
auto level_geo(const Texture&, int) { return geo; }
bool astc_ready() { return true; }
std::vector<uint8_t> guest(1024, 0x5b);
const uint8_t* guest_bytes(uint64_t at, uint64_t n, std::vector<uint8_t>&) {
    require(at <= guest.size() && n <= guest.size() - at, "authored guest bounds"); return guest.data() + at;
}
int64_t st_bytes = 0;
struct { uint32_t w=8, tw=8, d=1; } g;
Texture tex;
uint32_t px_h=2, th=2, level=0, layer=0, y0=0;
VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
std::vector<VkBufferImageCopy> copies;
/* ACTUAL */

void cpu(uint64_t n, uint8_t pattern) {
    auto s = staging(n, 16); require(s.data && s.allocation, "slice owns allocation");
    std::memset(s.data, pattern, n); cpu_slice(s, n); consume({});
    require(copies.back().bufferOffset == s.offset, "copy and publication address same slice");
}
int run(int argc, char** argv) {
    require(argc == 3, "case/cache arguments"); coherent = std::string(argv[2]) == "coherent";
    dev.props.limits.nonCoherentAtomSize = 1024;
    dev.props.limits.minStorageBufferOffsetAlignment = 256;
    const std::string test = argv[1];
    if (test == "cpu") { cpu(37, 0x5b); cpu(513, 0xa7); require(flushes == 2, "one flush per completed slice"); }
    else if (test == "astc") {
        cpu(37, 0x3c); require(upload_astc_gpu(tex, 128, nullptr), "ASTC gathered layers published");
        Ranges dirty{{64,128}}; require(upload_astc_gpu(tex, 128, &dirty), "dirty layer published");
        require(flushes == 3, "ASTC flushes each gathered allocation slice");
    } else if (test == "atom") {
        auto a = staging(17,16), b = staging(19,16);
        require(a.offset % 1024 == 0 && b.offset % 1024 == 0 && b.offset >= a.offset + 1024, "distinct noncoherent atoms");
    } else if (test == "reuse") {
        cpu(37,0x5b); auto old = arenas.front().buffer; completed_value = rec_value++;
        cpu(37,0xa7); require(arenas.size() == 1 && arenas.front().buffer == old, "completed arena reused with new bytes");
    } else if (test == "multiple") {
        cpu(1u << 20,0x5b); cpu(31,0xa7); require(arenas.size() == 2, "multiple arenas independently published");
    } else if (test == "failure") {
        fail_buffer = true; auto s = staging(64); require(!s.buffer && !s.data && !s.allocation && flushes == 0, "failed slice unpublished");
    } else if (test == "empty-range") {
        flush_mapped_range(nullptr, 0, 0); require(flushes == 0, "empty range no VMA call");
    } else require(false, "known case");
    return 0;
}
} // namespace fixture
int main(int argc, char** argv) { return fixture::run(argc, argv); }
