// RX_VK_RECORDER's packets against a fake Vulkan (tests/test_recorder_sync.py builds it with the real vk_work.h and vk_recorder.h).
// Every wrapped vkCmd* goes to a fake that logs the call with its arguments fully dereferenced, the way a driver reads them (by the
// counts and descriptor types, the pointers themselves never), so two logs are equal exactly when the driver was handed the same
// values. A frame of representative calls is made the way the renderer makes them (thread-local descriptor writes reset at every draw,
// push constants and vertex arrays on the stack, a rendering info with attachments on the stack, barriers, a reused copy vector,
// dynamic state arrays), and with `mutate` the caller poisons (0xCD) all of that right after each call, as the next draw reuses it.
//
//   recorder_sync_test            the checks; exits 1 on the first difference
//   recorder_sync_test --bench N  the packet cost per draw on the fake (fakes that do nothing), ns, median of 5
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "vk_work.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int);   // (a fault exits, without a dialog)
#endif

namespace mkwvk {
std::atomic<uint64_t> work_epoch{0};
}
using namespace mkwvk;

// ---- the fake driver's log
struct Call {
    std::string op, bytes;
};
static std::vector<Call> g_log;
static bool g_quiet = false;   // (bench: the fakes only count)
static uint64_t g_calls = 0;

static void begin(const char* op) { g_log.push_back({op, {}}); }
template <class T>
static void put(const T& v) {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "fields one by one: no padding, no pointers");
    g_log.back().bytes.append(reinterpret_cast<const char*>(&v), sizeof v);
}
template <class T>
static void handle(T h) {
    put((uint64_t)(uintptr_t)h);
}
static void raw(const void* p, size_t n) { g_log.back().bytes.append(static_cast<const char*>(p), n); }
static void tag(const char* s) { g_log.back().bytes.append(s).push_back(0); }
// (a pointer or count the caller has since poisoned, which a missed copy hands over: logged as such, never followed)
static bool readable(const void* p, uint64_t n) {
    if ((uintptr_t)p == (uintptr_t)0xCDCDCDCDCDCDCDCDull || n > 4096) {
        tag("UNREADABLE");
        return false;
    }
    return true;
}
template <class T, class F>
static void each(const T* p, uint64_t n, F f) {
    put((uint32_t)n);
    if (!n || !readable(p, n)) return;
    for (uint64_t i = 0; i < n; i++) f(p[i]);
}
template <class T, class F>
static void opt(const T* p, F f) {   // (an optional struct: there or not, then what it holds)
    put((uint8_t)(p != nullptr));
    if (p && readable(p, 1)) f(*p);
}

static void L(const VkImageSubresourceRange& r) { put(r.aspectMask), put(r.baseMipLevel), put(r.levelCount), put(r.baseArrayLayer), put(r.layerCount); }
static void L(const VkImageSubresourceLayers& r) { put(r.aspectMask), put(r.mipLevel), put(r.baseArrayLayer), put(r.layerCount); }
static void L(const VkOffset3D& o) { put(o.x), put(o.y), put(o.z); }
static void L(const VkExtent3D& e) { put(e.width), put(e.height), put(e.depth); }
static void L(const VkRect2D& r) { put(r.offset.x), put(r.offset.y), put(r.extent.width), put(r.extent.height); }
static void L(const VkViewport& v) { put(v.x), put(v.y), put(v.width), put(v.height), put(v.minDepth), put(v.maxDepth); }
static void L(const VkClearValue& v) { raw(&v, sizeof v); }   // (a union of 16 bytes, no padding)
static void L(const VkClearColorValue& v) { raw(&v, sizeof v); }
static void L(const VkClearDepthStencilValue& v) { put(v.depth), put(v.stencil); }
static void L(const VkMemoryBarrier2& b) {
    put(b.sType), put((uint8_t)!!b.pNext), put(b.srcStageMask), put(b.srcAccessMask), put(b.dstStageMask), put(b.dstAccessMask);
}
static void L(const VkBufferMemoryBarrier2& b) {
    put(b.sType), put((uint8_t)!!b.pNext), put(b.srcStageMask), put(b.srcAccessMask), put(b.dstStageMask), put(b.dstAccessMask);
    put(b.srcQueueFamilyIndex), put(b.dstQueueFamilyIndex), handle(b.buffer), put(b.offset), put(b.size);
}
static void L(const VkImageMemoryBarrier2& b) {
    put(b.sType), put((uint8_t)!!b.pNext), put(b.srcStageMask), put(b.srcAccessMask), put(b.dstStageMask), put(b.dstAccessMask);
    put(b.oldLayout), put(b.newLayout), put(b.srcQueueFamilyIndex), put(b.dstQueueFamilyIndex), handle(b.image), L(b.subresourceRange);
}
static void L(const VkRenderingAttachmentInfo& a) {
    put(a.sType), put((uint8_t)!!a.pNext), handle(a.imageView), put(a.imageLayout), put(a.resolveMode), handle(a.resolveImageView);
    put(a.resolveImageLayout), put(a.loadOp), put(a.storeOp), L(a.clearValue);
}
static void L(const VkBufferCopy& c) { put(c.srcOffset), put(c.dstOffset), put(c.size); }
static void L(const VkBufferImageCopy& c) {
    put(c.bufferOffset), put(c.bufferRowLength), put(c.bufferImageHeight), L(c.imageSubresource), L(c.imageOffset), L(c.imageExtent);
}
static void L(const VkImageCopy& c) { L(c.srcSubresource), L(c.srcOffset), L(c.dstSubresource), L(c.dstOffset), L(c.extent); }
static void L(const VkImageResolve& c) { L(c.srcSubresource), L(c.srcOffset), L(c.dstSubresource), L(c.dstOffset), L(c.extent); }
static void L(const VkImageBlit& b) {
    L(b.srcSubresource), L(b.srcOffsets[0]), L(b.srcOffsets[1]), L(b.dstSubresource), L(b.dstOffsets[0]), L(b.dstOffsets[1]);
}
static void L(const VkClearAttachment& a) { put(a.aspectMask), put(a.colorAttachment), L(a.clearValue); }
static void L(const VkClearRect& r) { L(r.rect), put(r.baseArrayLayer), put(r.layerCount); }
static void L(const VkColorBlendEquationEXT& e) {
    put(e.srcColorBlendFactor), put(e.dstColorBlendFactor), put(e.colorBlendOp), put(e.srcAlphaBlendFactor), put(e.dstAlphaBlendFactor),
        put(e.alphaBlendOp);
}
static void L(const VkWriteDescriptorSet& w) {   // (as a driver reads a push: by type, one of the three arrays)
    put(w.sType), put((uint8_t)!!w.pNext), handle(w.dstSet), put(w.dstBinding), put(w.dstArrayElement), put(w.descriptorType);
    switch (rec::write_kind(w.descriptorType)) {
    case 1: each(w.pImageInfo, w.descriptorCount, [](const VkDescriptorImageInfo& i) { handle(i.sampler), handle(i.imageView), put(i.imageLayout); }); break;
    case 2: each(w.pBufferInfo, w.descriptorCount, [](const VkDescriptorBufferInfo& b) { handle(b.buffer), put(b.offset), put(b.range); }); break;
    case 3: each(w.pTexelBufferView, w.descriptorCount, [](VkBufferView v) { handle(v); }); break;
    default: put(w.descriptorCount); break;   // (not read here: the packer leaves these to the direct call)
    }
}
template <class T>
static void A(const T* p, uint64_t n) {
    each(p, n, [](const T& v) { L(v); });
}

