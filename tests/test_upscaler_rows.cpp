#include "../gpu/vk/upscaler_rows.h"

#include <cstdio>
#include <cstring>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace ur = mkwvk::upscaler_rows;
    using ur::DLAA;
    using ur::DLSS;
    using ur::GAME;
    // (the values saved before keep their shares; the new ones are DLSS's ratios, each a twelfth of the output)
    CHECK("saved values keep their shares", ur::percent(0) == 0 && ur::percent(1) == 100 && ur::percent(2) == 75 &&
                                                ur::percent(3) == 67 && ur::percent(4) == 50);
    CHECK("Balanced and Ultra Performance", ur::percent(5) == 58 && ur::percent(6) == 33);
    CHECK("a value past the row is Auto", ur::percent(7) == 0 && ur::percent(-1) == 0);
    for (int v = 1; v < ur::kValues; v++) {
        const int k = (int)(ur::percent(v) * 12 / 100.0 + 0.5);
        CHECK("each share is a twelfth", k * 100 / 12 == ur::percent(v) || (k * 100 + 11) / 12 == ur::percent(v));
    }
    CHECK("DLSS's mode names", !std::strcmp(ur::dlss_mode_name(3), "Quality") && !std::strcmp(ur::dlss_mode_name(5), "Balanced") &&
                                   !std::strcmp(ur::dlss_mode_name(4), "Performance") &&
                                   !std::strcmp(ur::dlss_mode_name(6), "Ultra Performance"));
    CHECK("no mode name for Auto, 100% or 75%", !ur::dlss_mode_name(0) && !ur::dlss_mode_name(1) && !ur::dlss_mode_name(2));

    // the game's own: Auto, 100, 75, 67, 50, as before
    CHECK("game: Right", ur::step(GAME, 0, 1) == 1 && ur::step(GAME, 1, 1) == 2 && ur::step(GAME, 3, 1) == 4 && ur::step(GAME, 4, 1) == 0);
    CHECK("game: Left", ur::step(GAME, 0, -1) == 4 && ur::step(GAME, 1, -1) == 0 && ur::step(GAME, 4, -1) == 3);
    // DLSS: Auto, Quality 67, Balanced 58, Performance 50, Ultra Performance 33
    CHECK("dlss: Right", ur::step(DLSS, 0, 1) == 3 && ur::step(DLSS, 3, 1) == 5 && ur::step(DLSS, 5, 1) == 4 &&
                             ur::step(DLSS, 4, 1) == 6 && ur::step(DLSS, 6, 1) == 0);
    CHECK("dlss: Left", ur::step(DLSS, 0, -1) == 6 && ur::step(DLSS, 3, -1) == 0 && ur::step(DLSS, 6, -1) == 4);
    // a value that is not a step (saved with the other upscaler): its neighbours by size
    CHECK("dlss from 75%", ur::step(DLSS, 2, 1) == 3 && ur::step(DLSS, 2, -1) == 0);
    CHECK("dlss from 100%", ur::step(DLSS, 1, 1) == 3 && ur::step(DLSS, 1, -1) == 0);
    CHECK("game from 58%", ur::step(GAME, 5, 1) == 4 && ur::step(GAME, 5, -1) == 3);
    CHECK("game from 33%", ur::step(GAME, 6, 1) == 0 && ur::step(GAME, 6, -1) == 4);
    CHECK("DLAA does not step", ur::step(DLAA, 3, 1) == 3 && ur::step(DLAA, 0, -1) == 0);

    // another upscaler: the nearest step (the larger on a tie), Auto stays Auto
    CHECK("snap to DLSS", ur::snap(DLSS, 0) == 0 && ur::snap(DLSS, 1) == 3 && ur::snap(DLSS, 2) == 3 && ur::snap(DLSS, 3) == 3 &&
                              ur::snap(DLSS, 4) == 4);
    CHECK("snap to the game's", ur::snap(GAME, 5) == 4 && ur::snap(GAME, 6) == 4 && ur::snap(GAME, 3) == 3 && ur::snap(GAME, 1) == 1);
    CHECK("DLAA keeps the value", ur::snap(DLAA, 5) == 5 && ur::snap(DLAA, 1) == 1);
    CHECK("a value past the row snaps to Auto", ur::snap(GAME, 9) == 0 && ur::snap(DLSS, -3) == 0);

    // the Upscaler row: all three where DLSS runs, the game's own alone elsewhere
    CHECK("upscaler with DLSS", ur::step_upscaler(0, 1, true) == 1 && ur::step_upscaler(1, 1, true) == 2 &&
                                    ur::step_upscaler(2, 1, true) == 0 && ur::step_upscaler(0, -1, true) == 2);
    CHECK("upscaler without DLSS", ur::step_upscaler(0, 1, false) == 0 && ur::step_upscaler(2, -1, false) == 0);
    CHECK("upscaler from a bad value", ur::step_upscaler(7, 1, true) == 1);

    // the options file: the NVIDIA DLSS row's key loads as the Upscaler's
    CHECK("nvidia_dlss is dlss", ur::ini_alias("nvidia_dlss") && !std::strcmp(ur::ini_alias("nvidia_dlss"), "dlss"));
    CHECK("other keys have no alias", !ur::ini_alias("dlss") && !ur::ini_alias("nvidia_dlss2") && !ur::ini_alias("nvidia") &&
                                          !ur::ini_alias(""));
    std::puts("upscaler rows: all checks passed");
}
