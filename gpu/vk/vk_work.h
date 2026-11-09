// (hookshot) Every Vulkan command that does GPU work, or orders it, counts in work_epoch. full_barrier (vk_texture.cpp) leaves out a
// barrier recorded right after another one on the same command buffer with nothing in between: the second orders nothing the first
// did not. Include this header AFTER the file's other includes: it turns the calls below into a count and the call (a header
// included after it that names one of them with a '(' after it would be changed too).
#pragma once

#include <atomic>

#include "vk_loader.h"
#include "vk_recorder.h"   // (RX_VK_RECORDER: each macro below calls its packer, which calls the driver directly when the recorder is off)

namespace mkwvk {
extern std::atomic<uint64_t> work_epoch;   // (vk_texture.cpp)
inline void note_work() {
    // (an add, not a load and a store: a thread preempted between the two would store a stale count over another thread's, which could
    // make the count equal the one a barrier was recorded at with work recorded since, and the next barrier is left out for it)
    work_epoch.fetch_add(1, std::memory_order_relaxed);
}
} // namespace mkwvk

#define vkCmdPipelineBarrier2(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdPipelineBarrier2(__VA_ARGS__))
#define vkCmdBeginRendering(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdBeginRendering(__VA_ARGS__))
#define vkCmdEndRendering(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdEndRendering(__VA_ARGS__))
#define vkCmdDraw(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDraw(__VA_ARGS__))
#define vkCmdDrawIndexed(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndexed(__VA_ARGS__))
#define vkCmdDrawIndirect(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndirect(__VA_ARGS__))
#define vkCmdDrawIndexedIndirect(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndexedIndirect(__VA_ARGS__))
#define vkCmdDrawIndirectCount(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndirectCount(__VA_ARGS__))
#define vkCmdDrawIndexedIndirectCount(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawIndexedIndirectCount(__VA_ARGS__))
#define vkCmdDrawMeshTasksEXT(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawMeshTasksEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectEXT(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawMeshTasksIndirectEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectCountEXT(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDrawMeshTasksIndirectCountEXT(__VA_ARGS__))
#define vkCmdDispatch(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDispatch(__VA_ARGS__))
#define vkCmdDispatchIndirect(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdDispatchIndirect(__VA_ARGS__))
#define vkCmdCopyBuffer(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdCopyBuffer(__VA_ARGS__))
#define vkCmdCopyBufferToImage(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdCopyBufferToImage(__VA_ARGS__))
#define vkCmdCopyImageToBuffer(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdCopyImageToBuffer(__VA_ARGS__))
#define vkCmdCopyImage(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdCopyImage(__VA_ARGS__))
#define vkCmdBlitImage(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdBlitImage(__VA_ARGS__))
#define vkCmdResolveImage(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdResolveImage(__VA_ARGS__))
#define vkCmdClearColorImage(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdClearColorImage(__VA_ARGS__))
#define vkCmdClearDepthStencilImage(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdClearDepthStencilImage(__VA_ARGS__))
#define vkCmdClearAttachments(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdClearAttachments(__VA_ARGS__))
#define vkCmdFillBuffer(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdFillBuffer(__VA_ARGS__))
#define vkCmdUpdateBuffer(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdUpdateBuffer(__VA_ARGS__))
#define vkCmdBeginQuery(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdBeginQuery(__VA_ARGS__))
#define vkCmdEndQuery(...) (::mkwvk::note_work(), ::mkwvk::rec::rx_vkCmdEndQuery(__VA_ARGS__))

