"""Check the microphone's input streams (host/nn_audio_in.c) as the game drives them: one device listed, a stream opened
on the game's format, started (state 2), fed silence at its rate with the microphone off, the microphone's samples once
the menu picks one (older ones dropped past the stream's capacity), stopped and closed. SDL, the menu and the clock are
stand-ins. No GPU or game files are needed.
python -I -B tests/test_audio_in.py [--cc gcc]
"""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HOS_H = r'''
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
#define HLE(sym) void imp_##sym(Ctx *c)
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(uintptr_t)(a))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
#define HOS_WRITTEN(p, n) ((void)0)
void hos_log(const char *fmt, ...);
int64_t hos_now_ns(void);
'''
WINDOWS_H = r'''
#pragma once
typedef long LONG;
typedef int CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG cmp) { LONG o = *p; if (o == cmp) *p = v; return o; }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
static inline void Sleep(int ms) { (void)ms; }
'''
FIXTURE = r'''
#include <stdarg.h>
#include <stdlib.h>
#include "nn_audio_in.c"

static int64_t clock_ns = 1000000000;
int64_t hos_now_ns(void) { return clock_ns; }
void hos_log(const char *fmt, ...) { (void)fmt; }
static int choice = 0;   /* (the menu's Microphone row: 0 off, 1 the default) */
int rx_option(int id, int otherwise) { return id == 18 ? choice : otherwise; }
/* one microphone, its samples a counter */
static int opened, closed, cleared, mic_bytes, next_sample, last_format, last_rate, last_channels;
int hos_sdl_mic_list(uint32_t *ids, char (*names)[128], int max) {
    if (max > 0) { ids[0] = 7; strcpy(names[0], "Test microphone"); }
    return max > 0;
}
void *hos_sdl_mic_open(uint32_t id, int freq, int channels, uint16_t format) {
    (void)id;
    opened++, last_rate = freq, last_channels = channels, last_format = format;
    return &opened;
}
int hos_sdl_mic_available(void *mic) { return mic ? mic_bytes : 0; }
int hos_sdl_mic_read(void *mic, void *dst, int bytes) {
    (void)mic;
    if (bytes > mic_bytes) bytes = mic_bytes;
    for (int i = 0; i < bytes / 2; i++) ((int16_t *)dst)[i] = (int16_t)(next_sample++);
    mic_bytes -= bytes;
    return bytes;
}
void hos_sdl_mic_clear(void *mic) { (void)mic; cleared++, mic_bytes = 0; }
void hos_sdl_mic_close(void *mic) { (void)mic; closed++; }

/* an HLE function called with its arguments in x0.. as the lifted code makes the call: the result in x0 */
static Ctx c;
static uint64_t call(void (*fn)(Ctx *), int n, ...) {
    va_list ap;
    va_start(ap, n);
    memset(&c, 0, sizeof c);
    for (int i = 0; i < n; i++) c.x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    fn(&c);
    return c.x[0];
}
#define NARGS(...) (int)(sizeof((uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t))
#define CALL(fn, ...) call(fn, NARGS(__VA_ARGS__), __VA_ARGS__)
#define FAIL(n, ...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return n; } while (0)
#define U(p) ((uint64_t)(uintptr_t)(p))

int main(void) {
    int cases = 0;
    static uint8_t names[8][256], raw[256];
    if (CALL(imp__ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi, U(names), (uint64_t)8) != 1 ||
        strcmp((char *)names[0], "MainAudioIn"))
        FAIL(1, "the device list: one device");
    if (CALL(imp__ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi, U(names), (uint64_t)0) != 0) FAIL(2, "a list of none");
    CALL(imp__ZN2nn5audio26GetTrueRawAudioInputDeviceEPNS0_15AudioDeviceNameE, U(raw));
    cases += 3;

    static uint64_t stream[0x360 / 8], other[0x360 / 8], never[0x360 / 8];
    static const uint32_t format[4] = {2, 1, 48000, 0};   /* {PcmInt16, 1 channel, 48000 Hz}: the game's */
    static uint8_t work[0x4000];
    if (CALL(imp__ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi,
             U(stream), U(work), (uint64_t)sizeof work, U(names[0]), U(format), (uint64_t)4800) != 0 || (stream[0] >> 32) != 0x4D494349u)
        FAIL(3, "the stream did not open");
    if (CALL(imp__ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi,
             U(other), U(work), (uint64_t)sizeof work, U(raw), U(format), (uint64_t)4800) != 0 || stream[0] == other[0])
        FAIL(4, "the raw stream did not open apart");
    cases += 2;

#define STATE(s) CALL(imp__ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE, U(s))
#define FILLED(s) CALL(imp__ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE, U(s))
#define READ(s, buf, n) CALL(imp__ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi, U(s), U(buf), (uint64_t)(n))
    if (STATE(stream) == 2 || FILLED(stream) != 0) FAIL(5, "an opened stream is not started, and has nothing");
    if (CALL(imp__ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE, U(never)) == 0 || STATE(never) == 2 || FILLED(never))
        FAIL(6, "a stream never opened does not start");
    if (CALL(imp__ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE, U(stream)) != 0 || STATE(stream) != 2)
        FAIL(7, "the started state is 2: the game's check");
    if (opened) FAIL(8, "the microphone opened while the menu has it off");
    cases += 4;

    /* off: silence at 48 kHz, at most the capacity */
    static int16_t buf[4800];
    clock_ns += 10000000;   /* 10 ms */
    if (FILLED(stream) != 480) FAIL(9, "10 ms of silence: %d frames, not 480", (int)FILLED(stream));
    memset(buf, 0x55, sizeof buf);
    if (READ(stream, buf, 256) != 256 || buf[0] || buf[255] || FILLED(stream) != 224) FAIL(10, "silence read");
    clock_ns += 5000000000LL;
    if (FILLED(stream) != 4800) FAIL(11, "silence held to the capacity");
    cases += 3;

    /* the menu picks the default microphone: it opens at the next read, in the stream's format */
    choice = 1, mic_bytes = 1000;
    if (FILLED(stream) != 500 || opened != 1 || last_rate != 48000 || last_channels != 1 || last_format != 0x8010)
        FAIL(12, "the microphone at 48 kHz, mono, S16: %d frames, open %d, %d Hz %d ch fmt %#x", (int)FILLED(stream),
             opened, last_rate, last_channels, last_format);
    if (READ(stream, buf, 256) != 256 || buf[0] != 0 || buf[255] != 255 || FILLED(stream) != 244) FAIL(13, "its samples read");
    mic_bytes = 20000;   /* the game stopped reading: only the newest capacity's worth is kept */
    if (FILLED(stream) != 4800 || mic_bytes != 9600) FAIL(14, "past the capacity: %d frames, %d bytes left", (int)FILLED(stream), mic_bytes);
    if (READ(stream, buf, 10000) != 4800) FAIL(15, "a read takes at most what there is");
    cases += 4;

    mic_bytes = 300;
    CALL(imp__ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE, U(stream));
    if (!cleared || FILLED(stream) != 0) FAIL(16, "purged");
    choice = 0;
    if (FILLED(stream) != 0 || closed != 1) FAIL(17, "off again: the microphone closed, silence from then (%d, %d)", (int)FILLED(stream), closed);
    clock_ns += 10000000;
    if (FILLED(stream) != 480) FAIL(22, "silence after the switch");
    cases += 2;

    memset(buf, 0, sizeof buf);
    c.x[8] = U(buf);
    buf[4] = 77;
    imp__ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb(&c);
    if (buf[4]) FAIL(18, "the statistics: nothing lost");
    cases++;

    choice = 1;
    FILLED(stream);
    CALL(imp__ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE, U(stream));
    if (STATE(stream) == 2 || FILLED(stream) != 0 || closed != 2) FAIL(19, "stopped: no frames, the microphone closed");
    CALL(imp__ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE, U(stream));
    if (stream[0] || READ(stream, buf, 16) != 0) FAIL(20, "closed: the handle gone");
    if (CALL(imp__ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi,
             U(stream), U(work), (uint64_t)sizeof work, U(names[0]), U(format), (uint64_t)4800) != 0)
        FAIL(21, "a closed stream's slot is used again");
    cases += 3;
    printf("PASS: %d input stream cases (device list, open, start, silence, microphone, capacity, purge, stop, close)\n", cases);
    return 0;
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default='gcc')
    a = p.parse_args()
    source = (ROOT / 'host' / 'nn_audio_in.c').read_text(encoding='utf-8')
    sdl_h = (ROOT / 'host' / 'hos_sdl.h').read_text(encoding='utf-8')
    # (the menu's row reaches the host as option 18, and the host gives the menu its microphones)
    replay = (ROOT / 'gpu' / 'vk' / 'vk_replay.cpp').read_text(encoding='utf-8')
    option = replay[replay.index('t.option = '):]
    option = option[:option.index('};') + 2]
    check('id == 18 ? opts.mic' in option, 'the host cannot see the menu\'s Microphone row (option 18)')
    check('"Microphone", &opts.mic, 0,' in (ROOT / 'gpu' / 'vk' / 'vk_menu.cpp').read_text(encoding='utf-8'),
          'the Insert menu has no Microphone row')
    check(re.search(r'hos_mic_count,\s*hos_mic_name', (ROOT / 'host' / 'rx_backend.c').read_text(encoding='utf-8')),
          'the menu is not given the microphones')
    check('rx_option(18, 1)' in source, 'the streams do not read the menu\'s choice')
    with tempfile.TemporaryDirectory(prefix='audio-in-') as t:
        folder = Path(t)
        for name, text in (('hos.h', HOS_H), ('windows.h', WINDOWS_H), ('hos_sdl.h', sdl_h), ('nn_audio_in.c', source),
                           ('fixture.c', FIXTURE)):
            (folder / name).write_text(text, encoding='utf-8')
        exe = folder / 'fixture'
        subprocess.run([a.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(folder), str(folder / 'fixture.c'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
