/* (hookshot) nn::audio's input streams: the microphone, as the ocarina's "play with your voice / an instrument" option uses it.

   The game lists the virtual input devices once at its start (ListVirtualInputDevices) and opens two input streams only if
   there is one: one on the default virtual device, one on the "true raw" one (GetTrueRawAudioInputDevice), both 48 kHz,
   mono, 16-bit (its StreamFormat: {PcmInt16, 1 channel, 48000}). With the ocarina's microphone mode on it starts them
   (StartInputStream when GetInputStreamState is not 2), reads up to 256 frames a frame (GetInputStreamFilledFrameCount,
   ReadInputStream) and works the notes out itself. Without these the stubs listed no device: the game never opened a
   stream, and the ocarina never heard anything.

   Here there is always one device of each kind (as on the console, whose microphone is always there). The started streams
   of one format share a source: one microphone opened once (the one the Insert menu chose: gpu/vk/vk_menu.cpp, Gameplay:
   Microphone; rx_option HOS_OPT_MIC: 0 Off, 1 the system's default, 2.. a device as hos_mic_name lists them), through SDL3,
   or SDL2 as the Linux AppImage carries (host/hos_sdl.c), converted to that format; what it records goes to each stream's
   own buffer, the newest capacity's worth kept. Off, or no SDL, or no microphone opening, or one that stops sending (pulled
   out): silence at the stream's rate, so the game's reads go on as they would, and a chosen microphone is tried again every
   few seconds (found by its name: it may come back under another id). A change in the menu takes effect at the next read.
   HOS_MIC_LOG=1: a line a second while a stream runs (its frames and level). */
#include "hos.h"
#include "hos_sdl.h"
#include "../gpu/vk/vk_api.h"   /* (HOS_OPT_MIC) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int rx_option(int id, int otherwise);   /* (host/rx_backend.c: the Insert menu's settings) */

#define MIC_MAGIC 0x4D494349u   /* "MICI": the high word of our handle in the game's InputStreamType */
#define MAX_STREAMS 4
#define MAX_DEVICES 16
#define QUIET_NS 2000000000LL   /* a microphone that gives nothing this long is taken as gone */
#define RETRY_NS 3000000000LL   /* a chosen microphone that is not there: tried again this often */
enum { STATE_STOPPED = 1, STATE_STARTED = 2 };   /* (the game starts a stream whose state is not 2, stops one whose is) */
/* nn::audio::SampleFormat */
enum { FMT_PCM8 = 1, FMT_PCM16 = 2, FMT_PCM24 = 3, FMT_PCM32 = 4, FMT_FLOAT = 5 };

typedef struct {
    int used, users;             /* (users: the started streams reading it) */
    int format, channels, rate, frame_bytes;
    int choice;                  /* the menu's choice it is for */
    char want[128];              /* the chosen microphone's name ("": the system's default) */
    uint32_t want_id;            /* (and the id it had when chosen: of two with one name, the one chosen) */
    void *mic;                   /* the SDL device, NULL: silence */
    int64_t heard_ns;            /* (the last time the microphone gave anything) */
    int64_t retry_ns;            /* (a chosen microphone not open: when to try again) */
    int64_t silence_from_ns, silence_made;   /* (silence: counted from here, frames of it made) */
    int failures;                /* (opens failed in a row: logged at the first) */
} Source;

typedef struct {
    int used, started, raw;
    int format, channels, rate, frame_bytes, capacity;   /* (capacity: frames the stream holds, the open's last argument) */
    int source;                  /* the source it reads while started (-1 none) */
    uint8_t *ring;               /* what was recorded for it and not read: capacity's worth at most, the oldest dropped */
    int ring_start, ring_len;
    int64_t frames_read, log_next_ns;
    int peak;                    /* (the loudest sample since the last log line, 16-bit) */
} InStream;

