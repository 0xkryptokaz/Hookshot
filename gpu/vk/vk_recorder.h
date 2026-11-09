// (hookshot) RX_VK_RECORDER: the command buffer calls as packets, step 1 of a recorder thread (the "sync" phase).
//
// Every vkCmd* call vk_work.h routes here is copied into a packet: its value arguments, and every array and struct it points to (the
// descriptor writes and the image/buffer/texel infos they point to, push constant bytes, vertex buffer arrays, the rendering info and
// its attachments, the barriers, the copy regions, the dynamic state arrays), so a packet owns all it needs and no longer reads the
// caller's memory. A packet is run by one decoder (execute: the packet's thunk calls the real ::vkx function with the copies).
//
//   RX_VK_RECORDER unset / anything else: off, the calls go straight to the driver as before (one relaxed load and a branch a call)
//   RX_VK_RECORDER=sync: each call packed, then run at once on the same thread, in the same order: the same calls with the same
//                        values, made from the packet's copies (the proof that the copies are complete and exact)
//   (DEFER, tests only, set_mode): the packets held in the arena and run at drain(), in order; the caller may reuse or poison its
//                        memory in between, as it will once a thread runs them: tests/test_recorder_sync.py
//
// A packet: [Packet header: thunk, next, op, size][the call's closure: its value arguments and the arena pointers of its copies],
// preceded in the arena by the copied arrays. The arena is the recording thread's own (thread_local: the parallel replay's workers each
// have theirs) and in sync mode is rewound after each packet runs, so it stays at one packet's size. A call whose structs carry a
// pNext chain (none does today) or a descriptor type the packer does not know is not packed: what is held is drained, then the call
// is made directly (counted in Arena::fallbacks).
//
// Include through vk_work.h, not by itself: the names below are the wrappers its macros call.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

#include "vk_loader.h"

// The calls the recorder carries. V: value arguments only (handles, numbers, enums), one generic packer; A: arrays or structs behind
// pointers, a packer of their own below. vk_work.h has a macro for each (the test checks the two lists agree).
#define RX_REC_CALLS(V, A)                                                                                                         \
    A(vkCmdPipelineBarrier2) A(vkCmdBeginRendering) V(vkCmdEndRendering)                                                           \
    V(vkCmdDraw) V(vkCmdDrawIndexed) V(vkCmdDrawIndirect) V(vkCmdDrawIndexedIndirect) V(vkCmdDrawIndirectCount)                     \
    V(vkCmdDrawIndexedIndirectCount) V(vkCmdDrawMeshTasksEXT) V(vkCmdDrawMeshTasksIndirectEXT) V(vkCmdDrawMeshTasksIndirectCountEXT) \
    V(vkCmdDispatch) V(vkCmdDispatchIndirect)                                                                                      \
    A(vkCmdCopyBuffer) A(vkCmdCopyBufferToImage) A(vkCmdCopyImageToBuffer) A(vkCmdCopyImage) A(vkCmdBlitImage) A(vkCmdResolveImage) \
    A(vkCmdClearColorImage) A(vkCmdClearDepthStencilImage) A(vkCmdClearAttachments) V(vkCmdFillBuffer) A(vkCmdUpdateBuffer)          \
    V(vkCmdBeginQuery) V(vkCmdEndQuery)                                                                                            \
    V(vkCmdBindPipeline) A(vkCmdBindDescriptorSets) V(vkCmdBindIndexBuffer) A(vkCmdBindVertexBuffers2) A(vkCmdPushConstants)        \
    A(vkCmdPushDescriptorSetKHR)                                                                                                   \
    A(vkCmdSetViewport) A(vkCmdSetScissor) A(vkCmdSetViewportWithCount) A(vkCmdSetScissorWithCount) A(vkCmdSetBlendConstants)       \
    V(vkCmdSetStencilReference) V(vkCmdSetStencilCompareMask) V(vkCmdSetStencilWriteMask) V(vkCmdSetStencilOp) V(vkCmdSetDepthBias) \
    V(vkCmdSetDepthBiasEnable) V(vkCmdSetLineWidth) V(vkCmdSetDepthBoundsTestEnable) V(vkCmdSetCullMode) V(vkCmdSetFrontFace)       \
    V(vkCmdSetPrimitiveTopology) V(vkCmdSetPrimitiveRestartEnable) V(vkCmdSetDepthTestEnable) V(vkCmdSetDepthWriteEnable)           \
    V(vkCmdSetDepthCompareOp) V(vkCmdSetStencilTestEnable) V(vkCmdSetRasterizerDiscardEnable) V(vkCmdSetPolygonModeEXT)             \
    V(vkCmdSetDepthClampEnableEXT) V(vkCmdSetAlphaToCoverageEnableEXT)                                                             \
    A(vkCmdSetColorBlendEnableEXT) A(vkCmdSetColorBlendEquationEXT) A(vkCmdSetColorWriteMaskEXT)                                   \
    V(vkCmdWriteBufferMarkerAMD) A(vkCmdSetCheckpointNV) V(vkCmdWriteTimestamp2) V(vkCmdResetQueryPool)