#define QUIET \
    if (g_quiet) { g_calls++; return; }
#define F(name, params, ...) \
    static void VKAPI_CALL fake_##name params { QUIET begin(#name); __VA_ARGS__ }

F(vkCmdPipelineBarrier2, (VkCommandBuffer cb, const VkDependencyInfo* d), handle(cb); put(d->sType), put((uint8_t)!!d->pNext), put(d->dependencyFlags);
  A(d->pMemoryBarriers, d->memoryBarrierCount), A(d->pBufferMemoryBarriers, d->bufferMemoryBarrierCount);
  A(d->pImageMemoryBarriers, d->imageMemoryBarrierCount);)
F(vkCmdBeginRendering, (VkCommandBuffer cb, const VkRenderingInfo* r), handle(cb); put(r->sType), put((uint8_t)!!r->pNext), put(r->flags);
  L(r->renderArea), put(r->layerCount), put(r->viewMask); A(r->pColorAttachments, r->colorAttachmentCount);
  opt(r->pDepthAttachment, [](const VkRenderingAttachmentInfo& a) { L(a); }); opt(r->pStencilAttachment, [](const VkRenderingAttachmentInfo& a) { L(a); });)
F(vkCmdEndRendering, (VkCommandBuffer cb), handle(cb);)
F(vkCmdDraw, (VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d), handle(cb), put(a), put(b), put(c), put(d);)
F(vkCmdDrawIndexed, (VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e), handle(cb), put(a), put(b), put(c), put(d), put(e);)
F(vkCmdDrawIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s), handle(cb), handle(b), put(o), put(n), put(s);)
F(vkCmdDrawIndexedIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s), handle(cb), handle(b), put(o), put(n), put(s);)
F(vkCmdDrawIndirectCount, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m, uint32_t s),
  handle(cb), handle(b), put(o), handle(cbuf), put(co), put(m), put(s);)
F(vkCmdDrawIndexedIndirectCount, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m, uint32_t s),
  handle(cb), handle(b), put(o), handle(cbuf), put(co), put(m), put(s);)
F(vkCmdDrawMeshTasksEXT, (VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z), handle(cb), put(x), put(y), put(z);)
F(vkCmdDrawMeshTasksIndirectEXT, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s), handle(cb), handle(b), put(o), put(n), put(s);)
F(vkCmdDrawMeshTasksIndirectCountEXT, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m, uint32_t s),
  handle(cb), handle(b), put(o), handle(cbuf), put(co), put(m), put(s);)
F(vkCmdDispatch, (VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z), handle(cb), put(x), put(y), put(z);)
F(vkCmdDispatchIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o), handle(cb), handle(b), put(o);)
F(vkCmdCopyBuffer, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r), handle(cb), handle(s), handle(d); A(r, n);)
F(vkCmdCopyBufferToImage, (VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r),
  handle(cb), handle(s), handle(d), put(l); A(r, n);)
F(vkCmdCopyImageToBuffer, (VkCommandBuffer cb, VkImage s, VkImageLayout l, VkBuffer d, uint32_t n, const VkBufferImageCopy* r),
  handle(cb), handle(s), put(l), handle(d); A(r, n);)
F(vkCmdCopyImage, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r),
  handle(cb), handle(s), put(sl), handle(d), put(dl); A(r, n);)
F(vkCmdBlitImage, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f),
  handle(cb), handle(s), put(sl), handle(d), put(dl); A(r, n); put(f);)
F(vkCmdResolveImage, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve* r),
  handle(cb), handle(s), put(sl), handle(d), put(dl); A(r, n);)
F(vkCmdClearColorImage, (VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* c, uint32_t n, const VkImageSubresourceRange* r),
  handle(cb), handle(i), put(l); opt(c, [](const VkClearColorValue& v) { L(v); }); A(r, n);)
