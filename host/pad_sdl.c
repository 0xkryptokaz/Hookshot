/* Game controllers through SDL (host/hos_sdl.c: SDL3, else SDL2, third_party/sdl2/SDL2.dll; zlib license, loaded at
   run time): DualSense, DualShock 4, Switch Pro, Xbox and most other pads, over USB or Bluetooth. XInput alone sees
   only Xbox-style pads.

   A thread of its own (on Linux the window's, which runs SDL there too) opens every controller SDL knows (and ones
   plugged in later), polls them every 4 ms and keeps
   the merged state; nn_hid.c ORs it into the input with XInput, keyboard and mouse. Without SDL nothing
   changes. HOS_NO_SDL=1 leaves SDL out. The inputs go by position (HOS_PAD_*, SDL's own order: the bottom face button
   first, cross on a DualSense, A on an Xbox pad); nn_hid.c's controls make them the Switch's buttons. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "hos_sdl.h"
#include "hd_rumble.h"
#include "../gpu/vk/vk_api.h"   /* (HOS_PAD_*) */

void hos_host_thread_register_self(const char *name);   /* hos_prof.c */

/* (BTN_A ... BTN_RIGHT are HOS_PAD_SOUTH ... HOS_PAD_RIGHT) */
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_BACK, BTN_GUIDE, BTN_START, BTN_LSTICK, BTN_RSTICK, BTN_LSHOULDER, BTN_RSHOULDER,
       BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_MISC1, BTN_N = 21 };
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT };

#define MAX_PADS 8
static struct {
    void *gc;
    int32_t id;
    uint16_t rumble_lo, rumble_hi;
    int64_t rumble_ns;
    uint8_t hd, hd_kind, hd_ever;   /* HD rumble (hd_rumble.h): HD_RUMBLE_UNKNOWN / ON / OFF, the pad's kind, sent once */
    uint8_t hd_sent[HD_RUMBLE_PACKET_SIZE];
    int64_t hd_ns;
} pads[MAX_PADS];
static SRWLOCK state_lock = SRWLOCK_INIT;
static uint64_t state_buttons;
static int32_t state_stick[4];
static int state_count;
static volatile LONG rumble_low, rumble_high;   /* 0..65535, set by the vibration HLE */
/* the game's own vibration values per side (0 left, 1 right: {amp_low, freq_low, amp_high, freq_high}), for the pads
   that play HD rumble; silence until the game sends one */
static SRWLOCK hd_lock = SRWLOCK_INIT;
static float hd_values[2][4] = {{0.0f, 160.0f, 0.0f, 320.0f}, {0.0f, 160.0f, 0.0f, 320.0f}};

static int32_t stick_value(int16_t v, int flip) {
    int32_t a = v < -32767 ? -32767 : v;
    if (flip) a = -a;
    return (a > 7849 || a < -7849) ? a : 0;   /* XInput's dead zone, as nn_hid.c uses */
}

/* SDL's controller support started; 0 when it could not be */
static int pads_start(void) {
    /* poll in the background, whatever window is in front (nn_hid.c decides when input counts) */
    hos_sdl_hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    /* (SDL2: PlayStation pads' rumble asked for; SDL3 turns it on when a rumble is first sent) */
    hos_sdl_hint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1");
    hos_sdl_hint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "1");
    /* (the map below is by position; SDL2 reports Nintendo pads by their labels unless told not to, which put a Switch Pro
       Controller's A, on the right, at the bottom button's place: A acted as B and X as Y: SkyboundTotk issue 12. SDL3's
       are by position always) */
    hos_sdl_hint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
    /* (with no window nothing reads SDL's quit event: SDL's own SIGINT/SIGTERM handlers turned a timeout's or a service
       manager's SIGTERM into that event, and the run went on until SIGKILL. Without them the signals end it as usual) */
    if (getenv("HOS_HEADLESS")) hos_sdl_hint("SDL_NO_SIGNAL_HANDLERS", "1");
    if (!hos_sdl_init(HOS_SDL_GAMEPAD)) {
        hos_log("hid: SDL could not start its controller support: %s", hos_sdl_error());
        return 0;
    }
    hos_log("hid: SDL controller support ready (DualSense, DualShock 4, Switch Pro, Xbox and others)");
    return 1;
}