namespace mkwvk::rec {

enum Op : uint16_t {
#define RX_REC_OP(f) OP_##f,
    RX_REC_CALLS(RX_REC_OP, RX_REC_OP)
#undef RX_REC_OP
    OP_COUNT
};
inline const char* op_name(uint32_t op) {
    static const char* const names[] = {
#define RX_REC_NAME(f) #f,
        RX_REC_CALLS(RX_REC_NAME, RX_REC_NAME)
#undef RX_REC_NAME
    };
    return op < OP_COUNT ? names[op] : "?";
}

enum Mode : int { OFF = 0, SYNC = 1, DEFER = 2 };
inline std::atomic<int> g_mode{-1};
inline int mode_init() {
    const char* e = getenv("RX_VK_RECORDER");
    const int m = e && !strcmp(e, "sync") ? SYNC : OFF;
    g_mode.store(m, std::memory_order_relaxed);
    return m;
}
inline int mode() {
    const int m = g_mode.load(std::memory_order_relaxed);
    return m >= 0 ? m : mode_init();
}
inline bool on() { return mode() != OFF; }

struct alignas(16) Packet {
    void (*run)(const Packet*);   // the decoder for this packet's call
    Packet* next;                 // (DEFER) the next held packet
    uint32_t op;                  // Op, for diagnostics
    uint32_t size;                // bytes of the closure after the header
};

struct Arena {
    static constexpr size_t kChunk = 1u << 20;
    struct Chunk {
        uint8_t* p;
        size_t cap;
    };
    struct Mark {
        size_t ci, used;
    };
    std::vector<Chunk> chunks;
    size_t ci = 0, used = 0;                  // the chunk being filled and its bytes used
    Packet* head = nullptr;                   // (DEFER) the packets not run yet, in order
    Packet** tail = &head;
    uint64_t packets = 0, bytes = 0, fallbacks = 0;

