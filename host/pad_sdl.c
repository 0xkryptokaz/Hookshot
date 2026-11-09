/* Game controllers through SDL (host/hos_sdl.c: SDL3, else SDL2, third_party/sdl2/SDL2.dll; zlib license, loaded at
   run time): DualSense, DualShock 4, Switch Pro, Xbox and most other pads, over USB or Bluetooth. XInput alone sees
   only Xbox-style pads.

   A thread of its own (on Linux the window's, which runs SDL there too) opens every controller SDL knows (and ones
   plugged in later), polls them every 4 ms and keeps
   the merged state; nn_hid.c ORs it into the input with XInput, keyboard and mouse. Without SDL nothing
   changes. HOS_NO_SDL=1 leaves SDL out. The inputs go by position (HOS_PAD_*, SDL's own order: the bottom face button
   first, cross on a DualSense, A on an Xbox pad); nn_hid.c's controls make them the Switch's buttons.

   (hookshot) Motion: with the Insert menu's Motion controls on (the default), the first open pad with a gyroscope and
   an accelerometer (Switch Pro and pads in Switch mode, DualSense, DualShock 4, others) has them read each poll and
   turned into the six-axis state OoT aims with (host/hos_motion.h), which nn_hid.c's GetSixAxisSensorStates gives
   the game; Motion sensitivity and Invert motion X/Y apply there. Off, or with no such pad, the game sees the pad
   lying still, as before. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "hos_sdl.h"
#include "hos_motion.h"
#include "../gpu/vk/vk_api.h"   /* (HOS_PAD_*) */

void hos_host_thread_register_self(const char *name);   /* hos_prof.c */

/* (BTN_A ... BTN_RIGHT are HOS_PAD_SOUTH ... HOS_PAD_RIGHT) */
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_BACK, BTN_GUIDE, BTN_START, BTN_LSTICK, BTN_RSTICK, BTN_LSHOULDER, BTN_RSHOULDER,
       BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_MISC1, BTN_N = 21 };
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT };

#define MAX_PADS 8
static struct { void *gc; int32_t id; uint16_t rumble_lo, rumble_hi; int64_t rumble_ns; int motion, motion_on; } pads[MAX_PADS];
static SRWLOCK state_lock = SRWLOCK_INIT;
static uint64_t state_buttons;
static int32_t state_stick[4];
static int state_count;
static volatile LONG rumble_low, rumble_high;   /* 0..65535, set by the vibration HLE */
/* (motion) the pad read, its filter, the last state and how many readings it has had; gyro_status for the menu */
static HosMotion motion;
static HosMotionState motion_state;
static int64_t motion_samples, motion_ns, motion_dt_ns, motion_log_ns;
static int motion_live, motion_pad = -1;
static volatile LONG gyro_status;   /* 0 no controller, 1 controllers but none with a gyro, 2 one with a gyro */

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

/* (motion) the sensors on while Motion controls is on (rx_option 18, on without the menu), off otherwise; the first pad
   with them read into the filter. count: the pads open */
