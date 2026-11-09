// (hookshot) Dear ImGui over the window's image (vk_imgui.cpp, third_party/imgui): the context, its Vulkan backend on the
// renderer's device, the fonts, and the textures. The Insert menu, the FPS counter and the start-up screen draw with it
// (vk_menu.cpp, vk_ui.h). Nothing here runs while nothing is shown: no ImGui frame, no render pass.
#pragma once

#include "vk_internal.h"

struct ImFont;
struct ImTextureData;

namespace mkwvk::ui {

struct Fonts {
    ImFont* body = nullptr;      // (rows, help, prompts)
    ImFont* strong = nullptr;    // (the selected row, values, buttons)
    ImFont* heading = nullptr;   // (tabs, titles, the wordmark)
};

/// A new ImGui frame for a w x h window image (the context made and the fonts loaded on the first call); false when ImGui
/// cannot draw here (no views of the window's images: the surface does not allow drawing into them)
bool begin(uint32_t w, uint32_t h);
const Fonts& fonts();
/// The start-up screen's logo (assets/hookshot.png), or null; its width over its height in *aspect
ImTextureData* logo(float* aspect);
/// The frame's drawing over the window's image `index` (TRANSFER_DST_OPTIMAL before and after): new glyphs and textures
/// copied in on cmd first (no wait on the queue), then one render pass
void record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h);

} // namespace mkwvk::ui