    void* alloc(size_t n, size_t align) {
        bytes += n;
        for (;;) {
            if (ci < chunks.size()) {
                const size_t at = (used + align - 1) & ~(align - 1);
                if (at + n <= chunks[ci].cap) {
                    used = at + n;
                    return chunks[ci].p + at;
                }
                if (ci + 1 < chunks.size() || used) {   // (the next chunk, or a new one; a chunk too small even empty is passed)
                    ci++, used = 0;
                    continue;
                }
            }
            const size_t cap = n + align > kChunk ? n + align : kChunk;
            chunks.push_back({static_cast<uint8_t*>(::operator new(cap, std::align_val_t(64))), cap});
            ci = chunks.size() - 1, used = 0;
        }
    }
    Mark mark() const { return {ci, used}; }
    void rewind(Mark m) { ci = m.ci, used = m.used; }
    ~Arena() {
        for (Chunk& c : chunks) ::operator delete(c.p, std::align_val_t(64));
    }
};
inline thread_local Arena t_arena;

/// The decoder: runs one packet's call with its copies
inline void execute(const Packet* p) { p->run(p); }

/// (DEFER) runs every held packet, in order, and empties the arena
inline void drain() {
    Arena& a = t_arena;
    for (const Packet* p = a.head; p;) {
        const Packet* next = p->next;
        execute(p);
        p = next;
    }
    a.head = nullptr, a.tail = &a.head;
    a.ci = 0, a.used = 0;
}

/// (for tests) DEFER or back; what is held is run first
inline void set_mode(int m) {
    drain();
    g_mode.store(m, std::memory_order_relaxed);
}

/// A call that is not packed (a pNext chain, an unknown descriptor type): what is held goes first, then the call itself
template <class F>
inline void fallback(const F& direct) {
    drain();
    t_arena.fallbacks++;
    direct();
}

template <class F>
void thunk(const Packet* p) {
    (*reinterpret_cast<const F*>(p + 1))();
}

/// One packet being encoded: its copies, then emit() with the closure that makes the call. Sync: run at emit, the arena rewound when
/// the encoder goes out of scope
struct Enc {
    Arena& a = t_arena;
    const Arena::Mark m = a.mark();
    const int md = mode();

    template <class T>
    T* alloc(size_t n) {
        return static_cast<T*>(a.alloc(sizeof(T) * n, alignof(T) < 8 ? 8 : alignof(T)));
    }
    template <class T>
    const T* dup(const T* p, size_t n) {
        if (!p || !n) return nullptr;
        T* d = alloc<T>(n);
        std::memcpy(static_cast<void*>(d), p, sizeof(T) * n);
        return d;
    }
    const void* dup_bytes(const void* p, size_t n) { return dup(static_cast<const uint8_t*>(p), n); }

    template <class F>
    void emit(Op op, const F& f) {
        static_assert(std::is_trivially_copyable_v<F> && std::is_trivially_destructible_v<F>, "a packet holds values only");
        static_assert(alignof(F) <= alignof(Packet), "a closure aligned beyond the header");
        Packet* p = static_cast<Packet*>(a.alloc(sizeof(Packet) + sizeof(F), alignof(Packet)));
        p->run = &thunk<F>, p->next = nullptr, p->op = op, p->size = (uint32_t)sizeof(F);
        new (static_cast<void*>(p + 1)) F(f);
        a.packets++;
        if (md == SYNC) {
            execute(p);
        } else {
            *a.tail = p;
            a.tail = &p->next;
        }
    }
    ~Enc() {
        if (md == SYNC) a.rewind(m);
    }
};

// ---- The wrapper each vk_work.h macro calls: the driver's function when the recorder is off (the branch is all the off path costs: the
// packer is a function of its own, out of line, so the caller's code stays as it was), else the call's packer
#if defined(__GNUC__) || defined(__clang__)
#define RX_REC_PACKER __attribute__((noinline)) inline
#else
#define RX_REC_PACKER __declspec(noinline) inline
#endif
template <auto& F, auto& PK, class Sig = std::remove_cv_t<std::remove_reference_t<decltype(F)>>>
struct Route;
template <auto& F, auto& PK, class... P>
struct Route<F, PK, void(VKAPI_PTR*)(P...)> {
    static void call(P... p) {
        if (mode() == OFF) [[likely]]
            return F(p...);
        PK(p...);
    }
};

// ---- V: value arguments only. The closure holds them as the call's own parameter types (converted at the wrapper, as the direct call
// converts them). A pointer to const data here would be a pointer kept past the call: refused at compile time (handles are pointers to
// non-const opaque types and pass)
template <auto& F, Op OP, class Sig = std::remove_cv_t<std::remove_reference_t<decltype(F)>>>
struct Value;
template <auto& F, Op OP, class... P>
struct Value<F, OP, void(VKAPI_PTR*)(P...)> {
    static_assert(((!std::is_pointer_v<P> || !std::is_const_v<std::remove_pointer_t<P>>) && ...),
                  "a call with a pointer to data needs a packer of its own");
    RX_REC_PACKER static void pack(P... p) {
        Enc e;
        e.emit(OP, [=] { F(p...); });
    }
};
#define RX_REC_VALUE(f) inline constexpr auto& rx_##f = Route<::vkx::f, Value<::vkx::f, OP_##f>::pack>::call;
#define RX_REC_SKIP(f)
RX_REC_CALLS(RX_REC_VALUE, RX_REC_SKIP)
#undef RX_REC_VALUE
#undef RX_REC_SKIP

// ---- A: the calls with arrays and structs behind pointers. Each copies what the driver reads, by the counts the driver reads by
template <class T>
inline bool chained(const T* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (p[i].pNext) return true;
    return false;
}

RX_REC_PACKER void pk_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    if (di->pNext || chained(di->pMemoryBarriers, di->memoryBarrierCount) ||
        chained(di->pBufferMemoryBarriers, di->bufferMemoryBarrierCount) ||
        chained(di->pImageMemoryBarriers, di->imageMemoryBarrierCount))
        return fallback([&] { ::vkx::vkCmdPipelineBarrier2(cb, di); });
    Enc e;
    VkDependencyInfo c = *di;
    c.pMemoryBarriers = e.dup(di->pMemoryBarriers, di->memoryBarrierCount);
    c.pBufferMemoryBarriers = e.dup(di->pBufferMemoryBarriers, di->bufferMemoryBarrierCount);
    c.pImageMemoryBarriers = e.dup(di->pImageMemoryBarriers, di->imageMemoryBarrierCount);
    e.emit(OP_vkCmdPipelineBarrier2, [=] { ::vkx::vkCmdPipelineBarrier2(cb, &c); });
}
inline constexpr auto& rx_vkCmdPipelineBarrier2 = Route<::vkx::vkCmdPipelineBarrier2, pk_vkCmdPipelineBarrier2>::call;