F(vkCmdClearDepthStencilImage, (VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* c, uint32_t n,
  const VkImageSubresourceRange* r), handle(cb), handle(i), put(l); opt(c, [](const VkClearDepthStencilValue& v) { L(v); }); A(r, n);)
F(vkCmdClearAttachments, (VkCommandBuffer cb, uint32_t na, const VkClearAttachment* a, uint32_t nr, const VkClearRect* r),
  handle(cb); A(a, na); A(r, nr);)
F(vkCmdFillBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t d), handle(cb), handle(b), put(o), put(s), put(d);)
F(vkCmdUpdateBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, const void* d), handle(cb), handle(b), put(o), put(s);
  if (readable(d, s / 16)) raw(d, (size_t)s);)
F(vkCmdBeginQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f), handle(cb), handle(p), put(q), put(f);)
F(vkCmdEndQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q), handle(cb), handle(p), put(q);)
F(vkCmdBindPipeline, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p), handle(cb), put(bp), handle(p);)
F(vkCmdBindDescriptorSets, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet* s,
  uint32_t nd, const uint32_t* d), handle(cb), put(bp), handle(l), put(first); each(s, n, [](VkDescriptorSet v) { handle(v); });
  each(d, nd, [](uint32_t v) { put(v); });)
F(vkCmdBindIndexBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t), handle(cb), handle(b), put(o), put(t);)
F(vkCmdBindVertexBuffers2, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o, const VkDeviceSize* s,
  const VkDeviceSize* st), handle(cb), put(first); each(b, n, [](VkBuffer v) { handle(v); }); each(o, n, [](VkDeviceSize v) { put(v); });
  put((uint8_t)!!s); if (s) each(s, n, [](VkDeviceSize v) { put(v); }); put((uint8_t)!!st); if (st) each(st, n, [](VkDeviceSize v) { put(v); });)
F(vkCmdPushConstants, (VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags f, uint32_t o, uint32_t s, const void* v),
  handle(cb), handle(l), put(f), put(o), put(s); if (readable(v, s / 16)) raw(v, s);)
F(vkCmdPushDescriptorSetKHR, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n, const VkWriteDescriptorSet* w),
  handle(cb), put(bp), handle(l), put(set); A(w, n);)
F(vkCmdSetViewport, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v), handle(cb), put(first); A(v, n);)
F(vkCmdSetScissor, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r), handle(cb), put(first); A(r, n);)
F(vkCmdSetViewportWithCount, (VkCommandBuffer cb, uint32_t n, const VkViewport* v), handle(cb); A(v, n);)
F(vkCmdSetScissorWithCount, (VkCommandBuffer cb, uint32_t n, const VkRect2D* r), handle(cb); A(r, n);)
F(vkCmdSetBlendConstants, (VkCommandBuffer cb, const float b[4]), handle(cb); each(b, 4, [](float v) { put(v); });)
F(vkCmdSetStencilReference, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), handle(cb), put(f), put(v);)
F(vkCmdSetStencilCompareMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), handle(cb), put(f), put(v);)
F(vkCmdSetStencilWriteMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), handle(cb), put(f), put(v);)
F(vkCmdSetStencilOp, (VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp c, VkCompareOp d),
  handle(cb), put(f), put(a), put(b), put(c), put(d);)
F(vkCmdSetDepthBias, (VkCommandBuffer cb, float a, float b, float c), handle(cb), put(a), put(b), put(c);)
F(vkCmdSetDepthBiasEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetLineWidth, (VkCommandBuffer cb, float v), handle(cb), put(v);)
F(vkCmdSetDepthBoundsTestEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetCullMode, (VkCommandBuffer cb, VkCullModeFlags v), handle(cb), put(v);)
F(vkCmdSetFrontFace, (VkCommandBuffer cb, VkFrontFace v), handle(cb), put(v);)
F(vkCmdSetPrimitiveTopology, (VkCommandBuffer cb, VkPrimitiveTopology v), handle(cb), put(v);)
F(vkCmdSetPrimitiveRestartEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetDepthTestEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetDepthWriteEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetDepthCompareOp, (VkCommandBuffer cb, VkCompareOp v), handle(cb), put(v);)
F(vkCmdSetStencilTestEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetRasterizerDiscardEnable, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetPolygonModeEXT, (VkCommandBuffer cb, VkPolygonMode v), handle(cb), put(v);)
F(vkCmdSetDepthClampEnableEXT, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetAlphaToCoverageEnableEXT, (VkCommandBuffer cb, VkBool32 v), handle(cb), put(v);)
F(vkCmdSetColorBlendEnableEXT, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBool32* v), handle(cb), put(first);
  each(v, n, [](VkBool32 x) { put(x); });)
F(vkCmdSetColorBlendEquationEXT, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorBlendEquationEXT* v), handle(cb), put(first); A(v, n);)
F(vkCmdSetColorWriteMaskEXT, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorComponentFlags* v), handle(cb), put(first);
  each(v, n, [](VkColorComponentFlags x) { put(x); });)
F(vkCmdWriteBufferMarkerAMD, (VkCommandBuffer cb, VkPipelineStageFlagBits s, VkBuffer b, VkDeviceSize o, uint32_t m),
  handle(cb), put(s), handle(b), put(o), put(m);)
F(vkCmdSetCheckpointNV, (VkCommandBuffer cb, const void* m), handle(cb), handle(m);)   // (an opaque value: the pointer is the datum)
F(vkCmdWriteTimestamp2, (VkCommandBuffer cb, VkPipelineStageFlags2 s, VkQueryPool p, uint32_t q), handle(cb), put(s), handle(p), put(q);)
F(vkCmdResetQueryPool, (VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n), handle(cb), handle(p), put(f), put(n);)

