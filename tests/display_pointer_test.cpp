#define _Static_assert static_assert
#include "host/display_pointer.h"
#include <atomic>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <initializer_list>
using std::floor;
using std::isfinite;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
[[maybe_unused]] static const auto ui_thread = std::this_thread::get_id();
[[maybe_unused]] static void ui_only() { CHECK(std::this_thread::get_id() == ui_thread); }

namespace win {
[[maybe_unused]] static HosPointer pointer = HOS_POINTER_INIT;
[[maybe_unused]] static int mouse_captured;
[[maybe_unused]] static HWND hwnd = (HWND)(uintptr_t)1;
[[maybe_unused]] static LONG ptr_x, ptr_y, ptr_w = 1280, ptr_h = 720, ptr_in = 1, ptr_pressed, ptr_wheel;
[[maybe_unused]] static int origin_x, origin_y, cursor_x = 100, cursor_y = 200, width = 1280, height = 720;
[[maybe_unused]] static bool focus = true, iconic = false;
[[maybe_unused]] static HWND foreground() { return focus ? hwnd : nullptr; }
[[maybe_unused]] static short key(int) { return 0; }
[[maybe_unused]] static BOOL client(HWND, RECT* r) { ui_only(); *r = {0, 0, width, height}; return TRUE; }
[[maybe_unused]] static BOOL cursor(POINT* p) { ui_only(); p->x = cursor_x, p->y = cursor_y; return TRUE; }
[[maybe_unused]] static BOOL to_client(HWND, POINT* p) { ui_only(); p->x -= origin_x, p->y -= origin_y; return TRUE; }
[[maybe_unused]] static BOOL minimized(HWND) { ui_only(); return iconic; }
[[maybe_unused]] static HWND capture() { return nullptr; }
[[maybe_unused]] static bool covered = false;
[[maybe_unused]] static HWND under_cursor(POINT) { ui_only(); return covered ? nullptr : hwnd; }
#define GetForegroundWindow foreground
#define GetAsyncKeyState key
#define GetClientRect client
#define GetCursorPos cursor
#define ScreenToClient to_client
#define IsIconic minimized
#define GetCapture capture
#define WindowFromPoint under_cursor
/* ACTUAL_WINDOWS */
#undef GetForegroundWindow
#undef GetAsyncKeyState
#undef GetClientRect
#undef GetCursorPos
#undef ScreenToClient
#undef IsIconic
#undef GetCapture
#undef WindowFromPoint
}

namespace sdl {
[[maybe_unused]] static HosPointer pointer = HOS_POINTER_INIT;
[[maybe_unused]] static int mouse_captured, ptr_in = 1, hos_posix_window[4] = {1, 0, 0, 0};
[[maybe_unused]] static uint32_t ptr_held;
[[maybe_unused]] static LONG ptr_x, ptr_y, ptr_pressed, ptr_wheel;
[[maybe_unused]] static void* window = (void*)(uintptr_t)1;
[[maybe_unused]] static int ww = 1280, wh = 720, pw = 1280, ph = 720;
[[maybe_unused]] static float x = 100, y = 200;
[[maybe_unused]] static void hos_sdl_window_size(void*, int* w, int* h) { ui_only(); *w = ww, *h = wh; }
[[maybe_unused]] static void hos_sdl_pixel_size(void*, int* w, int* h) { ui_only(); *w = pw, *h = ph; }
[[maybe_unused]] static void hos_sdl_mouse_position(float* a, float* b) { ui_only(); *a = x, *b = y; }
/* ACTUAL_SDL */
}

namespace adapter {
static int version;
static struct { void* GetMouseState; } f;
[[maybe_unused]] static uint32_t floats(float* x, float* y) { *x = 101.75f, *y = -0.25f; return 1; }
[[maybe_unused]] static uint32_t integers(int* x, int* y) { *x = 101, *y = -1; return 1; }
#define CALL(type, name) ((type)f.name)
/* ACTUAL_SDL_ADAPTER */
#undef CALL
}