RX_REC_PACKER void pk_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    if (ri->pNext || chained(ri->pColorAttachments, ri->colorAttachmentCount) || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) ||
        (ri->pStencilAttachment && ri->pStencilAttachment->pNext))
        return fallback([&] { ::vkx::vkCmdBeginRendering(cb, ri); });
    Enc e;
    VkRenderingInfo c = *ri;
    c.pColorAttachments = e.dup(ri->pColorAttachments, ri->colorAttachmentCount);
    c.pDepthAttachment = e.dup(ri->pDepthAttachment, 1);
    c.pStencilAttachment = e.dup(ri->pStencilAttachment, 1);
    e.emit(OP_vkCmdBeginRendering, [=] { ::vkx::vkCmdBeginRendering(cb, &c); });
}
inline constexpr auto& rx_vkCmdBeginRendering = Route<::vkx::vkCmdBeginRendering, pk_vkCmdBeginRendering>::call;

RX_REC_PACKER void pk_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* r) {
    Enc e;
    const VkBufferCopy* c = e.dup(r, n);
    e.emit(OP_vkCmdCopyBuffer, [=] { ::vkx::vkCmdCopyBuffer(cb, src, dst, n, c); });
}
inline constexpr auto& rx_vkCmdCopyBuffer = Route<::vkx::vkCmdCopyBuffer, pk_vkCmdCopyBuffer>::call;

RX_REC_PACKER void pk_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t n,
                                      const VkBufferImageCopy* r) {
    Enc e;
    const VkBufferImageCopy* c = e.dup(r, n);
    e.emit(OP_vkCmdCopyBufferToImage, [=] { ::vkx::vkCmdCopyBufferToImage(cb, src, dst, layout, n, c); });
}
inline constexpr auto& rx_vkCmdCopyBufferToImage = Route<::vkx::vkCmdCopyBufferToImage, pk_vkCmdCopyBufferToImage>::call;

RX_REC_PACKER void pk_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout, VkBuffer dst, uint32_t n,
                                      const VkBufferImageCopy* r) {
    Enc e;
    const VkBufferImageCopy* c = e.dup(r, n);
    e.emit(OP_vkCmdCopyImageToBuffer, [=] { ::vkx::vkCmdCopyImageToBuffer(cb, src, layout, dst, n, c); });
}
inline constexpr auto& rx_vkCmdCopyImageToBuffer = Route<::vkx::vkCmdCopyImageToBuffer, pk_vkCmdCopyImageToBuffer>::call;