namespace vkx {
#define RX_FAKE(f) PFN_##f f = fake_##f;
RX_REC_CALLS(RX_FAKE, RX_FAKE)
#undef RX_FAKE
} // namespace vkx

// ---- the renderer's shapes of argument storage
template <class T>
static T H(uint64_t v) {
    return (T)(uintptr_t)v;
}
template <class T>
static void poison(T* p, size_t n = 1) {
    std::memset((void*)p, 0xCD, sizeof(T) * n);
}
template <class T>
static void poison(std::vector<T>& v) {   // (the next draw's reset: the storage stays, its contents change)
    if (!v.empty()) poison(v.data(), v.size());
    v.clear();
}

static bool g_mutate = false;
constexpr uint32_t kPushSize = 96;   // (vk_draw.cpp's: MESH_FALLBACK_PUSH_END)

struct DescriptorWrites {   // (vk_draw.cpp's thread-local dw: reset at every build, the writes point into the info vectors)
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufs;
    std::vector<VkDescriptorImageInfo> imgs;
    std::vector<VkBufferView> views;
    void reset() {
        writes.clear(), bufs.clear(), imgs.clear(), views.clear();
        bufs.reserve(64), imgs.reserve(64), views.reserve(64);
    }
    void after_call() {
        if (g_mutate) poison(writes), poison(bufs), poison(imgs), poison(views);
    }
};
static thread_local DescriptorWrites dw;

static VkWriteDescriptorSet write(uint32_t binding, VkDescriptorType t, uint32_t count) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = binding, w.descriptorType = t, w.descriptorCount = count;
    return w;
}
static void build_graphics(int d) {
    dw.reset();
    for (uint32_t i = 0; i < 3; i++) {
        dw.bufs.push_back({H<VkBuffer>(0xB000 + d * 16 + i), 256ull * i + d, 64ull + i});
        VkWriteDescriptorSet w = write(i, i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
        w.pBufferInfo = &dw.bufs.back();
        if (i == 1) w.pImageInfo = (const VkDescriptorImageInfo*)(uintptr_t)0xCDCDCDCDCDCDCDCDull;   // (ignored for a buffer)
        dw.writes.push_back(w);
    }
    for (uint32_t i = 0; i < 4; i++) {
        dw.imgs.push_back({H<VkSampler>(0x5000 + i), H<VkImageView>(0x6000 + d * 16 + i), VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w = write(3 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1);
        w.pImageInfo = &dw.imgs.back();
        dw.writes.push_back(w);
    }
    const size_t at = dw.imgs.size();   // (an array binding: two sampled images)
    dw.imgs.push_back({VK_NULL_HANDLE, H<VkImageView>(0x6100 + d), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    dw.imgs.push_back({VK_NULL_HANDLE, H<VkImageView>(0x6200 + d), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    VkWriteDescriptorSet arr = write(7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2);
    arr.dstArrayElement = 0, arr.pImageInfo = &dw.imgs[at];
    dw.writes.push_back(arr);
    dw.views.push_back(H<VkBufferView>(0x7000 + d));
    VkWriteDescriptorSet tb = write(8, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1);
    tb.pTexelBufferView = &dw.views.back();
    dw.writes.push_back(tb);
}

static void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, int d) {
    uint8_t data[kPushSize]{};
    for (uint32_t i = 0; i < kPushSize; i++) data[i] = (uint8_t)(i * 7 + d);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, kPushSize, data);
    if (g_mutate) poison(data, kPushSize);
    const uint64_t firsts = 0x1000ull + d;   // (a mesh draw's firsts: 8 bytes at their offset)
    uint64_t f = firsts;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 80, 8, &f);
    if (g_mutate) poison(&f);
}

struct DynState {   // (vk_draw.cpp's DynState: the arrays the dynamic state calls point into)
    VkViewport vp;
    VkRect2D sc;
    float blend[4];
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
};
static void dynamic_state(VkCommandBuffer cmd, int d) {
    DynState s{};
    s.vp = {0, (float)d, 1280, 720, 0, 1};
    s.sc = {{0, d}, {1280, 720}};
    for (int i = 0; i < 4; i++) s.blend[i] = 0.25f * i + d;
    for (uint32_t i = 0; i < 8; i++) {
        s.en[i] = (i + d) & 1;
        s.eq[i] = {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO,
                   (VkBlendOp)(i % 3)};
        s.wm[i] = 0xF >> (i & 3);
    }
    vkCmdSetViewportWithCount(cmd, 1, &s.vp);
    vkCmdSetScissorWithCount(cmd, 1, &s.sc);
    vkCmdSetBlendConstants(cmd, s.blend);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, 1 + d);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, 2 + d);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, 0xFF);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, 0x0F);
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                      VK_COMPARE_OP_LESS_OR_EQUAL);
    vkCmdSetDepthBias(cmd, 1.5f, 0.0f, -2.0f + d);
    vkCmdSetDepthBiasEnable(cmd, VK_TRUE);
    if (d == 0) vkCmdSetLineWidth(cmd, 1.0f), vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);
    vkCmdSetDepthWriteEnable(cmd, d & 1);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_GREATER);
    vkCmdSetStencilTestEnable(cmd, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
    vkCmdSetPolygonModeEXT(cmd, VK_POLYGON_MODE_FILL);
    vkCmdSetDepthClampEnableEXT(cmd, VK_FALSE);
    vkCmdSetAlphaToCoverageEnableEXT(cmd, d == 2);
    vkCmdSetColorBlendEnableEXT(cmd, 0, 3, s.en);
    vkCmdSetColorBlendEquationEXT(cmd, 0, 3, s.eq);
    vkCmdSetColorWriteMaskEXT(cmd, 0, 3, s.wm);
    if (g_mutate) poison(&s);
}

