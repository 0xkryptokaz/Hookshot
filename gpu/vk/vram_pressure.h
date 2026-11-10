/* How full VRAM really is, for the rules that give memory back near the driver's limit (vk_memory.cpp's write-back shadows).
   No Vulkan here. tests/test_vram_pressure.py */
#pragma once

#include <algorithm>
#include <cstdint>

namespace mkwvk::vram_pressure {

/// What the process really holds: the driver's count less the free space inside VMA's blocks. A destroyed image's or
/// buffer's memory stays in its block, and the driver counts the block whole until VMA frees it
inline uint64_t allocated(uint64_t used, uint64_t block_free) {
    return used - std::min(used, block_free);
}

/// Whether the write-back's shadows are given back: what is really allocated past 95% of what the driver allows, or
/// the driver's own count past 99%. The count alone passes 95% whenever the texture cache is full, with gigabytes of
/// it free space inside the blocks that no eviction gives back
inline bool shadows_go(uint64_t used, uint64_t allowed, uint64_t block_free) {
    if (!allowed) return false;
    return allocated(used, block_free) > allowed / 100 * 95 || used > allowed / 100 * 99;
}

/// Whether a shadow of n bytes may be made: what is really allocated stays under 93% with it (the texture budget gives
/// way from 90%), and the driver's own count under 98%
inline bool shadow_fits(uint64_t n, uint64_t used, uint64_t allowed, uint64_t block_free) {
    if (!allowed) return true;
    return allocated(used, block_free) + n <= allowed / 100 * 93 && used + n <= allowed / 100 * 98;
}

} // namespace mkwvk::vram_pressure
