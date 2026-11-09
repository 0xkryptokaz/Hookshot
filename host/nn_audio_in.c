/* (hookshot) nn::audio's input streams: the microphone, as the ocarina's "play with your voice / an instrument" option uses it.

   The game lists the virtual input devices once at its start (ListVirtualInputDevices) and opens two input streams only if
   there is one: one on the default virtual device, one on the "true raw" one (GetTrueRawAudioInputDevice), both 48 kHz,
   mono, 16-bit (its StreamFormat: {PcmInt16, 1 channel, 48000}). With the ocarina's microphone mode on it starts them
   (StartInputStream when GetInputStreamState is not 2), reads up to 256 frames a frame (GetInputStreamFilledFrameCount,
   ReadInputStream) and works the notes out itself. Without these the stubs listed no device: the game never opened a
   stream, and the ocarina never heard anything.

   Here there is always one device of each kind (as on the console, whose microphone is always there), and a started
   stream reads the microphone the Insert menu chose (gpu/vk/vk_menu.cpp, Gameplay: Microphone; rx_option 18: 0 Off, 1
   the system's default, 2.. a device as hos_mic_name lists them) through SDL3 (host/hos_sdl.c), converted to the
   stream's format. Off, or no SDL3 or no microphone: silence at the stream's rate, so the game's reads go on as they
   would. A change in the menu takes effect at the next read. HOS_MIC_LOG=1: a line a second while a stream runs (its
   frames and level). */
#include "hos.h"
#include "hos_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int rx_option(int id, int otherwise);   /* (host/rx_backend.c: the Insert menu's settings) */

#define MIC_MAGIC 0x4D494349u   /* "MICI": the high word of our handle in the game's InputStreamType */
#define MAX_STREAMS 4
#define MAX_DEVICES 16
enum { STATE_STOPPED = 1, STATE_STARTED = 2 };   /* (the game starts a stream whose state is not 2, stops one whose is) */
/* nn::audio::SampleFormat */
enum { FMT_PCM8 = 1, FMT_PCM16 = 2, FMT_PCM24 = 3, FMT_PCM32 = 4, FMT_FLOAT = 5 };

typedef struct {
    int used, started;
    int format, channels, rate, frame_bytes, capacity;   /* (capacity: frames the stream holds, the open's last argument) */
    int raw;                     /* (opened on the "true raw" device) */
    void *mic;                   /* the SDL stream, while started with a microphone */
    int choice;                  /* the menu's choice the microphone was opened for */
    int64_t silence_from_ns;     /* (no microphone: silence counted from here) */
    int64_t silence_given;       /* frames of it read */
    int64_t frames_read, log_next_ns;
    int peak;                    /* (the loudest sample since the last log line, 16-bit) */
} InStream;

static InStream streams[MAX_STREAMS];
static CRITICAL_SECTION lock;
static volatile LONG lock_state;   /* 0 none, 1 making, 2 ready */
static uint32_t dev_ids[MAX_DEVICES];
static char dev_names[MAX_DEVICES][128];
static int dev_count = -1;   /* (-1: not listed yet) */
static int log_on = -1;

static void mic_lock(void) {
    if (InterlockedCompareExchange(&lock_state, 1, 0) == 0) {
        InitializeCriticalSection(&lock);
        InterlockedExchange(&lock_state, 2);
    }
    while (lock_state != 2) Sleep(0);
    EnterCriticalSection(&lock);
}
static void mic_unlock(void) { LeaveCriticalSection(&lock); }

/* ---- the devices, for the menu (gpu/vk/vk_api.h: MkwVkHost mic_count, mic_name) ---- */
int hos_mic_count(void) {   /* (lists them again: a microphone plugged in since shows) */
    mic_lock();
    dev_count = hos_sdl_mic_list(dev_ids, dev_names, MAX_DEVICES);
    const int n = dev_count;
    mic_unlock();
    return n;
}
const char *hos_mic_name(int index) {
    static char name[128];
    mic_lock();
    if (dev_count < 0) dev_count = hos_sdl_mic_list(dev_ids, dev_names, MAX_DEVICES);
    snprintf(name, sizeof name, "%s", index >= 0 && index < dev_count ? dev_names[index] : "");
    mic_unlock();
    return name;
}