int main() {
    float x, y;
    adapter::version = 3; adapter::f.GetMouseState = (void*)adapter::floats;
    adapter::hos_sdl_mouse_position(&x, &y); CHECK(x == 101.75f && y == -0.25f);
    adapter::version = 2; adapter::f.GetMouseState = (void*)adapter::integers;
    adapter::hos_sdl_mouse_position(&x, &y); CHECK(x == 101 && y == -1);
    // No foreign-thread SDL calls, including the Vulkan window-size callback.
    sdl::pointer_update(HOS_MOUSE_LEFT, 60);
    MkwVkPointer out{};
    std::thread first([&] {
        CHECK(sdl::hos_mouse_pointer(&out));
        CHECK(out.x == 100 && out.y == 200 && out.width == 1280 && out.height == 720);
        CHECK(out.pressed == HOS_MOUSE_LEFT && out.wheel == 0);
        int w, h; sdl::drawable_size(sdl::window, &w, &h); CHECK(w == 1280 && h == 720);
    });
    first.join();
    sdl::pointer_update(HOS_MOUSE_RIGHT, 60);
    CHECK(sdl::hos_mouse_pointer(&out) && out.pressed == HOS_MOUSE_RIGHT && out.wheel == 1);
    CHECK(sdl::hos_mouse_pointer(&out) && out.pressed == 0 && out.wheel == 0);
    // Fractional SDL3 coordinates: scale before quantizing, including just outside the top/left.
    for (int dpi : {96, 120, 144, 192}) {
        sdl::pw = 1280 * dpi / 96, sdl::ph = 720 * dpi / 96;
        sdl::x = 101.75f, sdl::y = 203.75f;
        sdl::pointer_update(0, 0);
        CHECK(sdl::hos_mouse_pointer(&out));
        CHECK(out.x == (int)std::floor(101.75 * dpi / 96) && out.y == (int)std::floor(203.75 * dpi / 96));
    }
    sdl::x = -0.25f, sdl::y = -0.25f;
    sdl::pointer_update(0, 0); CHECK(sdl::hos_mouse_pointer(&out) && out.x == -1 && out.y == -1);
    sdl::x = NAN; sdl::pointer_update(HOS_MOUSE_LEFT, 120); CHECK(!sdl::hos_mouse_pointer(&out));
    sdl::x = 0; sdl::wh = 0; sdl::pointer_update(0, 0); CHECK(!sdl::hos_mouse_pointer(&out));
    sdl::wh = 720; sdl::pointer_update(0, 0); CHECK(sdl::hos_mouse_pointer(&out) && !out.pressed && !out.wheel);
    // A stationary screen cursor must follow the new client origin and dimensions.
    win::pointer_update(win::hwnd, HOS_MOUSE_LEFT, -60);
    CHECK(win::hos_mouse_pointer(&out) && out.x == 100 && out.y == 200);
    win::origin_x = 50, win::origin_y = 80, win::width = 1720, win::height = 720;
    win::pointer_update(win::hwnd, HOS_MOUSE_RIGHT, -60);
    CHECK(win::hos_mouse_pointer(&out) && out.x == 50 && out.y == 120 && out.width == 1720);
    CHECK(out.pressed == HOS_MOUSE_RIGHT && out.wheel == -1);
    win::focus = false; win::pointer_update(win::hwnd, HOS_MOUSE_LEFT, 120); CHECK(!win::hos_mouse_pointer(&out));
    win::focus = true; win::pointer_update(win::hwnd, 0, 0); CHECK(win::hos_mouse_pointer(&out) && !out.pressed && !out.wheel);
    win::mouse_captured = 1; win::pointer_update(win::hwnd, HOS_MOUSE_LEFT, 60); CHECK(!win::hos_mouse_pointer(&out));
    win::mouse_captured = 0; win::pointer_update(win::hwnd, 0, 60); CHECK(win::hos_mouse_pointer(&out) && !out.pressed && !out.wheel);
    win::iconic = true; win::pointer_update(win::hwnd, 0, 120); CHECK(!win::hos_mouse_pointer(&out));
    win::iconic = false; win::pointer_update(win::hwnd, 0, 0); CHECK(win::hos_mouse_pointer(&out) && !out.wheel);
    win::covered = true; win::pointer_update(win::hwnd, HOS_MOUSE_LEFT, 120); CHECK(!win::hos_mouse_pointer(&out));
    win::covered = false; win::pointer_update(win::hwnd, 0, 0); CHECK(win::hos_mouse_pointer(&out) && !out.pressed && !out.wheel);
    // Concurrent resize/scale samples are never torn. All button edges and wheel steps are drained once.
    HosPointer state = HOS_POINTER_INIT;
    std::atomic<bool> done{false}; std::atomic<int> reads{0};
    std::thread reader([&] {
        MkwVkPointer p{};
        do {
            if (hos_pointer_take(&state, &p)) {
                CHECK(p.y == p.x * 2 && p.width == p.x * 3 && p.height == p.x * 4);
                ++reads;
            }
        } while (!done.load(std::memory_order_acquire));
    });
    for (int i = 1; i <= 100000; ++i) {
        MkwVkPointer p{}; p.x = i, p.y = i * 2, p.width = i * 3, p.height = i * 4;
        hos_pointer_publish(&state, p, 1);
    }
    done.store(true, std::memory_order_release); reader.join(); CHECK(reads > 0);
    MkwVkPointer p{}; p.width = 3440, p.height = 1440, p.pressed = HOS_MOUSE_LEFT, p.wheel = 60;
    hos_pointer_publish(&state, p, 1); p.pressed = HOS_MOUSE_RIGHT; hos_pointer_publish(&state, p, 1);
    CHECK(hos_pointer_take(&state, &out) && out.pressed == (HOS_MOUSE_LEFT | HOS_MOUSE_RIGHT) && out.wheel == 1);
    CHECK(hos_pointer_take(&state, &out) && !out.pressed && !out.wheel);
    hos_pointer_publish(&state, p, 1); hos_pointer_reset(&state);
    int w, h; hos_pointer_size(&state, &w, &h); CHECK(w == 3440 && h == 1440);
    CHECK(!hos_pointer_take(&state, &out) && !out.pressed && !out.wheel && !out.held);
    std::puts("PASS: coherent host pointer samples, main-thread SDL, stationary resize, fractional DPI and input lifetime");
}