static InStream streams[MAX_STREAMS];
static Source sources[MAX_STREAMS];
static SRWLOCK lock = SRWLOCK_INIT;        /* the streams and the sources (the game's threads) */
static SRWLOCK list_lock = SRWLOCK_INIT;   /* the device list (the menu's thread too): never held while listing them */
static uint32_t dev_ids[MAX_DEVICES];
static char dev_names[MAX_DEVICES][128];
static int dev_count = -1;   /* (-1: not listed yet) */
static int log_on = -1;

/* ---- the devices, for the menu (gpu/vk/vk_api.h: MkwVkHost mic_count, mic_name) ---- */
int hos_mic_count(void) {   /* (lists them again: a microphone plugged in since shows; SDL asked without a lock held) */
    uint32_t ids[MAX_DEVICES];
    char names[MAX_DEVICES][128];
    const int n = hos_sdl_mic_list(ids, names, MAX_DEVICES);
    AcquireSRWLockExclusive(&list_lock);
    memcpy(dev_ids, ids, sizeof ids), memcpy(dev_names, names, sizeof names), dev_count = n;
    ReleaseSRWLockExclusive(&list_lock);
    return n;
}
const char *hos_mic_name(int index) {
    static char name[128];
    AcquireSRWLockShared(&list_lock);
    snprintf(name, sizeof name, "%s", index >= 0 && index < dev_count ? dev_names[index] : "");
    ReleaseSRWLockShared(&list_lock);
    return name;
}

/* ---- the sources: one microphone (or silence) for the started streams of one format ---- */
static uint16_t sdl_format(int f) {   /* (SDL_AudioFormat: little-endian, signed) */
    return f == FMT_PCM8 ? 0x8008 : f == FMT_PCM32 ? 0x8020 : f == FMT_FLOAT ? 0x8120 : 0x8010;
}
static int sample_bytes(int f) { return f == FMT_PCM8 ? 1 : f == FMT_PCM16 ? 2 : f == FMT_PCM24 ? 3 : 4; }

static void push(InStream *s, const uint8_t *in, int n) {   /* (into the stream's ring: the oldest dropped when full) */
    const int cap = s->capacity * s->frame_bytes;
    if (n > cap) {   /* (the newest capacity's worth) */
        if (in) in += n - cap;
        n = cap;
    }
    const int drop = s->ring_len + n - cap;
    if (drop > 0) s->ring_start = (s->ring_start + drop) % cap, s->ring_len -= drop;
    const int at = (s->ring_start + s->ring_len) % cap, first = n < cap - at ? n : cap - at;
    if (in) memcpy(s->ring + at, in, (size_t)first), memcpy(s->ring, in + first, (size_t)(n - first));
    else memset(s->ring + at, 0, (size_t)first), memset(s->ring, 0, (size_t)(n - first));   /* (silence) */
    s->ring_len += n;
}
static void push_all(const Source *src, const uint8_t *in, int n) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i].used && streams[i].started && &sources[streams[i].source] == src) push(&streams[i], in, n);
}

/* the SDL id of the chosen microphone: the one it was chosen as while a device of its name has that id (of two with one
   name, the one chosen), else the first of its name, in the menu's list or, retrying, in one listed now (a microphone
   plugged in again comes back under another id) */