RX_REC_PACKER void pk_vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                              const VkImageCopy* r) {
    Enc e;
    const VkImageCopy* c = e.dup(r, n);
    e.emit(OP_vkCmdCopyImage, [=] { ::vkx::vkCmdCopyImage(cb, src, sl, dst, dl, n, c); });
}
inline constexpr auto& rx_vkCmdCopyImage = Route<::vkx::vkCmdCopyImage, pk_vkCmdCopyImage>::call;

RX_REC_PACKER void pk_vkCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                              const VkImageBlit* r, VkFilter filter) {
    Enc e;
    const VkImageBlit* c = e.dup(r, n);
    e.emit(OP_vkCmdBlitImage, [=] { ::vkx::vkCmdBlitImage(cb, src, sl, dst, dl, n, c, filter); });
}
inline constexpr auto& rx_vkCmdBlitImage = Route<::vkx::vkCmdBlitImage, pk_vkCmdBlitImage>::call;

RX_REC_PACKER void pk_vkCmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                                 const VkImageResolve* r) {
    Enc e;
    const VkImageResolve* c = e.dup(r, n);
    e.emit(OP_vkCmdResolveImage, [=] { ::vkx::vkCmdResolveImage(cb, src, sl, dst, dl, n, c); });
}
inline constexpr auto& rx_vkCmdResolveImage = Route<::vkx::vkCmdResolveImage, pk_vkCmdResolveImage>::call;

RX_REC_PACKER void pk_vkCmdClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout, const VkClearColorValue* color, uint32_t n,
                                    const VkImageSubresourceRange* ranges) {
    Enc e;
    const VkClearColorValue* col = e.dup(color, 1);
    const VkImageSubresourceRange* c = e.dup(ranges, n);
    e.emit(OP_vkCmdClearColorImage, [=] { ::vkx::vkCmdClearColorImage(cb, image, layout, col, n, c); });
}
inline constexpr auto& rx_vkCmdClearColorImage = Route<::vkx::vkCmdClearColorImage, pk_vkCmdClearColorImage>::call;

RX_REC_PACKER void pk_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout, const VkClearDepthStencilValue* ds,
                                           uint32_t n, const VkImageSubresourceRange* ranges) {
    Enc e;
    const VkClearDepthStencilValue* v = e.dup(ds, 1);
    const VkImageSubresourceRange* c = e.dup(ranges, n);
    e.emit(OP_vkCmdClearDepthStencilImage, [=] { ::vkx::vkCmdClearDepthStencilImage(cb, image, layout, v, n, c); });
}
inline constexpr auto& rx_vkCmdClearDepthStencilImage = Route<::vkx::vkCmdClearDepthStencilImage, pk_vkCmdClearDepthStencilImage>::call;

RX_REC_PACKER void pk_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* att, uint32_t nr, const VkClearRect* rects) {
    Enc e;
    const VkClearAttachment* a = e.dup(att, na);
    const VkClearRect* r = e.dup(rects, nr);
    e.emit(OP_vkCmdClearAttachments, [=] { ::vkx::vkCmdClearAttachments(cb, na, a, nr, r); });
}
inline constexpr auto& rx_vkCmdClearAttachments = Route<::vkx::vkCmdClearAttachments, pk_vkCmdClearAttachments>::call;

RX_REC_PACKER void pk_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void* data) {
    Enc e;
    const void* d = e.dup_bytes(data, (size_t)size);
    e.emit(OP_vkCmdUpdateBuffer, [=] { ::vkx::vkCmdUpdateBuffer(cb, dst, offset, size, d); });
}
inline constexpr auto& rx_vkCmdUpdateBuffer = Route<::vkx::vkCmdUpdateBuffer, pk_vkCmdUpdateBuffer>::call;