/* ---- the streams ---- */
static int choice_now(void) { return rx_option(18, 1); }

static uint16_t sdl_format(int f) {   /* (SDL_AudioFormat: little-endian, signed) */
    return f == FMT_PCM8 ? 0x8008 : f == FMT_PCM32 ? 0x8020 : f == FMT_FLOAT ? 0x8120 : 0x8010;
}
static int sample_bytes(int f) { return f == FMT_PCM8 ? 1 : f == FMT_PCM16 ? 2 : f == FMT_PCM24 ? 3 : 4; }

static InStream *stream_of(uint64_t obj) {
    if (!obj) return NULL;
    const uint64_t h = G64(obj);
    const int i = (int)(h & 0xFFFF) - 1;
    if ((uint32_t)(h >> 32) != MIC_MAGIC || i < 0 || i >= MAX_STREAMS || !streams[i].used) return NULL;
    return &streams[i];
}

static void close_mic(InStream *s) {
    if (s->mic) hos_sdl_mic_close(s->mic);
    s->mic = NULL;
}
/* the microphone for the menu's choice (none when Off or none opens: silence), from now */
static void open_mic(InStream *s) {
    close_mic(s);
    s->choice = choice_now();
    s->silence_from_ns = hos_now_ns(), s->silence_given = 0;
    if (s->choice <= 0) {
        hos_log("audio in: stream %d: the microphone is off in the menu: silence", (int)(s - streams));
        return;
    }
    uint32_t id = 0;   /* (the system's default) */
    char name[128] = "the default microphone";
    if (s->choice >= 2) {
        if (dev_count < 0) dev_count = hos_sdl_mic_list(dev_ids, dev_names, MAX_DEVICES);
        if (s->choice - 2 < dev_count) id = dev_ids[s->choice - 2], snprintf(name, sizeof name, "%s", dev_names[s->choice - 2]);
    }
    /* (the device keeps its own format: SDL converts to the stream's, rate and channels too) */
    s->mic = hos_sdl_mic_open(id, s->rate, s->channels, sdl_format(s->format));
    hos_log("audio in: stream %d%s: %s %s (%d Hz, %d channel%s)", (int)(s - streams), s->raw ? " (raw)" : "", name,
            s->mic ? "open" : "did not open: silence", s->rate, s->channels, s->channels == 1 ? "" : "s");
}
/* frames the game can read now: the microphone's, at most the stream's capacity (older ones dropped), or silence's */
static int filled(InStream *s) {
    if (!s->started) return 0;
    if (s->choice != choice_now()) open_mic(s);   /* (the menu changed it) */
    if (s->mic) {
        int bytes = hos_sdl_mic_available(s->mic);
        const int most = s->capacity * s->frame_bytes;
        if (bytes > most) {   /* (the game stopped reading for a while: the newest only, not seconds of delay) */
            uint8_t drop[4096];
            for (int n = bytes - most; n > 0;) {
                const int got = hos_sdl_mic_read(s->mic, drop, n < (int)sizeof drop ? n : (int)sizeof drop);
                if (got <= 0) break;
                n -= got;
            }
            bytes = most;
        }
        return bytes / s->frame_bytes;
    }
    const int64_t due = (hos_now_ns() - s->silence_from_ns) * s->rate / 1000000000LL - s->silence_given;
    if (due > s->capacity) {
        s->silence_given += due - s->capacity;
        return s->capacity;
    }
    return due > 0 ? (int)due : 0;
}
static void note_read(InStream *s, const uint8_t *p, int frames) {
    s->frames_read += frames;
    if (log_on < 0) log_on = getenv("HOS_MIC_LOG") != NULL;
    if (!log_on) return;
    if (s->format == FMT_PCM16)
        for (int i = 0; i < frames * s->channels; i++) {
            int v = ((const int16_t *)p)[i];
            if (v < 0) v = -v;
            if (v > s->peak) s->peak = v;
        }
    const int64_t now = hos_now_ns();
    if (now >= s->log_next_ns) {
        hos_log("audio in: stream %d: %lld frames read, peak %d of 32767 in the last second", (int)(s - streams),
                (long long)s->frames_read, s->peak);
        s->peak = 0, s->log_next_ns = now + 1000000000LL;
    }
}

