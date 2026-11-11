/* The widescreen picture's size, worked out the same way by host/hos_main.c (hos_wide_out, at the start) and by the Insert
   menu's Output and Render resolution rows (gpu/vk/vk_menu.cpp, the sizes they show): the Output resolution's height kept, the
   width at the screen's ratio (whole multiples of 8 pixels), then at most hos_wide_max_px() pixels with the shape kept.
   Header only, C and C++. */
#ifndef HOS_WIDE_SIZE_H
#define HOS_WIDE_SIZE_H

#include <math.h>

/* The screen's ratio as the picture takes it: 5:4 to 32:9 (narrower and wider screens are kept to those). */
static inline double hos_wide_clamp(double want) {
    return want > 32.0 / 9.0 ? 32.0 / 9.0 : want < 5.0 / 4.0 ? 5.0 / 4.0 : want;
}

/* The most pixels a widescreen picture takes: 3840 x 2160's. The game's render targets come from a pool sized for its picture
   (host/nn_applet.c hos_rt_pool_size), and a 32:9 screen at the 4K output made a 7680 x 2160 one. Its pool at the output's size
   ran out as Kokiri Forest loaded (a null pointer, issue 1031); a pool for the whole picture, 1.4 GB, failed the game's own
   reservation at boot. 5400 x 1520 there, more than the 5120 x 1440 screens it was seen on. */
static inline double hos_wide_max_px(void) { return 3840.0 * 2160.0; }

/* The picture for an output height at ratio r (hos_wide_clamp's): *w x *h */
static inline void hos_wide_size(unsigned out_h, double r, double max_px, unsigned *w, unsigned *h) {
    unsigned oh = out_h, ow = (unsigned)(oh * r / 8 + 0.5) * 8;
    if ((double)ow * oh > max_px) {   /* (the shape kept) */
        oh = (unsigned)(sqrt(max_px / r) / 8) * 8;
        ow = (unsigned)(oh * r / 8) * 8;
    }
    *w = ow, *h = oh;
}

#endif