RX_REC_PACKER void pk_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                                       const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn) {
    Enc e;
    const VkDescriptorSet* s = e.dup(sets, n);
    const uint32_t* d = e.dup(dyn, nd);
    e.emit(OP_vkCmdBindDescriptorSets, [=] { ::vkx::vkCmdBindDescriptorSets(cb, bp, layout, first, n, s, nd, d); });
}
inline constexpr auto& rx_vkCmdBindDescriptorSets = Route<::vkx::vkCmdBindDescriptorSets, pk_vkCmdBindDescriptorSets>::call;

RX_REC_PACKER void pk_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* buffers, const VkDeviceSize* offsets,
                                       const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    Enc e;
    const VkBuffer* b = e.dup(buffers, n);
    const VkDeviceSize* o = e.dup(offsets, n);
    const VkDeviceSize* s = e.dup(sizes, n);     // (null stays null: the whole buffer)
    const VkDeviceSize* st = e.dup(strides, n);  // (null stays null: the pipeline's strides)
    e.emit(OP_vkCmdBindVertexBuffers2, [=] { ::vkx::vkCmdBindVertexBuffers2(cb, first, n, b, o, s, st); });
}
inline constexpr auto& rx_vkCmdBindVertexBuffers2 = Route<::vkx::vkCmdBindVertexBuffers2, pk_vkCmdBindVertexBuffers2>::call;

RX_REC_PACKER void pk_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                                  const void* values) {
    Enc e;
    const void* v = e.dup_bytes(values, size);
    e.emit(OP_vkCmdPushConstants, [=] { ::vkx::vkCmdPushConstants(cb, layout, stages, offset, size, v); });
}
inline constexpr auto& rx_vkCmdPushConstants = Route<::vkx::vkCmdPushConstants, pk_vkCmdPushConstants>::call;

/// Which of a write's three arrays the driver reads, by its type (the other two are ignored, and left null in the copy); 0: a type
/// the packer does not know (inline uniform blocks, acceleration structures: their data is in pNext)
inline int write_kind(VkDescriptorType t) {
    switch (t) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return 1;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return 2;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return 3;
    default: return 0;
    }
}

RX_REC_PACKER void pk_vkCmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set, uint32_t n,
                                         const VkWriteDescriptorSet* w) {
    for (uint32_t i = 0; i < n; i++)
        if (w[i].pNext || !write_kind(w[i].descriptorType))
            return fallback([&] { ::vkx::vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, w); });
    Enc e;
    VkWriteDescriptorSet* c = n ? e.alloc<VkWriteDescriptorSet>(n) : nullptr;
    for (uint32_t i = 0; i < n; i++) {
        c[i] = w[i];
        c[i].pImageInfo = nullptr, c[i].pBufferInfo = nullptr, c[i].pTexelBufferView = nullptr;
        switch (write_kind(w[i].descriptorType)) {
        case 1: c[i].pImageInfo = e.dup(w[i].pImageInfo, w[i].descriptorCount); break;
        case 2: c[i].pBufferInfo = e.dup(w[i].pBufferInfo, w[i].descriptorCount); break;
        default: c[i].pTexelBufferView = e.dup(w[i].pTexelBufferView, w[i].descriptorCount); break;
        }
    }
    e.emit(OP_vkCmdPushDescriptorSetKHR, [=] { ::vkx::vkCmdPushDescriptorSetKHR(cb, bp, layout, set, n, c); });
}
inline constexpr auto& rx_vkCmdPushDescriptorSetKHR = Route<::vkx::vkCmdPushDescriptorSetKHR, pk_vkCmdPushDescriptorSetKHR>::call;

RX_REC_PACKER void pk_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    Enc e;
    const VkViewport* c = e.dup(v, n);
    e.emit(OP_vkCmdSetViewport, [=] { ::vkx::vkCmdSetViewport(cb, first, n, c); });
}
inline constexpr auto& rx_vkCmdSetViewport = Route<::vkx::vkCmdSetViewport, pk_vkCmdSetViewport>::call;