static void motion_poll(int64_t now, int count) {
    int rx_option(int id, int otherwise);
    const int want = rx_option(18, 1) != 0;
    int pick = -1, any = 0;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i].gc || !pads[i].motion) continue;
        any = 1;
        if (pads[i].motion_on != want) {
            const int ok = hos_sdl_pad_sensor_enable(pads[i].gc, HOS_SDL_SENSOR_ACCEL, want) &
                           hos_sdl_pad_sensor_enable(pads[i].gc, HOS_SDL_SENSOR_GYRO, want);
            pads[i].motion_on = want;
            if (want && !ok) pads[i].motion = 0, hos_log("hid: \"%s\": its motion sensors could not be turned on", hos_sdl_pad_name(pads[i].gc));
        }
        if (want && pads[i].motion && pick < 0) pick = i;
    }
    InterlockedExchange(&gyro_status, any ? 2 : count ? 1 : 0);
    float a[3], g[3];
    const int read = pick >= 0 && hos_sdl_pad_sensor(pads[pick].gc, HOS_SDL_SENSOR_ACCEL, a, 3) &&
                     hos_sdl_pad_sensor(pads[pick].gc, HOS_SDL_SENSOR_GYRO, g, 3);
    AcquireSRWLockExclusive(&state_lock);
    if (!read) {
        motion_live = 0, motion_pad = -1;
    } else {
        if (pick != motion_pad) {   /* (a new pad: a fresh filter) */
            hos_motion_reset(&motion);
            motion_pad = pick, motion_ns = now;
            hos_log("hid: motion controls from \"%s\"", hos_sdl_pad_name(pads[pick].gc));
        }
        motion_dt_ns = now - motion_ns;
        /* (rx_option 19: the sensitivity in percent; 20 and 21: the horizontal and the vertical turned around) */
        hos_motion_update(&motion, a, g, (float)(motion_dt_ns / 1e9), rx_option(19, 100) / 100.0f, rx_option(20, 0),
                          rx_option(21, 0));
        if (getenv("HOS_MOTION_DEBUG") && now >= motion_log_ns) {   /* (twice a second: what the game gets) */
            motion_log_ns = now + 500000000LL;
            hos_log("hid: motion: up (%+.2f, %+.2f, %+.2f), pitch/yaw/roll %+.2f %+.2f %+.2f rad/s, aim tilt %+.3f, yaw "
                    "%+.3f turns/s", motion.up[0], motion.up[1], motion.up[2], motion.rates[0], motion.rates[1],
                    motion.rates[2], motion.pitch, motion.out.gyro[1]);
        }
        motion_ns = now;
        motion_state = motion.out;
        motion_samples++;
        motion_live = 1;
    }
    ReleaseSRWLockExclusive(&state_lock);
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
                    pads[i].motion = pads[i].motion_on = 0;
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
                pads[slot].motion = hos_sdl_pad_has_sensor(gc, HOS_SDL_SENSOR_GYRO) && hos_sdl_pad_has_sensor(gc, HOS_SDL_SENSOR_ACCEL);
                pads[slot].motion_on = 0;
                hos_log("hid: controller \"%s\" connected (SDL)%s", hos_sdl_pad_name(gc),
                        pads[slot].motion ? ", with a gyroscope" : "");
            }
        }
        uint64_t b = 0;
        int32_t s[4] = {0};
        int count = 0;
        const uint16_t lo = (uint16_t)rumble_low, hi = (uint16_t)rumble_high;
        for (int i = 0; i < MAX_PADS; i++) {
            void *gc = pads[i].gc;
            if (!gc) continue;
            count++;
            for (int k = BTN_A; k <= BTN_RIGHT; k++)   /* (SDL3's gamepad buttons are in SDL2's order) */
                if (hos_sdl_pad_button(gc, k)) b |= 1ULL << k;
            if (hos_sdl_pad_axis(gc, AX_LT) > 8000) b |= 1ULL << HOS_PAD_LTRIGGER;
            if (hos_sdl_pad_axis(gc, AX_RT) > 8000) b |= 1ULL << HOS_PAD_RTRIGGER;
            int rx_option(int id, int otherwise);
            const int inv_x = rx_option(16, 0);
            const int inv_y = rx_option(17, 0);
            const int32_t v[4] = {stick_value(hos_sdl_pad_axis(gc, AX_LX), 0), stick_value(hos_sdl_pad_axis(gc, AX_LY), 1),
                                  stick_value(hos_sdl_pad_axis(gc, AX_RX), inv_x), stick_value(hos_sdl_pad_axis(gc, AX_RY), inv_y ? 0 : 1)};
            for (int k = 0; k < 4; k++)
                if ((v[k] < 0 ? -v[k] : v[k]) > (s[k] < 0 ? -s[k] : s[k])) s[k] = v[k];
            if (rumble_due(lo, hi, pads[i].rumble_lo, pads[i].rumble_hi, now, pads[i].rumble_ns)) {
                hos_sdl_pad_rumble(gc, lo, hi, lo || hi ? 1000 : 0);
                pads[i].rumble_lo = lo;
                pads[i].rumble_hi = hi;
                pads[i].rumble_ns = now;
            }
        }
        motion_poll(now, count);
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

/* (motion) the newest six-axis state for nn_hid.c: 1 with *out, *samples (a count that grows with each reading) and
   *dt_ns (the time the reading covers); 0 when there is none (Motion controls off, or no pad with a gyroscope) */
int hos_sdl_motion(HosMotionState *out, int64_t *samples, int64_t *dt_ns) {
    AcquireSRWLockShared(&state_lock);
    const int live = motion_live;
    if (live) *out = motion_state, *samples = motion_samples, *dt_ns = motion_dt_ns;
    ReleaseSRWLockShared(&state_lock);
    return live;
}
/* (the Insert menu's Motion controls row) 0 no controller, 1 controllers with no gyroscope, 2 one with a gyroscope */
int hos_sdl_gyro_status(void) { return (int)gyro_status; }

/* Vibration from the game (0..1 amplitudes of the low and high bands), for the pads that rumble. */
void hos_sdl_rumble(float low, float high) {
    if (low < 0) low = 0;
    if (low > 1) low = 1;
    if (high < 0) high = 0;
    if (high > 1) high = 1;
    InterlockedExchange(&rumble_low, (LONG)(low * 65535));
    InterlockedExchange(&rumble_high, (LONG)(high * 65535));
}