/* ListVirtualInputDevices(AudioDeviceName *names, int count): one (256 bytes, a name) */
HLE(_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi) {
    if ((int)ARG(1) <= 0 || !ARG(0)) { RET(0); return; }
    memset(GP(ARG(0)), 0, 256);
    memcpy(GP(ARG(0)), "MainAudioIn", 11);
    RET(1);
}
HLE(_ZN2nn5audio28GetDefaultVirtualInputDeviceEPNS0_15AudioDeviceNameE) {
    if (ARG(0)) memset(GP(ARG(0)), 0, 256), memcpy(GP(ARG(0)), "MainAudioIn", 11);
    RET(0);
}
HLE(_ZN2nn5audio26GetTrueRawAudioInputDeviceEPNS0_15AudioDeviceNameE) {
    if (ARG(0)) memset(GP(ARG(0)), 0, 256), memcpy(GP(ARG(0)), "RawAudioIn", 10);
    RET(0);
}
HLE(_ZN2nn5audio41IsVirtualInputDeviceStreamFormatSupportedERKNS0_15AudioDeviceNameERKNS0_12StreamFormatE) { RET(1); }

/* OpenInputStream(InputStreamType *, void *work, size_t, const AudioDeviceName &, const StreamFormat &, int frames).
   StreamFormat: {SampleFormat, channel count, sample rate} (the game's: {2, 1, 48000}) */
