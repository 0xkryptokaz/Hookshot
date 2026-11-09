"""Check the microphone's input streams (host/nn_audio_in.c) as the game drives them: one device listed, streams opened on
the game's format and started (state 2), silence at their rate with the microphone off, one microphone opened for both
streams once the menu picks one and its samples given to each, the newest kept past a stream's capacity, silence and a
retry when the microphone stops sending or does not open, a named one opened by its id, purge, stop and close. SDL, the
menu and the clock are stand-ins. No GPU or game files are needed.
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
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; }
static inline void AcquireSRWLockShared(SRWLOCK *l) { (void)l; }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { (void)l; }
'''
FIXTURE = r'''
#include <stdarg.h>
#include <stdlib.h>
#include "nn_audio_in.c"

static int64_t clock_ns = 1000000000;
int64_t hos_now_ns(void) { return clock_ns; }
void hos_log(const char *fmt, ...) { (void)fmt; }
static int choice = 0;   /* (the menu's Microphone row: 0 off, 1 the default, 2 the first listed) */
int rx_option(int id, int otherwise) { return id == HOS_OPT_MIC ? choice : otherwise; }
/* one microphone, its samples a counter; mic_fail: it does not open */
static int opened, closed, mic_bytes, mic_fail, next_sample, last_format, last_rate, last_channels;
static uint32_t last_id = 99;
static int listed = 1;   /* (1: "Test microphone" as 7; 3: and two "Twin" as 8 and 9) */
int hos_sdl_mic_list(uint32_t *ids, char (*names)[128], int max) {
    static const char *const all[3] = {"Test microphone", "Twin", "Twin"};
    int n = 0;
    for (; n < listed && n < max; n++) ids[n] = 7 + (uint32_t)n, strcpy(names[n], all[n]);
    return n;
}
void *hos_sdl_mic_open(uint32_t id, int freq, int channels, uint16_t format) {
    if (mic_fail) return NULL;
    opened++, last_id = id, last_rate = freq, last_channels = channels, last_format = format;
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
void hos_sdl_mic_clear(void *mic) { (void)mic; mic_bytes = 0; }
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
#define OPEN(s, name) CALL(imp__ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi, \
                           U(s), U(work), (uint64_t)sizeof work, U(name), U(format), (uint64_t)4800)
#define START(s) CALL(imp__ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE, U(s))
#define STATE(s) CALL(imp__ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE, U(s))
#define FILLED(s) (int)CALL(imp__ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE, U(s))
#define READ(s, buf, n) (int)CALL(imp__ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi, U(s), U(buf), (uint64_t)(n))
#define PURGE(s) CALL(imp__ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE, U(s))
#define MS(n) (clock_ns += (int64_t)(n) * 1000000)

int main(void) {
    int cases = 0;
    static uint8_t names[8][256], raw[256], work[0x4000];
    static uint64_t stream[0x360 / 8], other[0x360 / 8], never[0x360 / 8];
    static const uint32_t format[4] = {2, 1, 48000, 0};   /* {PcmInt16, 1 channel, 48000 Hz}: the game's */
    static int16_t buf[4800], buf2[4800];
    if (CALL(imp__ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi, U(names), (uint64_t)8) != 1 ||
        strcmp((char *)names[0], "MainAudioIn"))
        FAIL(1, "the device list: one device");
    if (CALL(imp__ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi, U(names), (uint64_t)0) != 0) FAIL(2, "a list of none");
    CALL(imp__ZN2nn5audio26GetTrueRawAudioInputDeviceEPNS0_15AudioDeviceNameE, U(raw));
    if (OPEN(stream, names[0]) != 0 || (stream[0] >> 32) != 0x4D494349u) FAIL(3, "the stream did not open");
    if (OPEN(other, raw) != 0 || stream[0] == other[0]) FAIL(4, "the raw stream did not open apart");
    if (STATE(stream) == 2 || FILLED(stream) != 0) FAIL(5, "an opened stream is not started, and has nothing");
    if (START(never) == 0 || STATE(never) == 2 || FILLED(never)) FAIL(6, "a stream never opened does not start");
    if (START(stream) != 0 || STATE(stream) != 2) FAIL(7, "the started state is 2: the game's check");
    if (opened) FAIL(8, "the microphone opened while the menu has it off");
    cases += 8;

    /* off: silence at 48 kHz, at most the capacity */
    MS(10);
    if (FILLED(stream) != 480) FAIL(9, "10 ms of silence: %d frames, not 480", FILLED(stream));
    memset(buf, 0x55, sizeof buf);
    if (READ(stream, buf, 256) != 256 || buf[0] || buf[255] || FILLED(stream) != 224) FAIL(10, "silence read");
    MS(5000);
    if (FILLED(stream) != 4800) FAIL(11, "silence held to the capacity");
    START(other);
    MS(10);
    if (FILLED(other) != 480 || FILLED(stream) != 4800) FAIL(12, "the second stream: its own silence from its start");
    cases += 4;

    /* the default microphone: opened once for both streams, in their format, its samples given to each */
    PURGE(stream), PURGE(other);
    choice = 1, mic_bytes = 1000;
    if (FILLED(stream) != 500 || FILLED(other) != 500 || opened != 1 || last_id != 0 || last_rate != 48000 ||
        last_channels != 1 || last_format != 0x8010)
        FAIL(13, "one microphone for both, 48 kHz mono S16: %d and %d frames, opened %d, id %u, %d Hz %d ch fmt %#x",
             FILLED(stream), FILLED(other), opened, last_id, last_rate, last_channels, last_format);
    if (READ(stream, buf, 256) != 256 || READ(other, buf2, 256) != 256 || buf[0] != 0 || buf[255] != 255 ||
        memcmp(buf, buf2, 512))
        FAIL(14, "both streams read the same samples");
    mic_bytes = 20000;   /* the game stopped reading: only the newest capacity's worth is kept */
    if (FILLED(stream) != 4800 || mic_bytes != 0) FAIL(15, "past the capacity: %d frames", FILLED(stream));
    if (READ(stream, buf, 10000) != 4800 || buf[0] != 5700 || buf[4799] != 10499) FAIL(16, "the newest kept: %d .. %d", buf[0], buf[4799]);
    cases += 4;

    /* pulled out: nothing for 2 s, then silence, and opened again every 3 s once it can be */
    MS(1000);
    if (FILLED(stream) != 0 || closed) FAIL(17, "a second of nothing is not yet a lost microphone");
    MS(1500);
    if (FILLED(stream) != 0 || closed != 1) FAIL(18, "2.5 s of nothing: closed (%d)", closed);
    MS(10);
    if (FILLED(stream) != 480) FAIL(19, "silence once it stopped sending: %d", FILLED(stream));
    mic_fail = 1;
    MS(3000);
    (void)FILLED(stream);
    if (opened != 1 || FILLED(stream) != 4800) FAIL(20, "a failed retry: still silence (%d opens)", opened);
    mic_fail = 0;
    MS(3000);
    PURGE(stream);
    mic_bytes = 200;
    if (opened != 2 || FILLED(stream) != 100) FAIL(21, "the retry opened it again: %d opens, %d frames", opened, FILLED(stream));
    cases += 5;

    /* a named one, by the id SDL lists it under; the same one chosen again is not opened again */
    hos_mic_count();
    if (strcmp(hos_mic_name(0), "Test microphone") || hos_mic_name(1)[0]) FAIL(22, "the menu's list");
    choice = 2;
    (void)FILLED(stream);
    if (opened != 3 || last_id != 7 || closed != 2) FAIL(23, "the named microphone: %d opens, id %u", opened, last_id);
    choice = 2;
    (void)FILLED(stream);
    if (opened != 3) FAIL(24, "the same choice: not opened again");
    listed = 3;   /* two of one name: the one chosen opens, the second here */
    hos_mic_count();
    choice = 4;
    (void)FILLED(stream);
    if (opened != 4 || last_id != 9) FAIL(31, "the second of two named alike: id %u", last_id);
    choice = 0;
    (void)FILLED(stream);
    if (closed != 4) FAIL(25, "off: closed");
    cases += 5;

    memset(buf, 0, sizeof buf);
    c.x[8] = U(buf);
    buf[4] = 77;
    imp__ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb(&c);
    if (buf[4]) FAIL(26, "the statistics: nothing lost");
    cases++;

    choice = 1;
    (void)FILLED(stream);
    CALL(imp__ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE, U(stream));
    if (STATE(stream) == 2 || FILLED(stream) != 0 || closed != 4) FAIL(27, "one stopped: the other still has the microphone");
    CALL(imp__ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE, U(other));
    if (closed != 5) FAIL(28, "both stopped: the microphone closed");
    CALL(imp__ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE, U(stream));
    if (stream[0] || READ(stream, buf, 16) != 0) FAIL(29, "closed: the handle gone");
    if (OPEN(stream, names[0]) != 0) FAIL(30, "a closed stream's slot is used again");
    cases += 4;
    printf("PASS: %d input stream cases (device list, open, start, silence, a shared microphone, capacity, a lost one and its"
           " retry, a named one, stop, close)\n", cases);
    return 0;
}
'''


MENU_FIXTURE = r"""#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
struct Opts { int mic = 1; } opts;
std::vector<std::string> names;   /* the host's microphones */
struct Host { int (*mic_count)(); const char* (*mic_name)(int); };
int count() { return (int)names.size(); }
const char* name(int i) { return i >= 0 && i < (int)names.size() ? names[i].c_str() : ""; }
Host h{count, name}, *host = &h;
struct Row { int* value; int hi; } rows[] = {{&opts.mic, 17}};
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
@TEXT@
@REFRESH@
int main() {
    names = {"USB Mic", "USB Mic", "Webcam"};
    refresh_mics();
    CHECK(rows[0].hi == 4 && opts.mic == 1 && mic_want.empty());
    opts.mic = 3, mic_chosen();                  /* the second of two of one name */
    refresh_mics();
    CHECK(opts.mic == 3 && mic_want == "USB Mic");   /* stays the second */
    names = {"Webcam"};                          /* unplugged */
    refresh_mics();
    CHECK(opts.mic == 1 && mic_want == "USB Mic" && std::strstr(tx_mic(1), "not found") && rows[0].hi == 2);
    names = {"Webcam", "USB Mic"};               /* back, elsewhere in the list */
    refresh_mics();
    CHECK(opts.mic == 3 && !std::strcmp(tx_mic(3), "USB Mic"));
    opts.mic = 1, mic_chosen();                  /* the default chosen: the name goes */
    refresh_mics();
    CHECK(opts.mic == 1 && mic_want.empty() && !std::strcmp(tx_mic(1), "Default") && !std::strcmp(tx_mic(0), "Off"));
    names = {std::string(100, 'x'), "Webcam"};   /* a long name: kept short, matched by as much of it */
    opts.mic = 2, mic_chosen();
    CHECK(mic_want.size() == kMicNameKept && std::strlen("microphone_device=") + mic_want.size() + 1 < 80);
    names = {"Webcam", std::string(100, 'x')};
    refresh_mics();
    CHECK(opts.mic == 3);
    std::puts("PASS: the Microphone row keeps its choice by name");
    return 0;
}
"""


def block(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default='gcc')
    p.add_argument('--cxx', default='g++')
    a = p.parse_args()
    source = (ROOT / 'host' / 'nn_audio_in.c').read_text(encoding='utf-8')
    sdl_h = (ROOT / 'host' / 'hos_sdl.h').read_text(encoding='utf-8')
    # (the menu's row reaches the host as option 18, and the host gives the menu its microphones)
    replay = (ROOT / 'gpu' / 'vk' / 'vk_replay.cpp').read_text(encoding='utf-8')
    option = replay[replay.index('t.option = '):]
    option = option[:option.index('};') + 2]
    check('id == HOS_OPT_MIC ? opts.mic' in option, 'the host cannot see the menu\'s Microphone row (HOS_OPT_MIC)')
    api = (ROOT / 'gpu' / 'vk' / 'vk_api.h').read_text(encoding='utf-8')
    opt = re.search(r'enum \{ HOS_OPT_MIC = (\d+) \};', api)
    check(opt is not None, 'vk_api.h has no HOS_OPT_MIC')
    check('"Microphone", &opts.mic, 0,' in (ROOT / 'gpu' / 'vk' / 'vk_menu.cpp').read_text(encoding='utf-8'),
          'the Insert menu has no Microphone row')
    check(re.search(r'hos_mic_count,\s*hos_mic_name', (ROOT / 'host' / 'rx_backend.c').read_text(encoding='utf-8')),
          'the menu is not given the microphones')
    check('rx_option(HOS_OPT_MIC, 1)' in source, 'the streams do not read the menu\'s choice')
    include = '#include "../gpu/vk/vk_api.h"'
    check(include in source, 'nn_audio_in.c does not take HOS_OPT_MIC from vk_api.h')
    source = source.replace(include, '#define HOS_OPT_MIC %s' % opt.group(1))
    with tempfile.TemporaryDirectory(prefix='audio-in-') as t:
        folder = Path(t)
        for name, text in (('hos.h', HOS_H), ('windows.h', WINDOWS_H), ('hos_sdl.h', sdl_h), ('nn_audio_in.c', source),
                           ('fixture.c', FIXTURE)):
            (folder / name).write_text(text, encoding='utf-8')
        exe = folder / 'fixture'
        subprocess.run([a.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(folder), str(folder / 'fixture.c'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        # (the menu's side: the row's choice by name, gpu/vk/vk_menu.cpp)
        menu = (ROOT / 'gpu' / 'vk' / 'vk_menu.cpp').read_text(encoding='utf-8')
        fixture = MENU_FIXTURE.replace('@TEXT@', block(menu, 'constexpr int kMaxMics', 'const char* tx_robust'))
        fixture = fixture.replace('@REFRESH@', block(menu, 'void refresh_mics()', 'void save()'))
        (folder / 'menu.cpp').write_text(fixture, encoding='utf-8')
        menu_exe = folder / 'menu'
        subprocess.run([a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(folder / 'menu.cpp'), '-o', str(menu_exe)],
                       check=True)
        subprocess.run([str(menu_exe)], check=True)


if __name__ == '__main__':
    main()
