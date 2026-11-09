/* (hookshot) NVIDIA DLSS's camera math, as plain functions: the jitter's sequence (host/rx_backend.c, in C) and the motion
   vectors' reprojection (gpu/vk/vk_dlss.cpp). No Vulkan, no NGX: tests/test_dlss_math.cpp checks them. */
#pragma once

#ifdef __cplusplus
#include <cmath>
#include <utility>
#endif

/* The radical inverse of index in base: the Halton sequence's index-th value, in [0, 1) */
static inline float dlss_halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    for (int i = index; i > 0; i /= base) {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
}

/* The frame's sub-pixel jitter, in pixels, each in [-0.5, 0.5): the Halton (2, 3) sequence over `phases` frames (from 1: its
   first value, 0, would put every phase's start on the pixel's corner) */
static inline void dlss_jitter_pixels(unsigned frame, int phases, float out[2]) {
    const int i = (int)(frame % (unsigned)(phases > 0 ? phases : 1)) + 1;
    out[0] = dlss_halton(i, 2) - 0.5f;
    out[1] = dlss_halton(i, 3) - 0.5f;
}

#ifdef __cplusplus
namespace mkwvk::dlss_math {

/// This frame's clip space to the frame before's: previous * inverse(current), both row major (clip = M * position); false
/// when current is singular or the result is not finite
inline bool reprojection(const float* current, const float* previous, float out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? current[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {   // (Gauss-Jordan with partial pivoting)
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        if (p != c)
            for (int k = 0; k < 8; k++) std::swap(a[p][k], a[c][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c || a[r][c] == 0.0) continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += previous[r * 4 + k] * a[k][c + 4];
            if (!std::isfinite(v)) return false;
            out[r * 4 + c] = (float)v;
        }
    return true;
}

/// A view-projection's jitter taken off (row major): a jitter moves clip x and y by jitter * w, so jitter times the w row
/// comes off the first two rows
inline void unjitter(float m[16], const float jitter[2]) {
    for (int c = 0; c < 4; c++) {
        m[0 * 4 + c] -= jitter[0] * m[3 * 4 + c];
        m[1 * 4 + c] -= jitter[1] * m[3 * 4 + c];
    }
}

/// A jitter in clip space as DLSS takes it: in render pixels, x right and y down (clip y is up)
inline void jitter_to_pixels(const float clip[2], unsigned width, unsigned height, float out[2]) {
    out[0] = clip[0] * 0.5f * (float)width;
    out[1] = -clip[1] * 0.5f * (float)height;
}

} // namespace mkwvk::dlss_math
#endif