/* SDL_GameControllerRumble runs only for the duration it is given, then stops, and a repeat of the same numbers was
   skipped. A rumble the game holds, and one that was already playing when this controller opened, never reached it. */
static int rumble_due(unsigned lo, unsigned hi, unsigned sent_lo, unsigned sent_hi, long long now, long long sent_ns) {
    if (lo != sent_lo || hi != sent_hi) return 1;
    if ((lo || hi) && now - sent_ns >= 500000000LL) return 1;   /* (half of the 1 s effect, so it does not lapse) */
    return 0;
}

/* The Insert menu's Rumble strength (percent; 0 Off) on the motors' 0..65535, heavy and light together: up to 100 each
   motor is scaled on its own (100 sends them as they are); above, when the stronger would pass the motor's top, both
   are scaled by the same factor so that it just reaches it, keeping the balance between the motors that the frequency
   split chose. Off stops a rumble that is playing (the poll sends the change). */
static void rumble_strength(unsigned heavy, unsigned light, int percent, uint16_t out[2]) {
    if (percent <= 0) {
        out[0] = out[1] = 0;
        return;
    }
    unsigned long long h = (unsigned long long)heavy * (unsigned)percent / 100u;
    unsigned long long l = (unsigned long long)light * (unsigned)percent / 100u;
    if (h > 0xFFFFu || l > 0xFFFFu) {   /* (from the motors' own values: the same factor for both, the stronger at the top) */
        const unsigned long long top = heavy > light ? heavy : light;
        h = heavy * 0xFFFFull / top;
        l = light * 0xFFFFull / top;
    }
    out[0] = (uint16_t)h;
    out[1] = (uint16_t)l;
}

/* A pad SDL just opened: HD rumble when it is a Switch Pro Controller or a Joy-Con and the SDL has the effect call
   (HOS_HD_RUMBLE=0: never); whether SDL's driver takes it shows with the first packet. */
static void hd_open(int i, void *gc) {
    static int env_off = -1;
    if (env_off < 0) {
        const char *e = getenv("HOS_HD_RUMBLE");
        env_off = e && !strcmp(e, "0");
    }
    pads[i].hd_kind = (uint8_t)hd_rumble_kind(hos_sdl_load(), hos_sdl_pad_type(gc));
    pads[i].hd = (uint8_t)hd_rumble_start(pads[i].hd_kind, hos_sdl_pad_has_effect(), env_off);
    pads[i].hd_ever = 0;
    pads[i].hd_ns = 0;
    const uint8_t n[4] = {0x00, 0x01, 0x40, 0x40};
    hd_rumble_packet(HD_RUMBLE_PRO, n, n, pads[i].hd_sent);
}

/* A pad's HD rumble, each poll: its packet when due (hd_rumble_due). 1 when the pad plays HD rumble (the motor rumble
   is not sent to it); 0 when it does not, or SDL just refused it (then it gets the motor rumble from now on). */
static int hd_poll(int i, const uint8_t side[2][4], long long now) {
    if (pads[i].hd == HD_RUMBLE_OFF) return 0;
    uint8_t packet[HD_RUMBLE_PACKET_SIZE];
    hd_rumble_packet(pads[i].hd_kind, side[0], side[1], packet);
    if (!hd_rumble_due(packet, pads[i].hd_sent, pads[i].hd_ever, now, pads[i].hd_ns)) return 1;
    const int was = pads[i].hd;
    pads[i].hd = (uint8_t)hd_rumble_after_send(hos_sdl_pad_effect(pads[i].gc, packet, sizeof packet));
    if (pads[i].hd == HD_RUMBLE_OFF) {
        hos_log("hid: \"%s\" did not take HD rumble (%s): its rumble goes to its motors", hos_sdl_pad_name(pads[i].gc),
                hos_sdl_error());
        pads[i].rumble_lo = pads[i].rumble_hi = 0;   /* (the motor path sends what is playing at once) */
        pads[i].rumble_ns = 0;
        return 0;
    }
    if (was == HD_RUMBLE_UNKNOWN) hos_log("hid: HD rumble on \"%s\"", hos_sdl_pad_name(pads[i].gc));
    memcpy(pads[i].hd_sent, packet, sizeof packet);
    pads[i].hd_ns = now;
    pads[i].hd_ever = 1;
    return 1;
}