static int pick(const uint32_t *ids, char (*names)[128], int n, const Source *src, uint32_t *id) {
    int first = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], src->want)) {
            if (ids[i] == src->want_id) return *id = ids[i], 1;
            if (first < 0) first = i;
        }
    if (first >= 0) *id = ids[first];
    return first >= 0;
}
static int find_device(const Source *src, uint32_t *id, int list_now) {
    AcquireSRWLockShared(&list_lock);
    const int found = pick(dev_ids, dev_names, dev_count, src, id);
    ReleaseSRWLockShared(&list_lock);
    if (found || !list_now) return found;
    uint32_t ids[MAX_DEVICES];
    char names[MAX_DEVICES][128];
    const int n = hos_sdl_mic_list(ids, names, MAX_DEVICES);
    return pick(ids, names, n, src, id);
}
static void close_mic(Source *src) {
    if (src->mic) hos_sdl_mic_close(src->mic);
    src->mic = NULL;
}
/* the source's microphone, as its choice wants (none: silence from now) */
static void open_mic(Source *src, int retry) {
    const int64_t now = hos_now_ns();
    if (src->mic) close_mic(src), src->silence_from_ns = now, src->silence_made = 0;   /* (silence from here) */
    if (src->choice <= 0) return;
    uint32_t id = 0;   /* (the system's default) */
    if (src->want[0] && !find_device(src, &id, retry)) {
        if (!src->failures++) hos_log("audio in: %s is not there: silence, tried again every few seconds", src->want);
        src->retry_ns = now + RETRY_NS;
        return;
    }
    src->mic = hos_sdl_mic_open(id, src->rate, src->channels, sdl_format(src->format));
    if (!src->mic) {
        if (!src->failures++)
            hos_log("audio in: %s did not open: silence, tried again every few seconds", src->want[0] ? src->want : "the default microphone");
        src->retry_ns = now + RETRY_NS;
        return;
    }
    hos_log("audio in: %s open (%d Hz, %d channel%s)%s", src->want[0] ? src->want : "the default microphone", src->rate,
            src->channels, src->channels == 1 ? "" : "s", src->failures ? ", back" : "");
    src->failures = 0, src->heard_ns = now;
}
/* the menu's choice now: a microphone by its name, opened again only when it is another one */
static void choose(Source *src, int choice) {
    char want[128] = "";
    uint32_t want_id = 0;
    if (choice >= 2) {
        AcquireSRWLockShared(&list_lock);
        if (choice - 2 < dev_count) snprintf(want, sizeof want, "%s", dev_names[choice - 2]), want_id = dev_ids[choice - 2];
        ReleaseSRWLockShared(&list_lock);
    }
    const int same = (choice > 0) == (src->choice > 0) && !strcmp(want, src->want) && want_id == src->want_id &&
                     src->choice != -1;
    src->choice = choice;
    if (same) return;
    snprintf(src->want, sizeof src->want, "%s", want), src->want_id = want_id;
    src->failures = 0;
    if (choice <= 0) hos_log("audio in: the microphone is off in the menu: silence");
    open_mic(src, 0);
}
/* what the source has recorded since, into its streams: the microphone's, or silence at the rate when there is none */
static void pump(Source *src) {
    const int64_t now = hos_now_ns();
    if (src->mic) {
        uint8_t buf[4096];
        const int chunk = (int)sizeof buf - (int)sizeof buf % src->frame_bytes;
        int heard = 0;
        for (int avail = hos_sdl_mic_available(src->mic); avail > 0;) {
            const int got = hos_sdl_mic_read(src->mic, buf, avail < chunk ? avail - avail % src->frame_bytes : chunk);
            if (got <= 0) break;
            push_all(src, buf, got), heard = 1;
            avail -= got;
        }
        if (heard) src->heard_ns = now;
        else if (now - src->heard_ns > QUIET_NS) {   /* (pulled out, or its driver stopped: silence until it is back) */
            hos_log("audio in: %s stopped sending: silence, tried again every few seconds",
                    src->want[0] ? src->want : "the default microphone");
            close_mic(src);
            src->silence_from_ns = now, src->silence_made = 0, src->retry_ns = now + RETRY_NS, src->failures = 1;
        }
        return;
    }
    if (src->choice > 0 && now >= src->retry_ns) {
        open_mic(src, 1);
        if (src->mic) return;
    }
    int64_t due = (now - src->silence_from_ns) * src->rate / 1000000000LL - src->silence_made;
    src->silence_made += due;
    if (due > src->rate) due = src->rate;   /* (more than a second is past every stream's capacity anyway) */
    if (due > 0) push_all(src, NULL, (int)due * src->frame_bytes);
}
static void pump_all(void) {
    const int choice = rx_option(HOS_OPT_MIC, 1);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (sources[i].used) {
            if (choice != sources[i].choice) choose(&sources[i], choice);
            pump(&sources[i]);
        }
}