static void vertex_streams(VkCommandBuffer cmd, int d) {
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16], vsizes[16], vstrides[16];
    for (uint32_t i = 0; i < 16; i++) vbufs[i] = H<VkBuffer>(0x9000 + i), voffs[i] = 64ull * i + d, vsizes[i] = 4096 - i, vstrides[i] = 12 + 4 * (i % 3);
    vkCmdBindVertexBuffers2(cmd, 0, 16, vbufs, voffs, vsizes, vstrides);
    if (g_mutate) poison(vbufs, 16), poison(voffs, 16), poison(vsizes, 16), poison(vstrides, 16);
    VkBuffer one = H<VkBuffer>(0x9100 + d);
    VkDeviceSize off = 32;
    vkCmdBindVertexBuffers2(cmd, 3, 1, &one, &off, nullptr, nullptr);   // (sizes and strides left out: null stays null)
    if (g_mutate) poison(&one), poison(&off);
}

static void begin_pass(VkCommandBuffer cmd, int pass) {   // (vk_draw.cpp's begin_pass: ca[8] and da on the stack)
    VkRenderingAttachmentInfo ca[8]{}, da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    for (uint32_t i = 0; i < 8; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = H<VkImageView>(0xA000 + pass * 16 + i);
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = i ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca[i].clearValue.color.float32[0] = 0.5f + i, ca[i].clearValue.color.float32[3] = 1.0f;
    }
    da.imageView = H<VkImageView>(0xA0FF + pass), da.imageLayout = VK_IMAGE_LAYOUT_GENERAL, da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE, da.clearValue.depthStencil = {0.0f, 7};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {1280u + pass, 720}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = ca;
    ri.pDepthAttachment = &da;
    ri.pStencilAttachment = pass ? &da : nullptr;   // (the same struct for both, or no stencil)
    vkCmdBeginRendering(cmd, &ri);
    if (g_mutate) poison(ca, 8), poison(&da), poison(&ri);
}

static void barrier(VkCommandBuffer cmd, int k) {   // (full_barrier / texture transitions: barriers on the stack)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | k;
    VkBufferMemoryBarrier2 bb[2]{{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2}, {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2}};
    for (int i = 0; i < 2; i++) bb[i].buffer = H<VkBuffer>(0xC000 + i), bb[i].offset = 16ull * k, bb[i].size = VK_WHOLE_SIZE, bb[i].srcQueueFamilyIndex = bb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkImageMemoryBarrier2 ib[3]{};
    for (int i = 0; i < 3; i++) {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].image = H<VkImage>(0xD000 + i + k);
        ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1 + (uint32_t)i, 0, VK_REMAINING_ARRAY_LAYERS};
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    di.bufferMemoryBarrierCount = 2, di.pBufferMemoryBarriers = bb;
    di.imageMemoryBarrierCount = k ? 3 : 0, di.pImageMemoryBarriers = ib;   // (a count of 0 with a pointer: not read)
    vkCmdPipelineBarrier2(cmd, &di);
    if (g_mutate) poison(&mb), poison(bb, 2), poison(ib, 3), poison(&di);
}

static thread_local std::vector<VkBufferCopy> copies;   // (vk_memory.cpp's mirror copies: a reused thread-local vector)
static void copy_runs(VkCommandBuffer up, int k) {
    copies.clear();
    for (int i = 0; i < 5 - k; i++) copies.push_back({4096ull * i, 8192ull * i + k, 256ull + i});
    vkCmdCopyBuffer(up, H<VkBuffer>(0xE000), H<VkBuffer>(0xE001 + k), (uint32_t)copies.size(), copies.data());
    if (g_mutate) poison(copies);
}