/* one poll: controllers plugged in or gone (each second), then every open one's buttons, sticks and rumble */
static void pads_poll(void) {
    static int64_t next_scan = 0;
    hos_sdl_pads_update();
        const int64_t now = hos_now_ns();
        if (now >= next_scan) {   /* new controllers (and ones that went away) */
            next_scan = now + 1000000000LL;
            for (int i = 0; i < MAX_PADS; i++)
                if (pads[i].gc && !hos_sdl_pad_connected(pads[i].gc)) {
                    hos_log("hid: controller \"%s\" disconnected (SDL)", hos_sdl_pad_name(pads[i].gc));
                    hos_sdl_pad_close(pads[i].gc);
                    pads[i].gc = NULL;
                    pads[i].rumble_lo = pads[i].rumble_hi = 0;
                    pads[i].rumble_ns = 0;
                }
            int32_t ids[32];
            int opens[32];
            const int nj = hos_sdl_pads_list(ids, opens, 32);
            for (int j = 0; j < nj; j++) {
                const int32_t id = ids[j];
                int have = 0, slot = -1;
                for (int i = 0; i < MAX_PADS; i++) {
                    if (pads[i].gc && pads[i].id == id) have = 1;
                    if (!pads[i].gc && slot < 0) slot = i;
                }
                if (have || slot < 0) continue;
                void *gc = hos_sdl_pad_open(opens[j]);
                if (!gc) continue;
                pads[slot].gc = gc;
                pads[slot].id = hos_sdl_pad_id(gc);
                pads[slot].rumble_lo = pads[slot].rumble_hi = 0;
                pads[slot].rumble_ns = 0;
                hd_open(slot, gc);
                hos_log("hid: controller \"%s\" connected (SDL)", hos_sdl_pad_name(gc));
            }
        }
        uint64_t b = 0;
        int32_t s[4] = {0};
        int count = 0;
        int rx_option(int id, int otherwise);
        const int strength = rx_option(9, 100);   /* (the menu's Rumble strength: the motors' and HD rumble's) */
        uint16_t motors[2];
        rumble_strength((uint16_t)rumble_low, (uint16_t)rumble_high, strength, motors);
        const uint16_t lo = motors[0], hi = motors[1];
        uint8_t side[2][4];   /* (the HD rumble pads': each side's bytes) */
        {
            float hv[2][4], felt[2][4];
            AcquireSRWLockShared(&hd_lock);
            memcpy(hv, hd_values, sizeof hv);
            ReleaseSRWLockShared(&hd_lock);
            hd_rumble_felt(hv[0], felt[0]);   /* (the game's small amplitudes, as the motors get them) */
            hd_rumble_felt(hv[1], felt[1]);
            hd_rumble_encode(felt[0], strength, side[0]);
            hd_rumble_encode(felt[1], strength, side[1]);
        }
        for (int i = 0; i < MAX_PADS; i++) {
            void *gc = pads[i].gc;
            if (!gc) continue;
            count++;
            for (int k = BTN_A; k <= BTN_RIGHT; k++)   /* (SDL3's gamepad buttons are in SDL2's order) */
                if (hos_sdl_pad_button(gc, k)) b |= 1ULL << k;
            if (hos_sdl_pad_axis(gc, AX_LT) > 8000) b |= 1ULL << HOS_PAD_LTRIGGER;
            if (hos_sdl_pad_axis(gc, AX_RT) > 8000) b |= 1ULL << HOS_PAD_RTRIGGER;
            const int inv_x = rx_option(16, 0);
            const int inv_y = rx_option(17, 0);
            const int32_t v[4] = {stick_value(hos_sdl_pad_axis(gc, AX_LX), 0), stick_value(hos_sdl_pad_axis(gc, AX_LY), 1),
                                  stick_value(hos_sdl_pad_axis(gc, AX_RX), inv_x), stick_value(hos_sdl_pad_axis(gc, AX_RY), inv_y ? 0 : 1)};
            for (int k = 0; k < 4; k++)
                if ((v[k] < 0 ? -v[k] : v[k]) > (s[k] < 0 ? -s[k] : s[k])) s[k] = v[k];
            if (hd_poll(i, side, now)) continue;
            if (rumble_due(lo, hi, pads[i].rumble_lo, pads[i].rumble_hi, now, pads[i].rumble_ns)) {
                hos_sdl_pad_rumble(gc, lo, hi, lo || hi ? 1000 : 0);
                pads[i].rumble_lo = lo;
                pads[i].rumble_hi = hi;
                pads[i].rumble_ns = now;
            }
        }
        AcquireSRWLockExclusive(&state_lock);
        state_buttons = b;
        memcpy(state_stick, s, sizeof s);
        state_count = count;
        ReleaseSRWLockExclusive(&state_lock);
}