/* ---- the streams ---- */
static InStream *stream_of(uint64_t obj) {
    if (!obj) return NULL;
    const uint64_t h = G64(obj);
    const int i = (int)(h & 0xFFFF) - 1;
    if ((uint32_t)(h >> 32) != MIC_MAGIC || i < 0 || i >= MAX_STREAMS || !streams[i].used) return NULL;
    return &streams[i];
}
static void attach(InStream *s) {   /* (the source of its format, made when there is none) */
    int free_at = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        const Source *src = &sources[i];
        if (src->used && src->format == s->format && src->channels == s->channels && src->rate == s->rate) {
            s->source = i, sources[i].users++;
            return;
        }
        if (!src->used && free_at < 0) free_at = i;
    }
    Source *src = &sources[free_at];   /* (as many sources as streams: one is free) */
    memset(src, 0, sizeof *src);
    src->used = 1, src->users = 1, src->choice = -1, src->silence_from_ns = hos_now_ns();
    src->format = s->format, src->channels = s->channels, src->rate = s->rate, src->frame_bytes = s->frame_bytes;
    s->source = free_at;
    choose(src, rx_option(HOS_OPT_MIC, 1));
}
static void detach(InStream *s) {
    if (s->source < 0) return;
    Source *src = &sources[s->source];
    s->source = -1;
    if (--src->users > 0) return;
    close_mic(src);
    memset(src, 0, sizeof *src);
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
   StreamFormat: {SampleFormat, channel count, sample rate} (the game's: {2, 1, 48000}). (The overload with an
   os::SystemEvent is not here: OoT does not import it, and a stub of it logs its first call, where an event never signalled
   could leave a waiter blocked) */
HLE(_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi) {
    const uint64_t obj = ARG(0), name = ARG(3), format = ARG(4);
    const int frames = (int)ARG(5);
    AcquireSRWLockExclusive(&lock);
    int i = 0;
    while (i < MAX_STREAMS && streams[i].used) i++;
    if (!obj || !format || i == MAX_STREAMS) {
        ReleaseSRWLockExclusive(&lock);
        hos_log("audio in: an input stream did not open (%s)", i == MAX_STREAMS ? "too many" : "no stream or format");
        RET(0x1F3);   /* (a failure: the game leaves the stream alone) */
        return;
    }
    InStream *s = &streams[i];
    memset(s, 0, sizeof *s);
    s->format = (int)G32(format), s->channels = (int)G32(format + 4), s->rate = (int)G32(format + 8);
    if (s->format < FMT_PCM8 || s->format > FMT_FLOAT || s->format == FMT_PCM24) s->format = FMT_PCM16;
    if (s->channels < 1 || s->channels > 8) s->channels = 1;
    if (s->rate < 8000 || s->rate > 192000) s->rate = 48000;
    s->frame_bytes = sample_bytes(s->format) * s->channels;
    s->capacity = frames > 0 ? frames : s->rate / 10;
    s->ring = (uint8_t *)malloc((size_t)s->capacity * s->frame_bytes);
    if (!s->ring) {
        ReleaseSRWLockExclusive(&lock);
        RET(0x1F3);
        return;
    }
    s->used = 1, s->source = -1;
    s->raw = name && !memcmp(GP(name), "RawAudioIn", 10);
    G64(obj) = (uint64_t)MIC_MAGIC << 32 | (uint64_t)(i + 1);
    ReleaseSRWLockExclusive(&lock);
    hos_log("audio in: stream %d opened%s: format %d, %d channel%s, %d Hz, %d frames", i, s->raw ? " (raw)" : "", s->format,
            s->channels, s->channels == 1 ? "" : "s", s->rate, s->capacity);
    RET(0);
}
HLE(_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE) {
    AcquireSRWLockExclusive(&lock);
    InStream *s = stream_of(ARG(0));
    if (s) {
        if (s->started) detach(s);
        free(s->ring);
        memset(s, 0, sizeof *s);
        G64(ARG(0)) = 0;
    }
    ReleaseSRWLockExclusive(&lock);
}
HLE(_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE) {
    AcquireSRWLockExclusive(&lock);
    InStream *s = stream_of(ARG(0));
    if (s && !s->started) {
        s->started = 1, s->ring_start = s->ring_len = 0;
        attach(s);
    }
    ReleaseSRWLockExclusive(&lock);
    RET(s ? 0 : 0x1F3);
}
HLE(_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE) {
    AcquireSRWLockExclusive(&lock);
    InStream *s = stream_of(ARG(0));
    if (s && s->started) {
        s->started = 0;
        detach(s);
        hos_log("audio in: stream %d stopped (%lld frames read)", (int)(s - streams), (long long)s->frames_read);
    }
    ReleaseSRWLockExclusive(&lock);
    RET(0);
}
HLE(_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE) {   /* (what was recorded and not read: gone) */
    AcquireSRWLockExclusive(&lock);
    InStream *s = stream_of(ARG(0));
    if (s) {
        if (s->started) pump_all();   /* (what the source had for it so far goes too) */
        s->ring_start = s->ring_len = 0;
    }
    ReleaseSRWLockExclusive(&lock);
    RET(0);
}
HLE(_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE) {
    AcquireSRWLockExclusive(&lock);
    InStream *s = stream_of(ARG(0));
    RET(s && s->started ? STATE_STARTED : STATE_STOPPED);
    ReleaseSRWLockExclusive(&lock);
}
static int filled(InStream *s) {
    if (!s->started) return 0;
    pump_all();
    return s->ring_len / s->frame_bytes;
}
HLE(_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE) {
    AcquireSRWLockExclusive(&lock);
    InStream *s = stream_of(ARG(0));
    RET(s ? filled(s) : 0);
    ReleaseSRWLockExclusive(&lock);
}
HLE(_ZN2nn5audio28GetInputStreamFreeFrameCountEPKNS0_15InputStreamTypeE) {
    AcquireSRWLockExclusive(&lock);
    InStream *s = stream_of(ARG(0));
    RET(s ? s->capacity - filled(s) : 0);
    ReleaseSRWLockExclusive(&lock);
}
HLE(_ZN2nn5audio32GetInputStreamCapacityFrameCountEPKNS0_15InputStreamTypeE) {
    AcquireSRWLockExclusive(&lock);
    InStream *s = stream_of(ARG(0));
    RET(s ? s->capacity : 0);
    ReleaseSRWLockExclusive(&lock);
}
/* ReadInputStream(InputStreamType *, void *buffer, int frames): the frames read */
HLE(_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi) {
    AcquireSRWLockExclusive(&lock);
    InStream *s = stream_of(ARG(0));
    int frames = 0;
    if (s && ARG(1) && (int)ARG(2) > 0) {
        const int have = filled(s);
        frames = (int)ARG(2) < have ? (int)ARG(2) : have;
        uint8_t *dst = (uint8_t *)GP(ARG(1));
        const int cap = s->capacity * s->frame_bytes, n = frames * s->frame_bytes;
        const int first = n < cap - s->ring_start ? n : cap - s->ring_start;
        memcpy(dst, s->ring + s->ring_start, (size_t)first);
        memcpy(dst + first, s->ring, (size_t)(n - first));
        s->ring_start = (s->ring_start + n) % cap, s->ring_len -= n;
        HOS_WRITTEN(dst, (size_t)n);
        note_read(s, dst, frames);
    }
    ReleaseSRWLockExclusive(&lock);
    RET(frames);
}
/* GetInputStreamStatistics(InputStreamType *, bool reset): a structure returned through x8; the game checks its second
   64-bit word (frames lost?) against 1. Nothing lost */
HLE(_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb) {
    if (c->x[8]) memset(GP(c->x[8]), 0, 16);
}
HLE(_ZN2nn5audio18SetInputStreamGainEPNS0_15InputStreamTypeEfNS_8TimeSpanE) { (void)c; }   /* (the device's level as the system has it) */
HLE(_ZN2nn5audio18GetInputStreamGainEPKNS0_15InputStreamTypeE) {
    const float one = 1.0f;
    c->vreg[0][0] = c->vreg[0][1] = 0;
    memcpy(&c->vreg[0][0], &one, 4);
}