static void frame() {
    const VkCommandBuffer cmd = H<VkCommandBuffer>(0x100), up = H<VkCommandBuffer>(0x200);   // (the recording's and its upload one)
    const VkPipelineLayout layout = H<VkPipelineLayout>(0x300);
    vkCmdResetQueryPool(up, H<VkQueryPool>(0x400), 0, 64);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, H<VkQueryPool>(0x400), 0);
    copy_runs(up, 0);
    copy_runs(up, 2);
    {
        uint8_t data[64];
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)(255 - i);
        vkCmdUpdateBuffer(up, H<VkBuffer>(0xE100), 128, sizeof data, data);
        if (g_mutate) poison(data, 64);
    }
    vkCmdFillBuffer(cmd, H<VkBuffer>(0xE200), 0, VK_WHOLE_SIZE, 0);
    barrier(cmd, 0);
    for (int pass = 0; pass < 2; pass++) {
        begin_pass(cmd, pass);
        for (int d = 0; d < 3; d++) {   // (draw(): pipeline, descriptors, push constants, dynamic state, streams, the draw)
            const int at = pass * 3 + d;
            vkCmdWriteBufferMarkerAMD(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, H<VkBuffer>(0xF000), 8ull * at, 100 + at);
            vkCmdSetCheckpointNV(cmd, (void*)(uintptr_t)(at + 1));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, H<VkPipeline>(0x800 + at));
            build_graphics(at);
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, (uint32_t)dw.writes.size(), dw.writes.data());
            {   // (bind_descriptors' delta: the changed writes copied into another thread-local vector)
                static thread_local std::vector<VkWriteDescriptorSet> changed;
                changed.clear();
                changed.push_back(dw.writes[0]), changed.push_back(dw.writes[4]), changed.push_back(dw.writes[8]);
                vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, (uint32_t)changed.size(), changed.data());
                if (g_mutate) poison(changed);
            }
            dw.after_call();
            push_constants(cmd, layout, at);
            dynamic_state(cmd, at);
            vertex_streams(cmd, at);
            vkCmdBindIndexBuffer(cmd, H<VkBuffer>(0x9F00), 4ull * at, d ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            vkCmdBeginQuery(cmd, H<VkQueryPool>(0x401), at, 0);
            switch ((pass * 3 + d) % 6) {
            case 0: vkCmdDrawIndexed(cmd, 36, 1, 0, -4, 0); vkCmdDraw(cmd, 3, 1, 0, 0); break;
            case 1: vkCmdDrawIndirect(cmd, H<VkBuffer>(0x9E00), 16, 4, 16); vkCmdDrawIndexedIndirect(cmd, H<VkBuffer>(0x9E00), 64, 2, 20); break;
            case 2: vkCmdDrawIndirectCount(cmd, H<VkBuffer>(0x9E00), 0, H<VkBuffer>(0x9E01), 4, 8, 16);
                    vkCmdDrawIndexedIndirectCount(cmd, H<VkBuffer>(0x9E00), 0, H<VkBuffer>(0x9E01), 8, 8, 20); break;
            case 3: vkCmdDrawMeshTasksEXT(cmd, 4, 1, 1); break;
            case 4: vkCmdDrawMeshTasksIndirectEXT(cmd, H<VkBuffer>(0x9E02), 12, 3, 12); break;
            default: vkCmdDrawMeshTasksIndirectCountEXT(cmd, H<VkBuffer>(0x9E02), 0, H<VkBuffer>(0x9E01), 12, 5, 12); break;
            }
            vkCmdEndQuery(cmd, H<VkQueryPool>(0x401), at);
        }
        {   // (a clear inside the pass: a rect and an attachment on the stack; the shader clear's own viewport and scissor)
            VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 1, {}};
            ca.clearValue.color.uint32[2] = 0xABCD + pass;
            VkClearRect cr{{{0, 0}, {64, 32}}, 0, 1};
            vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
            if (g_mutate) poison(&ca), poison(&cr);
            VkViewport v{0, 0, 64, 32, 0, 1};
            VkRect2D rect{{0, 0}, {64, 32}};
            vkCmdSetViewport(cmd, 0, 1, &v);
            vkCmdSetScissor(cmd, 0, 1, &rect);
            if (g_mutate) poison(&v), poison(&rect);
        }
        vkCmdEndRendering(cmd);
        barrier(cmd, 1 + pass);
    }
    {   // (a compute dispatch: storage image, storage buffer, texel buffer; push constants; descriptor sets with dynamic offsets)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, H<VkPipeline>(0x900));
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, H<VkImageView>(0x6500), VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo bi{H<VkBuffer>(0xB500), 0, VK_WHOLE_SIZE};
        VkBufferView tv = H<VkBufferView>(0x7500);
        VkWriteDescriptorSet w[3] = {write(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1), write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
                                     write(2, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1)};
        w[0].pImageInfo = &ii, w[1].pBufferInfo = &bi, w[2].pTexelBufferView = &tv;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 3, w);
        if (g_mutate) poison(&ii), poison(&bi), poison(&tv), poison(w, 3);
        uint32_t size[3] = {640, 480, 1};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, size);
        if (g_mutate) poison(size, 3);
        vkCmdDispatch(cmd, 80, 60, 1);
        vkCmdDispatchIndirect(cmd, H<VkBuffer>(0x9D00), 48);
        VkDescriptorSet sets[2] = {H<VkDescriptorSet>(0x1100), H<VkDescriptorSet>(0x1101)};
        uint32_t dyn[3] = {256, 512, 768};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, sets, 3, dyn);
        if (g_mutate) poison(dyn, 3);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 2, 1, &sets[1], 0, dyn);   // (no dynamic offsets: not read)
        if (g_mutate) poison(sets, 2);
    }
    {   // (texture work: uploads, copies, blits, resolves, clears; regions and values on the stack)
        VkBufferImageCopy bic[2]{};
        for (int i = 0; i < 2; i++) bic[i] = {1024ull * i, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)i, 0, 1}, {i, 0, 0}, {64u >> i, 64u >> i, 1}};
        vkCmdCopyBufferToImage(up, H<VkBuffer>(0xE300), H<VkImage>(0xD100), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, bic);
        vkCmdCopyImageToBuffer(cmd, H<VkImage>(0xD101), VK_IMAGE_LAYOUT_GENERAL, H<VkBuffer>(0xE301), 1, &bic[1]);
        if (g_mutate) poison(bic, 2);
        VkImageCopy ic{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {1, 2, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {3, 4, 0}, {16, 16, 1}};
        vkCmdCopyImage(cmd, H<VkImage>(0xD102), VK_IMAGE_LAYOUT_GENERAL, H<VkImage>(0xD103), VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        if (g_mutate) poison(&ic);
        VkImageBlit bl{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {128, 64, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1}, {{0, 0, 0}, {64, 32, 1}}};
        vkCmdBlitImage(cmd, H<VkImage>(0xD104), VK_IMAGE_LAYOUT_GENERAL, H<VkImage>(0xD105), VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        if (g_mutate) poison(&bl);
        VkImageResolve rs{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {1280, 720, 1}};
        vkCmdResolveImage(cmd, H<VkImage>(0xD106), VK_IMAGE_LAYOUT_GENERAL, H<VkImage>(0xD107), VK_IMAGE_LAYOUT_GENERAL, 1, &rs);
        if (g_mutate) poison(&rs);
        VkClearColorValue col{};
        col.float32[0] = 0.1f, col.float32[1] = 0.2f, col.float32[2] = 0.3f, col.float32[3] = 1.0f;
        VkImageSubresourceRange ranges[2] = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 1, 2, 0, 6}};
        vkCmdClearColorImage(cmd, H<VkImage>(0xD108), VK_IMAGE_LAYOUT_GENERAL, &col, 2, ranges);
        if (g_mutate) poison(&col);
        VkClearDepthStencilValue ds{1.0f, 0x55};
        VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, H<VkImage>(0xD109), VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &dr);
        if (g_mutate) poison(&ds), poison(&dr), poison(ranges, 2);
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, H<VkQueryPool>(0x400), 1);
}