RX_REC_PACKER void pk_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    Enc e;
    const VkRect2D* c = e.dup(r, n);
    e.emit(OP_vkCmdSetScissor, [=] { ::vkx::vkCmdSetScissor(cb, first, n, c); });
}
inline constexpr auto& rx_vkCmdSetScissor = Route<::vkx::vkCmdSetScissor, pk_vkCmdSetScissor>::call;

RX_REC_PACKER void pk_vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport* v) {
    Enc e;
    const VkViewport* c = e.dup(v, n);
    e.emit(OP_vkCmdSetViewportWithCount, [=] { ::vkx::vkCmdSetViewportWithCount(cb, n, c); });
}
inline constexpr auto& rx_vkCmdSetViewportWithCount = Route<::vkx::vkCmdSetViewportWithCount, pk_vkCmdSetViewportWithCount>::call;

RX_REC_PACKER void pk_vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t n, const VkRect2D* r) {
    Enc e;
    const VkRect2D* c = e.dup(r, n);
    e.emit(OP_vkCmdSetScissorWithCount, [=] { ::vkx::vkCmdSetScissorWithCount(cb, n, c); });
}
inline constexpr auto& rx_vkCmdSetScissorWithCount = Route<::vkx::vkCmdSetScissorWithCount, pk_vkCmdSetScissorWithCount>::call;

RX_REC_PACKER void pk_vkCmdSetBlendConstants(VkCommandBuffer cb, const float blend[4]) {
    Enc e;
    const float* c = e.dup(blend, 4);
    e.emit(OP_vkCmdSetBlendConstants, [=] { ::vkx::vkCmdSetBlendConstants(cb, c); });
}
inline constexpr auto& rx_vkCmdSetBlendConstants = Route<::vkx::vkCmdSetBlendConstants, pk_vkCmdSetBlendConstants>::call;

RX_REC_PACKER void pk_vkCmdSetColorBlendEnableEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBool32* en) {
    Enc e;
    const VkBool32* c = e.dup(en, n);
    e.emit(OP_vkCmdSetColorBlendEnableEXT, [=] { ::vkx::vkCmdSetColorBlendEnableEXT(cb, first, n, c); });
}
inline constexpr auto& rx_vkCmdSetColorBlendEnableEXT = Route<::vkx::vkCmdSetColorBlendEnableEXT, pk_vkCmdSetColorBlendEnableEXT>::call;

RX_REC_PACKER void pk_vkCmdSetColorBlendEquationEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorBlendEquationEXT* eq) {
    Enc e;
    const VkColorBlendEquationEXT* c = e.dup(eq, n);
    e.emit(OP_vkCmdSetColorBlendEquationEXT, [=] { ::vkx::vkCmdSetColorBlendEquationEXT(cb, first, n, c); });
}
inline constexpr auto& rx_vkCmdSetColorBlendEquationEXT = Route<::vkx::vkCmdSetColorBlendEquationEXT, pk_vkCmdSetColorBlendEquationEXT>::call;

RX_REC_PACKER void pk_vkCmdSetColorWriteMaskEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorComponentFlags* wm) {
    Enc e;
    const VkColorComponentFlags* c = e.dup(wm, n);
    e.emit(OP_vkCmdSetColorWriteMaskEXT, [=] { ::vkx::vkCmdSetColorWriteMaskEXT(cb, first, n, c); });
}
inline constexpr auto& rx_vkCmdSetColorWriteMaskEXT = Route<::vkx::vkCmdSetColorWriteMaskEXT, pk_vkCmdSetColorWriteMaskEXT>::call;

/// (the marker is an opaque value the driver hands back, never read through: kept as the pointer value)
RX_REC_PACKER void pk_vkCmdSetCheckpointNV(VkCommandBuffer cb, const void* marker) {
    Enc e;
    e.emit(OP_vkCmdSetCheckpointNV, [=] { ::vkx::vkCmdSetCheckpointNV(cb, marker); });
}
inline constexpr auto& rx_vkCmdSetCheckpointNV = Route<::vkx::vkCmdSetCheckpointNV, pk_vkCmdSetCheckpointNV>::call;

} // namespace mkwvk::rec