static DWORD WINAPI pad_main(void *arg) {
    (void)arg;
    hos_host_thread_register_self("gamepads (SDL)");
    if (!pads_start()) return 0;
    for (;;) {
        pads_poll();
        Sleep(4);
    }
}
#ifndef _WIN32
int hos_display_owns_sdl(void);   /* host/posix/display_sdl.c */
/* (linux) SDL also runs the window (host/posix/display_sdl.c), and its event queue is not for two threads. Polled
   from the window's thread, between its events. Two threads pumping it corrupted the heap once a controller was
   attached. */
static volatile LONG pads_loaded;
void hos_sdl_pads_pump(void) {
    static int state;   /* 0 not started, 1 polling, -1 not available */
    if (!state && pads_loaded) state = pads_start() ? 1 : -1;
    if (state > 0) pads_poll();
}
#endif

/* The controllers' merged state (nn_hid.c): ORs their inputs in (1 << HOS_PAD_*), keeps the larger stick
   deflections; returns how many controllers SDL has open. Starts the SDL thread on the first call. */
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]) {
    static volatile LONG started;
    if (!started && !InterlockedExchange(&started, 1)) {
        if (getenv("HOS_NO_SDL")) return 0;
        if (!hos_sdl_load()) {
            hos_log("hid: no SDL beside the exe or on the system: only XInput controllers (Xbox pads) work");
            return 0;
        }
#ifdef _WIN32
        CreateThread(NULL, 0, pad_main, NULL, 0, NULL);
#else
        if (hos_display_owns_sdl())
            InterlockedExchange(&pads_loaded, 1);   /* (the window's thread polls: hos_sdl_pads_pump) */
        else
            CreateThread(NULL, 0, pad_main, NULL, 0, NULL);   /* (no window: nothing else pumps SDL) */
#endif
    }
    AcquireSRWLockShared(&state_lock);
    const int n = state_count;
    *buttons |= state_buttons;
    for (int k = 0; k < 4; k++)
        if ((state_stick[k] < 0 ? -state_stick[k] : state_stick[k]) > (stick[k] < 0 ? -stick[k] : stick[k]))
            stick[k] = state_stick[k];
    ReleaseSRWLockShared(&state_lock);
    return n;
}

/* Vibration from the game (0..1 amplitudes of the low and high bands), for the pads that rumble. */
void hos_sdl_rumble(float low, float high) {
    if (low < 0) low = 0;
    if (low > 1) low = 1;
    if (high < 0) high = 0;
    if (high > 1) high = 1;
    InterlockedExchange(&rumble_low, (LONG)(low * 65535));
    InterlockedExchange(&rumble_high, (LONG)(high * 65535));
}

/* The game's own vibration value for one side (0 left, 1 right; {amp_low, freq_low, amp_high, freq_high}), for the
   pads that play HD rumble (hd_rumble.h); nn_hid.c's vibration_send, beside hos_sdl_rumble. */
void hos_sdl_rumble_hd(int side, const float value[4]) {
    AcquireSRWLockExclusive(&hd_lock);
    memcpy(hd_values[side & 1], value, sizeof hd_values[0]);
    ReleaseSRWLockExclusive(&hd_lock);
}