// (a call the packer does not take: a pNext chain, a descriptor type it does not know. Held packets go first, then the direct call)
static void fallbacks() {
    const VkCommandBuffer cmd = H<VkCommandBuffer>(0x100);
    vkCmdDraw(cmd, 1, 2, 3, 4);
    VkMemoryBarrier2 next{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.pNext = &next;
    vkCmdPipelineBarrier2(cmd, &di);
    vkCmdDraw(cmd, 5, 6, 7, 8);
    VkWriteDescriptorSet w = write(0, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 16);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, H<VkPipelineLayout>(0x300), 0, 1, &w);
    vkCmdDispatch(cmd, 1, 1, 1);
}

// ---- checks
static int failures = 0;
static std::vector<Call> run(void (*scenario)(), int mode, bool mutate) {
    rec::set_mode(mode);
    g_mutate = mutate;
    g_log.clear();
    scenario();
    if (mode == rec::DEFER && !g_log.empty() && scenario == frame) {
        std::printf("FAIL deferred: %zu calls reached the driver before the drain\n", g_log.size());
        failures++;
    }
    rec::drain();
    rec::set_mode(rec::OFF);
    return g_log;
}
static void same(const char* what, const std::vector<Call>& ref, const std::vector<Call>& got) {
    for (size_t i = 0; i < std::max(ref.size(), got.size()); i++) {
        if (i < ref.size() && i < got.size() && ref[i].op == got[i].op && ref[i].bytes == got[i].bytes) continue;
        size_t at = 0;
        if (i < ref.size() && i < got.size())
            while (at < ref[i].bytes.size() && at < got[i].bytes.size() && ref[i].bytes[at] == got[i].bytes[at]) at++;
        std::printf("MISMATCH %s: call %zu of %zu/%zu: expected %s, got %s (bytes differ from %zu)\n", what, i, ref.size(), got.size(),
                    i < ref.size() ? ref[i].op.c_str() : "(end)", i < got.size() ? got[i].op.c_str() : "(end)", at);
        failures++;
        return;
    }
    size_t bytes = 0;
    for (const Call& c : got) bytes += c.bytes.size();
    std::printf("PASS %s: %zu calls, %zu argument bytes identical\n", what, got.size(), bytes);
}