static void open_stream(Ctx *c, uint64_t obj, uint64_t name, uint64_t format, int frames) {
    mic_lock();
    int i = 0;
    while (i < MAX_STREAMS && streams[i].used) i++;
    if (!obj || !format || i == MAX_STREAMS) {
        mic_unlock();
        hos_log("audio in: an input stream did not open (%s)", i == MAX_STREAMS ? "too many" : "no stream or format");
        RET(0x1F3);   /* (a failure: the game leaves the stream alone) */
        return;
    }
    InStream *s = &streams[i];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->format = (int)G32(format), s->channels = (int)G32(format + 4), s->rate = (int)G32(format + 8);
    if (s->format < FMT_PCM8 || s->format > FMT_FLOAT || s->format == FMT_PCM24) s->format = FMT_PCM16;
    if (s->channels < 1 || s->channels > 8) s->channels = 1;
    if (s->rate < 8000 || s->rate > 192000) s->rate = 48000;
    s->frame_bytes = sample_bytes(s->format) * s->channels;
    s->capacity = frames > 0 ? frames : s->rate / 10;
    s->raw = name && !memcmp(GP(name), "RawAudioIn", 10);
    G64(obj) = (uint64_t)MIC_MAGIC << 32 | (uint64_t)(i + 1);
    mic_unlock();
    hos_log("audio in: stream %d opened%s: format %d, %d channel%s, %d Hz, %d frames", i, s->raw ? " (raw)" : "", s->format,
            s->channels, s->channels == 1 ? "" : "s", s->rate, s->capacity);
    RET(0);
}
HLE(_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi) {
    open_stream(c, ARG(0), ARG(3), ARG(4), (int)ARG(5));
}
/* (the same with a system event first: (stream, event, work, size, name, format, frames, ...)) */
HLE(_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEii) {
    open_stream(c, ARG(0), ARG(4), ARG(5), (int)ARG(6));
}
HLE(_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE) {
    mic_lock();
    InStream *s = stream_of(ARG(0));
    if (s) {
        close_mic(s);
        memset(s, 0, sizeof *s);
        G64(ARG(0)) = 0;
    }
    mic_unlock();
}
HLE(_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE) {
    mic_lock();
    InStream *s = stream_of(ARG(0));
    if (s && !s->started) {
        s->started = 1;
        open_mic(s);
    }
    mic_unlock();
    RET(s ? 0 : 0x1F3);
}
HLE(_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE) {
    mic_lock();
    InStream *s = stream_of(ARG(0));
    if (s && s->started) {
        s->started = 0;
        close_mic(s);
        hos_log("audio in: stream %d stopped (%lld frames read)", (int)(s - streams), (long long)s->frames_read);
    }
    mic_unlock();
    RET(0);
}
HLE(_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE) {   /* (what was recorded and not read: gone) */
    mic_lock();
    InStream *s = stream_of(ARG(0));
    if (s) {
        if (s->mic) hos_sdl_mic_clear(s->mic);
        s->silence_from_ns = hos_now_ns(), s->silence_given = 0;
    }
    mic_unlock();
    RET(0);
}
HLE(_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE) {
    mic_lock();
    InStream *s = stream_of(ARG(0));
    RET(s && s->started ? STATE_STARTED : STATE_STOPPED);
    mic_unlock();
}
HLE(_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE) {
    mic_lock();
    InStream *s = stream_of(ARG(0));
    RET(s ? filled(s) : 0);
    mic_unlock();
}
HLE(_ZN2nn5audio28GetInputStreamFreeFrameCountEPKNS0_15InputStreamTypeE) {
    mic_lock();
    InStream *s = stream_of(ARG(0));
    RET(s ? s->capacity - filled(s) : 0);
    mic_unlock();
}
HLE(_ZN2nn5audio32GetInputStreamCapacityFrameCountEPKNS0_15InputStreamTypeE) {
    mic_lock();
    InStream *s = stream_of(ARG(0));
    RET(s ? s->capacity : 0);
    mic_unlock();
}
/* ReadInputStream(InputStreamType *, void *buffer, int frames): the frames read */
HLE(_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi) {
    mic_lock();
    InStream *s = stream_of(ARG(0));
    int frames = 0;
    if (s && ARG(1) && (int)ARG(2) > 0) {
        const int have = filled(s);
        frames = (int)ARG(2) < have ? (int)ARG(2) : have;
        uint8_t *dst = (uint8_t *)GP(ARG(1));
        if (s->mic) {
            const int got = hos_sdl_mic_read(s->mic, dst, frames * s->frame_bytes);
            frames = got / s->frame_bytes;
        } else {
            memset(dst, 0, (size_t)frames * s->frame_bytes);
            s->silence_given += frames;
        }
        HOS_WRITTEN(dst, (size_t)frames * s->frame_bytes);
        note_read(s, dst, frames);
    }
    mic_unlock();
    RET(frames);
}
/* GetInputStreamStatistics(InputStreamType *, bool reset): a structure returned through x8; the game checks its second
   64-bit word (frames lost?) against 1. Nothing lost */
HLE(_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb) {
    if (c->x[8]) memset(GP(c->x[8]), 0, 16);
}
HLE(_ZN2nn5audio18SetInputStreamGainEPNS0_15InputStreamTypeEfNS_8TimeSpanE) { (void)c; }   /* (the game sets the gain it wants: the device's level as Windows has it) */
HLE(_ZN2nn5audio18GetInputStreamGainEPKNS0_15InputStreamTypeE) {
    const float one = 1.0f;
    c->vreg[0][0] = c->vreg[0][1] = 0;
    memcpy(&c->vreg[0][0], &one, 4);
}