// (RX_VK_RECORDER) the calls that set state, bind or mark: not work (no count), but in the command buffer in order with the work, so
// through the recorder too
#define vkCmdBindPipeline(...) (::mkwvk::rec::rx_vkCmdBindPipeline(__VA_ARGS__))
#define vkCmdBindDescriptorSets(...) (::mkwvk::rec::rx_vkCmdBindDescriptorSets(__VA_ARGS__))
#define vkCmdBindIndexBuffer(...) (::mkwvk::rec::rx_vkCmdBindIndexBuffer(__VA_ARGS__))
#define vkCmdBindVertexBuffers2(...) (::mkwvk::rec::rx_vkCmdBindVertexBuffers2(__VA_ARGS__))
#define vkCmdPushConstants(...) (::mkwvk::rec::rx_vkCmdPushConstants(__VA_ARGS__))
#define vkCmdPushDescriptorSetKHR(...) (::mkwvk::rec::rx_vkCmdPushDescriptorSetKHR(__VA_ARGS__))
#define vkCmdSetViewport(...) (::mkwvk::rec::rx_vkCmdSetViewport(__VA_ARGS__))
#define vkCmdSetScissor(...) (::mkwvk::rec::rx_vkCmdSetScissor(__VA_ARGS__))
#define vkCmdSetViewportWithCount(...) (::mkwvk::rec::rx_vkCmdSetViewportWithCount(__VA_ARGS__))
#define vkCmdSetScissorWithCount(...) (::mkwvk::rec::rx_vkCmdSetScissorWithCount(__VA_ARGS__))
#define vkCmdSetBlendConstants(...) (::mkwvk::rec::rx_vkCmdSetBlendConstants(__VA_ARGS__))
#define vkCmdSetStencilReference(...) (::mkwvk::rec::rx_vkCmdSetStencilReference(__VA_ARGS__))
#define vkCmdSetStencilCompareMask(...) (::mkwvk::rec::rx_vkCmdSetStencilCompareMask(__VA_ARGS__))
#define vkCmdSetStencilWriteMask(...) (::mkwvk::rec::rx_vkCmdSetStencilWriteMask(__VA_ARGS__))
#define vkCmdSetStencilOp(...) (::mkwvk::rec::rx_vkCmdSetStencilOp(__VA_ARGS__))
#define vkCmdSetDepthBias(...) (::mkwvk::rec::rx_vkCmdSetDepthBias(__VA_ARGS__))
#define vkCmdSetDepthBiasEnable(...) (::mkwvk::rec::rx_vkCmdSetDepthBiasEnable(__VA_ARGS__))
#define vkCmdSetLineWidth(...) (::mkwvk::rec::rx_vkCmdSetLineWidth(__VA_ARGS__))
#define vkCmdSetDepthBoundsTestEnable(...) (::mkwvk::rec::rx_vkCmdSetDepthBoundsTestEnable(__VA_ARGS__))
#define vkCmdSetCullMode(...) (::mkwvk::rec::rx_vkCmdSetCullMode(__VA_ARGS__))
#define vkCmdSetFrontFace(...) (::mkwvk::rec::rx_vkCmdSetFrontFace(__VA_ARGS__))
#define vkCmdSetPrimitiveTopology(...) (::mkwvk::rec::rx_vkCmdSetPrimitiveTopology(__VA_ARGS__))
#define vkCmdSetPrimitiveRestartEnable(...) (::mkwvk::rec::rx_vkCmdSetPrimitiveRestartEnable(__VA_ARGS__))
#define vkCmdSetDepthTestEnable(...) (::mkwvk::rec::rx_vkCmdSetDepthTestEnable(__VA_ARGS__))
#define vkCmdSetDepthWriteEnable(...) (::mkwvk::rec::rx_vkCmdSetDepthWriteEnable(__VA_ARGS__))
#define vkCmdSetDepthCompareOp(...) (::mkwvk::rec::rx_vkCmdSetDepthCompareOp(__VA_ARGS__))
#define vkCmdSetStencilTestEnable(...) (::mkwvk::rec::rx_vkCmdSetStencilTestEnable(__VA_ARGS__))
#define vkCmdSetRasterizerDiscardEnable(...) (::mkwvk::rec::rx_vkCmdSetRasterizerDiscardEnable(__VA_ARGS__))
#define vkCmdSetPolygonModeEXT(...) (::mkwvk::rec::rx_vkCmdSetPolygonModeEXT(__VA_ARGS__))
#define vkCmdSetDepthClampEnableEXT(...) (::mkwvk::rec::rx_vkCmdSetDepthClampEnableEXT(__VA_ARGS__))
#define vkCmdSetAlphaToCoverageEnableEXT(...) (::mkwvk::rec::rx_vkCmdSetAlphaToCoverageEnableEXT(__VA_ARGS__))
#define vkCmdSetColorBlendEnableEXT(...) (::mkwvk::rec::rx_vkCmdSetColorBlendEnableEXT(__VA_ARGS__))
#define vkCmdSetColorBlendEquationEXT(...) (::mkwvk::rec::rx_vkCmdSetColorBlendEquationEXT(__VA_ARGS__))
#define vkCmdSetColorWriteMaskEXT(...) (::mkwvk::rec::rx_vkCmdSetColorWriteMaskEXT(__VA_ARGS__))
#define vkCmdWriteBufferMarkerAMD(...) (::mkwvk::rec::rx_vkCmdWriteBufferMarkerAMD(__VA_ARGS__))
#define vkCmdSetCheckpointNV(...) (::mkwvk::rec::rx_vkCmdSetCheckpointNV(__VA_ARGS__))
#define vkCmdWriteTimestamp2(...) (::mkwvk::rec::rx_vkCmdWriteTimestamp2(__VA_ARGS__))
#define vkCmdResetQueryPool(...) (::mkwvk::rec::rx_vkCmdResetQueryPool(__VA_ARGS__))