static double now_ns() { return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- bench: a typical draw (~4 calls: the descriptor delta, push constants, streams, the draw) and a cold one (the whole state)
static void draw_typical(VkCommandBuffer cmd, VkPipelineLayout layout, int d) {
    static thread_local std::vector<VkWriteDescriptorSet> changed;
    changed.clear();
    changed.push_back(dw.writes[0]), changed.push_back(dw.writes[3]), changed.push_back(dw.writes[4]);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, changed.data());
    uint8_t data[kPushSize];
    std::memset(data, d, sizeof data);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, kPushSize, data);
    VkBuffer vb[16];
    VkDeviceSize vo[16], vs[16], vst[16];
    for (int i = 0; i < 16; i++) vb[i] = H<VkBuffer>(0x9000 + i), vo[i] = i + d, vs[i] = 4096, vst[i] = 16;
    vkCmdBindVertexBuffers2(cmd, 0, 16, vb, vo, vs, vst);
    vkCmdDrawIndexed(cmd, 36, 1, 0, d & 7, 0);
}
static void draw_cold(VkCommandBuffer cmd, VkPipelineLayout layout, int d) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, H<VkPipeline>(0x800 + (d & 3)));
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, (uint32_t)dw.writes.size(), dw.writes.data());
    DynState s{};
    s.vp = {0, 0, 1280, 720, 0, 1}, s.sc = {{0, 0}, {1280, 720}}, s.en[0] = d & 1, s.wm[0] = 0xF;
    vkCmdSetViewportWithCount(cmd, 1, &s.vp);
    vkCmdSetScissorWithCount(cmd, 1, &s.sc);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);
    vkCmdSetDepthWriteEnable(cmd, d & 1);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_GREATER);
    vkCmdSetColorBlendEnableEXT(cmd, 0, 1, s.en);
    vkCmdSetColorBlendEquationEXT(cmd, 0, 1, s.eq);
    vkCmdSetColorWriteMaskEXT(cmd, 0, 1, s.wm);
    vkCmdBindIndexBuffer(cmd, H<VkBuffer>(0x9F00), 0, VK_INDEX_TYPE_UINT16);
    draw_typical(cmd, layout, d);
}
static void draw_typical_direct(VkCommandBuffer cmd, VkPipelineLayout layout, int d) {   // (no wrapper: the driver's functions, as before)
    static thread_local std::vector<VkWriteDescriptorSet> changed;
    changed.clear();
    changed.push_back(dw.writes[0]), changed.push_back(dw.writes[3]), changed.push_back(dw.writes[4]);
    (::vkx::vkCmdPushDescriptorSetKHR)(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, changed.data());
    uint8_t data[kPushSize];
    std::memset(data, d, sizeof data);
    (::vkx::vkCmdPushConstants)(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, kPushSize, data);
    VkBuffer vb[16];
    VkDeviceSize vo[16], vs[16], vst[16];
    for (int i = 0; i < 16; i++) vb[i] = H<VkBuffer>(0x9000 + i), vo[i] = i + d, vs[i] = 4096, vst[i] = 16;
    (::vkx::vkCmdBindVertexBuffers2)(cmd, 0, 16, vb, vo, vs, vst);
    (::vkx::vkCmdDrawIndexed)(cmd, 36, 1, 0, d & 7, 0);
}
static int bench(int n) {
    g_quiet = true;
    build_graphics(0);
    const VkCommandBuffer cmd = H<VkCommandBuffer>(0x100);
    const VkPipelineLayout layout = H<VkPipelineLayout>(0x300);
    struct Shape {
        const char* name;
        void (*draw)(VkCommandBuffer, VkPipelineLayout, int);
        void (*direct)(VkCommandBuffer, VkPipelineLayout, int);
        int calls;
    } shapes[] = {{"typical draw (push descriptors x3 writes, push constants 96 B, vertex buffers x16, draw)", draw_typical,
                   draw_typical_direct, 4},
                  {"cold draw (+ pipeline, 9 writes, 9 dynamic states, index buffer)", draw_cold, nullptr, 16}};
    rec::Arena& a = rec::t_arena;
    for (const Shape& sh : shapes) {
        // (modes: 0 direct (no wrapper), 1 off (the wrapper's branch), 2 sync, 3 deferred: encode and drain timed apart)
        double med[5] = {};
        double bytes_per_packet = 0;
        for (int m = 0; m < 4; m++) {
            if (m == 0 && !sh.direct) continue;
            std::vector<double> reps[2];
            for (int rep = 0; rep < 5; rep++) {
                rec::set_mode(m <= 1 ? rec::OFF : m == 2 ? rec::SYNC : rec::DEFER);
                const uint64_t p0 = a.packets, b0 = a.bytes;
                double enc = 0, drn = 0;
                for (int i = 0; i < n; i += 256) {   // (deferred: drained every 256 draws, about a recording's draws)
                    const double t0 = now_ns();
                    for (int j = 0; j < 256; j++) (m == 0 ? sh.direct : sh.draw)(cmd, layout, i + j);
                    const double t1 = now_ns();
                    rec::drain();
                    enc += t1 - t0, drn += now_ns() - t1;
                }
                reps[0].push_back(enc / n), reps[1].push_back(drn / n);
                if (m == 3) bytes_per_packet = (double)(a.bytes - b0) / (double)(a.packets - p0);
            }
            for (auto& r : reps) std::sort(r.begin(), r.end());
            med[m] = reps[0][2];
            if (m == 3) med[4] = reps[1][2];
        }
        std::printf("BENCH %s, %d calls: ", sh.name, sh.calls);
        if (sh.direct) std::printf("direct %.1f ns, ", med[0]);
        std::printf("off %.1f ns, sync %.1f ns (+%.1f over off, +%.1f a call), deferred encode %.1f ns + drain %.1f ns; "
                    "%.0f bytes a packet (median of 5, %d draws)\n",
                    med[1], med[2], med[2] - med[1], (med[2] - med[1]) / sh.calls, med[3], med[4], bytes_per_packet, n);
    }
    rec::set_mode(rec::OFF);
    std::printf("BENCH %llu fake calls, arena %zu chunk(s)\n", (unsigned long long)g_calls, a.chunks.size());
    g_quiet = false;
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(0x0002 | 0x0001);   // (SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS)
#endif
    // (the switch as the renderer reads it: first use, from the environment)
    std::printf("env mode %d\n", rec::mode());
    const int env_mode = rec::mode();
    if (argc > 2 && !std::strcmp(argv[1], "--bench")) return bench(std::atoi(argv[2]));

    const std::vector<Call> ref = run(frame, rec::OFF, false);
    same("direct, caller memory reused after each call", ref, run(frame, rec::OFF, true));
    if (env_mode != rec::OFF) same("RX_VK_RECORDER from the environment, caller memory reused", ref, run(frame, env_mode, true));
    rec::t_arena.packets = 0;
    same("sync (packed, run at once), caller memory reused", ref, run(frame, rec::SYNC, true));
    if (rec::t_arena.packets != ref.size()) std::printf("FAIL sync: %llu packets for %zu calls\n", (unsigned long long)rec::t_arena.packets, ref.size()), failures++;
    if (rec::t_arena.ci || rec::t_arena.used) std::printf("FAIL sync: the arena not rewound\n"), failures++;
    same("deferred (packed, run at the drain), caller memory poisoned in between", ref, run(frame, rec::DEFER, true));
    if (rec::t_arena.fallbacks) std::printf("FAIL: %llu calls not packed\n", (unsigned long long)rec::t_arena.fallbacks), failures++;

    const std::vector<Call> fref = run(fallbacks, rec::OFF, false);
    same("fallback order (pNext chain, inline uniform block), deferred", fref, run(fallbacks, rec::DEFER, false));
    if (rec::t_arena.fallbacks != 2) std::printf("FAIL: %llu fallbacks, 2 expected\n", (unsigned long long)rec::t_arena.fallbacks), failures++;

    std::set<std::string> seen;
    for (const Call& c : ref) seen.insert(c.op);
    int missing = 0;
    for (uint32_t op = 0; op < rec::OP_COUNT; op++)
        if (!seen.count(rec::op_name(op))) std::printf("NOT COVERED %s\n", rec::op_name(op)), missing++;
    if (missing) failures++;
    else std::printf("PASS coverage: all %d packed calls made by the frame\n", (int)rec::OP_COUNT);
    std::printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
