/*
 * Loopback tests: play a known signal out of the device, record what comes
 * back through a cable, and say whether the two agree.
 *
 * Needs a cable from the outputs to the inputs, both channels, at a level that
 * does not clip. Everything else the repo can test runs without hardware; this
 * is the only thing that closes the loop, and closing it is what catches the
 * failure mode this driver keeps producing -- correct byte counts, zero errors
 * and wrong audio. `frActCount == frReqCount` cannot tell you that the frames
 * were in the right order. A cable can.
 *
 * The client side is a single duplex AudioDeviceIOProc, which is the view an
 * application has: one call carries the input buffer and the output buffer and
 * the sample times of both, on the timeline the driver publishes through
 * GetZeroTimeStamp. Every measurement here is expressed on that timeline, so a
 * disagreement between it and the audio is exactly what shows up.
 *
 * The driver's own counters are read either side of every run, so each verdict
 * has two independent sources: what the driver thinks it did, and what came
 * back down the wire.
 */

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "loopback.h"

#define DEVICE_UID "net.quantum-bit.EMUTrackerPre"
#define CHANNELS   2

/*
 * Thresholds. Each one is a claim about the driver, not about the hardware's
 * audio quality -- the analogue path's own noise and distortion are reported
 * but not judged, because they depend on where the user set the gain.
 */
/* The device inserts a frame every tenth service interval until its startup
 * phase is recovered, and that ramp has been measured lasting 1.63 s
 * (FINDINGS) -- long enough to reach into a 0.75 s window and step the phase
 * this tool then judges. Two seconds is what the clock measurements already
 * discard, for the same reason. */
#define SETTLE_SECONDS       2.00   /* skip the stream-start transient */
#define TAIL_SECONDS         0.50   /* plan past the analysis window */

/* A frame that moved. The rings are addressed by absolute sample time, so the
 * phase between Core Audio and the USB cursors is fixed by construction; if it
 * moves at all, that construction is broken. Half a frame is far below what
 * any real slip would produce and far above the phase noise. */
#define MAX_SLIPPED_FRAMES   0.5

/* Level wander across the run. A dropout deep enough to matter moves this. */
#define MAX_LEVEL_SPREAD_DB  1.0

/* The tone must survive the trip. This is not an audio-quality threshold: a
 * clean converter pair does 20 dB better, and anything worse than this is a
 * splice, not distortion. */
#define MAX_THDN_DB        (-45.0)

/* Signal has to be there at all, and must not be clipping. */
#define MIN_LEVEL_DB       (-40.0)
#define MAX_PEAK             0.98

/* The other channel's tone, in this channel. A swap reads about 0 dB relative;
 * real crosstalk is below -60. */
#define MAX_CROSSTALK_DB   (-30.0)

/* Round trip measured against what Core Audio declares for the device. */
#define MAX_LATENCY_ERROR_MS 1.0
#define MAX_LATENCY_SPREAD_FRAMES 2.0

typedef struct {
    double      seconds;
    double      amplitude;
    double      rate;        /* 0: leave the device where it is */
    unsigned    buffer;      /* 0: likewise */
    unsigned    reps;
    unsigned    runs;        /* repetitions of the glitch test */
    const char* wav;

    /*
     * Where the returned audio is listened for. Defaults to the device under
     * test -- the cable goes out of it and back into it -- but a second
     * interface can take that job instead.
     *
     * It has to be able to, for two reasons. Self-loopback measures the
     * device's output through its own ADC, and at 176.4/192 kHz that ADC is
     * inside the fault being measured: the capture traffic is what provokes
     * the playback dropouts, and a bInterval 3 stream corrupts the IN stream
     * of whichever stream follows it. Worse, the driver can be asked to run
     * those rates with the input stream disabled entirely, and then there is
     * no ADC to measure through at all. A separate interface has its own
     * clock, its own converters and its own bus traffic, so it can hold an
     * opinion about this one.
     */
    AudioObjectID capture;   /* kAudioObjectUnknown: use the device itself */
    bool          split;     /* capture is a different device                */
} Options;

/* --- reporting ------------------------------------------------------------ */

static int gChecks, gFailures;

static void verdict(bool ok, const char* what, const char* fmt, ...)
{
    gChecks++;
    if (!ok) gFailures++;
    printf("  %-4s %-42s ", ok ? "ok" : "FAIL", what);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

static void note(const char* fmt, ...)
{
    printf("       %-42s ", "");
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

/* --- the device ----------------------------------------------------------- */

static AudioObjectID find_device(void)
{
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address,
                                       0, NULL, &size) != noErr) return 0;
    UInt32 count = size / sizeof(AudioObjectID);
    if (count > 64) count = 64;
    AudioObjectID devices[64];
    size = count * sizeof(AudioObjectID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                   0, NULL, &size, devices) != noErr) return 0;

    for (UInt32 i = 0; i < count; i++) {
        AudioObjectPropertyAddress uidAddress = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
        };
        CFStringRef uid = NULL;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(devices[i], &uidAddress, 0, NULL,
                                       &uidSize, &uid) != noErr || !uid) continue;
        Boolean match = CFStringCompare(uid, CFSTR(DEVICE_UID), 0) == kCFCompareEqualTo;
        CFRelease(uid);
        if (match) return devices[i];
    }
    return 0;
}

static UInt32 get_u32(AudioObjectID device, AudioObjectPropertySelector selector,
                      AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address = { selector, scope, kAudioObjectPropertyElementMain };
    UInt32 value = 0, size = sizeof value;
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &value) != noErr) return 0;
    return value;
}

static double get_rate(AudioObjectID device)
{
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    Float64 rate = 0;
    UInt32 size = sizeof rate;
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &rate) != noErr) return 0;
    return (double)rate;
}

/* The device may be shared, and a rate change is asynchronous: set it, then
 * wait for the read-back, the same discipline the driver uses on the wire. */
/*
 * Resolve a capture device by UID or by a substring of its name, listing what
 * is available either way -- nobody knows a third-party interface's UID, and
 * the listing is what makes the name form usable.
 *
 * Only input-capable devices are offered: a device with no input streams
 * cannot do this job, and silently matching one would fail much later with a
 * far less obvious message.
 */
static AudioObjectID find_capture_device(const char* needle)
{
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address,
                                       0, NULL, &size) != noErr || size == 0) {
        return kAudioObjectUnknown;
    }
    unsigned count = size / sizeof(AudioObjectID);
    AudioObjectID* ids = calloc(count, sizeof *ids);
    if (!ids) return kAudioObjectUnknown;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                   0, NULL, &size, ids) != noErr) {
        free(ids);
        return kAudioObjectUnknown;
    }

    AudioObjectID found = kAudioObjectUnknown;
    printf("input devices\n");
    for (unsigned i = 0; i < count; i++) {
        AudioObjectPropertyAddress streams = {
            kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput,
            kAudioObjectPropertyElementMain
        };
        UInt32 bytes = 0;
        if (AudioObjectGetPropertyDataSize(ids[i], &streams, 0, NULL, &bytes) != noErr
            || bytes == 0) {
            continue;
        }

        char name[256] = {0}, uid[256] = {0};
        CFStringRef cf = NULL;
        UInt32 n = sizeof cf;
        AudioObjectPropertyAddress na = {
            kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &n, &cf) == noErr && cf) {
            CFStringGetCString(cf, name, sizeof name, kCFStringEncodingUTF8);
            CFRelease(cf);
        }
        cf = NULL; n = sizeof cf;
        AudioObjectPropertyAddress ua = {
            kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(ids[i], &ua, 0, NULL, &n, &cf) == noErr && cf) {
            CFStringGetCString(cf, uid, sizeof uid, kCFStringEncodingUTF8);
            CFRelease(cf);
        }

        bool match = needle && (strcmp(uid, needle) == 0 || strstr(name, needle) != NULL);
        printf("  %s %-34s  %s\n", match ? "->" : "  ", name, uid);
        if (match && found == kAudioObjectUnknown) found = ids[i];
    }
    printf("\n");
    free(ids);
    return found;
}

static bool set_rate(AudioObjectID device, double rate)
{
    if (fabs(get_rate(device) - rate) < 1.0) return true;
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    Float64 want = rate;
    if (AudioObjectSetPropertyData(device, &address, 0, NULL, sizeof want, &want) != noErr) {
        return false;
    }
    for (int i = 0; i < 250; i++) {
        if (fabs(get_rate(device) - rate) < 1.0) { usleep(150000); return true; }
        usleep(20000);
    }
    return false;
}

static bool set_buffer(AudioObjectID device, unsigned frames)
{
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 want = frames;
    return AudioObjectSetPropertyData(device, &address, 0, NULL, sizeof want, &want) == noErr;
}

static unsigned available_rates(AudioObjectID device, double* out, unsigned capacity)
{
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, NULL, &size) != noErr) return 0;
    unsigned n = size / sizeof(AudioValueRange);
    if (n > capacity) n = capacity;
    AudioValueRange ranges[32];
    if (n > 32) n = 32;
    size = n * sizeof(AudioValueRange);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, ranges) != noErr) return 0;
    for (unsigned i = 0; i < n; i++) out[i] = (double)ranges[i].mMinimum;
    return n;
}

/* --- the driver's counters ------------------------------------------------ */

#define DIAG_MAX 64

typedef struct {
    char      key[64][40];
    long long value[64];
    unsigned  count;
} Diag;

static bool read_diag(AudioObjectID device, Diag* out)
{
    AudioObjectPropertyAddress address = {
        'emuD', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    CFPropertyListRef plist = NULL;
    UInt32 size = sizeof plist;
    memset(out, 0, sizeof *out);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &plist) != noErr
        || !plist) return false;
    if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) { CFRelease(plist); return false; }

    /* The driver decides how many counters it publishes, and
     * CFDictionaryGetKeysAndValues fills one slot per entry regardless of what
     * the caller has room for -- so the scratch arrays are sized from the
     * dictionary, not from DIAG_MAX. */
    CFDictionaryRef dict = (CFDictionaryRef)plist;
    CFIndex count = CFDictionaryGetCount(dict);
    const void** keys = calloc((size_t)count, sizeof *keys);
    const void** values = calloc((size_t)count, sizeof *values);
    if (!keys || !values) { free(keys); free(values); CFRelease(plist); return false; }

    CFDictionaryGetKeysAndValues(dict, keys, values);
    if (count > DIAG_MAX) count = DIAG_MAX;
    for (CFIndex i = 0; i < count; i++) {
        CFStringGetCString((CFStringRef)keys[i], out->key[i], sizeof out->key[i],
                           kCFStringEncodingUTF8);
        CFNumberGetValue((CFNumberRef)values[i], kCFNumberLongLongType, &out->value[i]);
    }
    out->count = (unsigned)count;
    free(keys);
    free(values);
    CFRelease(plist);
    return true;
}

static long long diag_get(const Diag* d, const char* key)
{
    for (unsigned i = 0; i < d->count; i++) {
        if (strcmp(d->key[i], key) == 0) return d->value[i];
    }
    return 0;
}

static long long diag_delta(const Diag* before, const Diag* after, const char* key)
{
    return diag_get(after, key) - diag_get(before, key);
}

static bool reset_counters(AudioObjectID device)
{
    AudioObjectPropertyAddress address = {
        'emuR', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    CFStringRef value = CFSTR("reset");
    return AudioObjectSetPropertyData(device, &address, 0, NULL, sizeof value, &value) == noErr;
}

/*
 * Counters that must not move while audio is flowing, and what a non-zero one
 * means. Enumerated here rather than assumed, so a driver that grows a new
 * counter does not quietly go unchecked -- the run also reports anything else
 * that moved.
 */
static const struct { const char* key; const char* means; } kMustStayZero[] = {
    { "outputUnderruns",  "frames the device played that Core Audio had not filled" },
    { "unfilledPlayback", "requests that went out still zeroed" },
    { "shortPlayback",    "a playback packet the bus called good and did not carry in full" },
    { "inputUnderruns",   "capture ring read past what had arrived" },
    { "inputOverruns",    "capture ring overwritten before it was read" },
    { "emptyCapture",     "a capture request came back with nothing in it" },
    { "resyncs",          "the schedule went stale and had to be rebuilt" },
    { "deadFrames",       "the timeline interval a rebuild could not cover" },
    { "bindRaces",        "a request was recycled under the IO thread's write" },
    { "unmappedFrames",   "audio no queued request covered" },
    { "unmappedAhead",    "the schedule was shorter than the write lead" },
    { "tsResets",         "the timestamp filter was thrown away and restarted" },
    { "usbErrors",        "isochronous transfers reported errors" },
    { "scheduleClamped",  "MAX_REQUESTS truncated the derived schedule depth" },
};

/* Counters that carry a level rather than a fault count, reported for context. */
static const char* const kLevels[] = {
    "outputLead", "inputDepth", "writeLeadMax", "anchorJitterMaxNs", "feedbackStarved",
};

/*
 * frameDeficit -- frames the device consumed beyond what Core Audio handed
 * over -- is a level, not a fault count. The two counters behind it are
 * advanced by different threads at different cadences (the engine every couple
 * of milliseconds, the IO thread once a buffer), so reading them together
 * catches an arbitrary point in the IO cycle and the difference swings by up
 * to one buffer for that reason alone. What would be a fault is *growth*: a
 * timeline running slower than the hardware adds a frame per cycle for as long
 * as the stream lasts, which leaves this far beyond a buffer or two.
 */
#define MAX_DEFICIT_BUFFERS 2

/* --- the duplex harness --------------------------------------------------- */

typedef struct {
    const float* plan;            /* interleaved, what we intend to play */
    uint64_t     plan_frames;
    float*       capture;         /* interleaved, indexed by input sample time */
    uint64_t     capture_capacity;

    _Atomic uint64_t captured;    /* frames of capture written so far */
    _Atomic uint64_t consumed;    /* frames of plan handed to the device */
    _Atomic unsigned cycles;
    _Atomic bool     running;

    /* Which device does which job. One device may do both; when two are in
     * play each callback is told apart by the device it arrived for, because
     * the device under test is duplex and will otherwise offer its own input
     * here -- taking that would interleave two unrelated sample-time bases
     * into one capture buffer. */
    AudioObjectID play_device, capture_device;
    bool          split;

    /* First cycle's sample times: the origin of each index space. With one
     * device both are taken on the same callback; with two, each is taken on
     * its own device's first callback, and the two bases are unrelated. */
    double in_origin, out_origin;
    bool   in_started, out_started;

    /* Continuity of the HAL's own sample times. A gap here is Core Audio
     * skipping a cycle, which is a different fault from a ring glitch and is
     * invisible to the driver's counters. */
    double   next_in, next_out;
    unsigned in_gaps, out_gaps;
    double   in_gap_worst, out_gap_worst;

    /* How far ahead of the input the output cycle sits: the HAL's own view of
     * the two safety offsets and the buffer. */
    double   lead_min, lead_max;

    unsigned frames_min, frames_max;
    uint64_t outside;             /* frames that fell outside the arrays */

    uint64_t last_host, host_gap_max;
} Loop;

static OSStatus loop_proc(AudioObjectID device, const AudioTimeStamp* now,
                          const AudioBufferList* input, const AudioTimeStamp* inputTime,
                          AudioBufferList* output, const AudioTimeStamp* outputTime,
                          void* refcon)
{
    (void)device;
    Loop* loop = (Loop*)refcon;

    /* Real-time thread: no allocation, no locks, no logging. */
    if (!atomic_load_explicit(&loop->running, memory_order_relaxed)) return noErr;

    bool have_in = input && input->mNumberBuffers > 0 && input->mBuffers[0].mData
                && inputTime && (inputTime->mFlags & kAudioTimeStampSampleTimeValid);
    bool have_out = output && output->mNumberBuffers > 0 && output->mBuffers[0].mData
                 && outputTime && (outputTime->mFlags & kAudioTimeStampSampleTimeValid);

    /* Each device does exactly one job. Without this the duplex device under
     * test contributes its own input alongside the capture device's, and the
     * two sample-time bases land in the same buffer -- which reads as the
     * capture stream skipping on almost every cycle. */
    if (loop->split) {
        bool is_capture = (device == loop->capture_device);
        have_in  = have_in  && is_capture;
        have_out = have_out && !is_capture;
    }

    if (loop->split) {
        if (have_out && !loop->out_started) {
            loop->out_origin = outputTime->mSampleTime;
            loop->next_out   = loop->out_origin;
            loop->lead_min   = 1e18;
            loop->lead_max   = -1e18;
            loop->frames_min = 0xffffffffu;
            loop->out_started = true;
        }
        if (have_in && !loop->in_started) {
            loop->in_origin = inputTime->mSampleTime;
            loop->next_in   = loop->in_origin;
            loop->in_started = true;
        }
    } else if (atomic_load_explicit(&loop->cycles, memory_order_relaxed) == 0) {
        if (!have_in || !have_out) return noErr;      /* wait for a full cycle */
        loop->in_origin = inputTime->mSampleTime;
        loop->out_origin = outputTime->mSampleTime;
        loop->next_in = loop->in_origin;
        loop->next_out = loop->out_origin;
        loop->lead_min = 1e18;
        loop->lead_max = -1e18;
        loop->frames_min = 0xffffffffu;
        loop->in_started = loop->out_started = true;
    }

    if (have_in && have_out) {
        double lead = outputTime->mSampleTime - inputTime->mSampleTime;
        if (lead < loop->lead_min) loop->lead_min = lead;
        if (lead > loop->lead_max) loop->lead_max = lead;
    }

    if (now && (now->mFlags & kAudioTimeStampHostTimeValid)) {
        if (loop->last_host) {
            uint64_t gap = now->mHostTime - loop->last_host;
            if (gap > loop->host_gap_max) loop->host_gap_max = gap;
        }
        loop->last_host = now->mHostTime;
    }

    if (have_out) {
        unsigned frames = output->mBuffers[0].mDataByteSize / (CHANNELS * sizeof(float));
        float* dst = (float*)output->mBuffers[0].mData;
        if (frames < loop->frames_min) loop->frames_min = frames;
        if (frames > loop->frames_max) loop->frames_max = frames;

        double delta = outputTime->mSampleTime - loop->next_out;
        if (fabs(delta) > 0.5) {
            loop->out_gaps++;
            if (fabs(delta) > fabs(loop->out_gap_worst)) loop->out_gap_worst = delta;
        }
        loop->next_out = outputTime->mSampleTime + frames;

        double position = outputTime->mSampleTime - loop->out_origin;
        if (position >= 0.0) {
            uint64_t at = (uint64_t)(position + 0.5);
            for (unsigned i = 0; i < frames; i++) {
                uint64_t p = at + i;
                bool inside = p < loop->plan_frames;
                dst[i * CHANNELS + 0] = inside ? loop->plan[p * CHANNELS + 0] : 0.0f;
                dst[i * CHANNELS + 1] = inside ? loop->plan[p * CHANNELS + 1] : 0.0f;
            }
            uint64_t reached = at + frames;
            if (reached > loop->plan_frames) reached = loop->plan_frames;
            atomic_store_explicit(&loop->consumed, reached, memory_order_relaxed);
        } else {
            memset(dst, 0, (size_t)frames * CHANNELS * sizeof(float));
        }
    }

    if (have_in) {
        /* The listening device need not be stereo: a four-input interface
         * presents all four in one buffer, so the frame count comes from its
         * own channel count and only the first pair is taken. */
        unsigned in_ch = input->mBuffers[0].mNumberChannels;
        if (in_ch == 0) in_ch = CHANNELS;
        unsigned frames = input->mBuffers[0].mDataByteSize / (in_ch * sizeof(float));
        const float* src = (const float*)input->mBuffers[0].mData;

        double delta = inputTime->mSampleTime - loop->next_in;
        if (fabs(delta) > 0.5) {
            loop->in_gaps++;
            if (fabs(delta) > fabs(loop->in_gap_worst)) loop->in_gap_worst = delta;
        }
        loop->next_in = inputTime->mSampleTime + frames;

        double position = inputTime->mSampleTime - loop->in_origin;
        if (position >= 0.0) {
            uint64_t at = (uint64_t)(position + 0.5);
            if (at + frames <= loop->capture_capacity) {
                if (in_ch == CHANNELS) {
                    memcpy(loop->capture + at * CHANNELS, src,
                           (size_t)frames * CHANNELS * sizeof(float));
                } else {
                    float* into = loop->capture + at * CHANNELS;
                    for (unsigned i = 0; i < frames; i++) {
                        into[i * CHANNELS + 0] = src[(size_t)i * in_ch + 0];
                        into[i * CHANNELS + 1] = in_ch > 1 ? src[(size_t)i * in_ch + 1]
                                                           : src[(size_t)i * in_ch + 0];
                    }
                }
                uint64_t reached = at + frames;
                if (reached > atomic_load_explicit(&loop->captured, memory_order_relaxed)) {
                    atomic_store_explicit(&loop->captured, reached, memory_order_relaxed);
                }
            } else {
                loop->outside += frames;
            }
        } else {
            loop->outside += frames;
        }
    }

    atomic_fetch_add_explicit(&loop->cycles, 1, memory_order_relaxed);
    return noErr;
}

/*
 * Start the stream, let it settle, zero the driver's counters, run the plan
 * out, stop. The analysis window opens after the reset so it is contained in
 * the counter window: every glitch the audio shows has a counter reading that
 * covers it.
 */
static bool run_stream(AudioObjectID device, Loop* loop, double rate,
                       Diag* before, Diag* after, Diag* final,
                       uint64_t* analysis_start)
{
    AudioDeviceIOProcID proc = NULL;
    if (AudioDeviceCreateIOProcID(device, loop_proc, loop, &proc) != noErr || !proc) {
        fprintf(stderr, "error: AudioDeviceCreateIOProcID failed\n");
        return false;
    }

    /* One callback serves both devices: it already keys off which buffer list
     * it was given, and a capture-only device never offers an output one. */
    AudioDeviceIOProcID listen = NULL;
    if (loop->split &&
        (AudioDeviceCreateIOProcID(loop->capture_device, loop_proc, loop, &listen) != noErr
         || !listen)) {
        fprintf(stderr, "error: AudioDeviceCreateIOProcID failed on the capture device\n");
        AudioDeviceDestroyIOProcID(device, proc);
        return false;
    }

    atomic_store(&loop->running, true);

    /* Listening starts first, so no part of the tone can go out before there
     * is something to hear it. */
    if (listen) {
        OSStatus ls = AudioDeviceStart(loop->capture_device, listen);
        if (ls != noErr) {
            fprintf(stderr, "error: AudioDeviceStart on the capture device: %d\n", (int)ls);
            if (ls == -2004 || ls == 560557673) {
                fprintf(stderr, "       microphone permission for this terminal is denied;\n"
                                "       System Settings > Privacy & Security > Microphone.\n");
            }
            AudioDeviceDestroyIOProcID(loop->capture_device, listen);
            AudioDeviceDestroyIOProcID(device, proc);
            return false;
        }
    }

    OSStatus status = AudioDeviceStart(device, proc);
    if (status != noErr) {
        fprintf(stderr, "error: AudioDeviceStart: %d\n", (int)status);
        if (status == -2004 || status == 560557673) {
            fprintf(stderr, "       microphone permission for this terminal is denied;\n"
                            "       System Settings > Privacy & Security > Microphone.\n");
        }
        if (listen) {
            AudioDeviceStop(loop->capture_device, listen);
            AudioDeviceDestroyIOProcID(loop->capture_device, listen);
        }
        AudioDeviceDestroyIOProcID(device, proc);
        return false;
    }

    usleep((useconds_t)(SETTLE_SECONDS * 1e6));
    reset_counters(device);
    read_diag(device, before);
    *analysis_start = atomic_load(&loop->captured);

    /* Wait for the plan to be handed over, then a little longer so the tail of
     * it has time to come back round the cable. */
    double limit = (double)loop->plan_frames / rate + 3.0;
    double waited = 0.0;
    while (atomic_load(&loop->consumed) < loop->plan_frames && waited < limit) {
        usleep(5000);
        waited += 0.005;
    }
    usleep(200000);

    /* Sampled while the stream is still running. Stopping drains a schedule
     * that is deeper than the safety offset, so the device plays out frames
     * Core Audio has stopped writing -- real, expected, and not this run's
     * doing. Teardown gets its own snapshot below. */
    read_diag(device, after);

    atomic_store(&loop->running, false);
    AudioDeviceStop(device, proc);
    if (listen) AudioDeviceStop(loop->capture_device, listen);
    read_diag(device, final);
    AudioDeviceDestroyIOProcID(device, proc);
    if (listen) AudioDeviceDestroyIOProcID(loop->capture_device, listen);
    return atomic_load(&loop->cycles) > 0;
}

/* --- counters, judged ----------------------------------------------------- */

/* Counters that can only move while the stream is being torn down, and so are
 * judged over a window that includes the stop. */
static const struct { const char* key; const char* means; } kTeardown[] = {
    { "bindRaces",     "a request was recycled under the IO thread's write" },
    { "usbErrors",     "isochronous transfers reported errors" },
};

static void report_counters(const Diag* before, const Diag* after, const Diag* final,
                            unsigned buffer_frames)
{
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof kMustStayZero / sizeof kMustStayZero[0]; i++) {
        long long moved = diag_delta(before, after, kMustStayZero[i].key);
        if (moved != 0) {
            bad++;
            note("%s %+lld -- %s", kMustStayZero[i].key, moved, kMustStayZero[i].means);
        }
    }
    verdict(bad == 0, "the driver reports no glitch of its own",
            bad == 0 ? "%u fault counters all still zero"
                     : "%u fault counters moved (above)",
            bad == 0 ? (unsigned)(sizeof kMustStayZero / sizeof kMustStayZero[0]) : bad);

    /* The stated invariant on the output path: everything Core Audio handed
     * over got bound into a request that went out. */
    long long to_output = diag_delta(before, after, "framesToOutput");
    long long bound = diag_delta(before, after, "framesBound");
    verdict(to_output == bound, "every frame written was bound to a request",
            "framesToOutput %lld, framesBound %lld", to_output, bound);

    long long deficit = diag_get(after, "frameDeficit");
    long long allowed = (long long)buffer_frames * MAX_DEFICIT_BUFFERS;
    verdict(deficit <= allowed, "the timeline keeps up with the hardware",
            "frameDeficit %lld frames, one IO buffer is %u", deficit, buffer_frames);

    unsigned torn = 0;
    for (unsigned i = 0; i < sizeof kTeardown / sizeof kTeardown[0]; i++) {
        long long moved = diag_delta(before, final, kTeardown[i].key);
        if (moved != 0) {
            torn++;
            note("%s %+lld -- %s", kTeardown[i].key, moved, kTeardown[i].means);
        }
    }
    verdict(torn == 0, "stopping the stream tore down cleanly",
            torn == 0 ? "no writer was left inside a freed buffer"
                      : "%u counters moved through the stop", torn);

    printf("       %-42s ", "");
    for (unsigned i = 0; i < sizeof kLevels / sizeof kLevels[0]; i++) {
        printf("%s %lld  ", kLevels[i], diag_get(after, kLevels[i]));
    }
    printf("\n");
}

static void report_cycles(const Loop* loop, double rate)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double host_gap_ms = (double)loop->host_gap_max * tb.numer / tb.denom / 1e6;

    verdict(loop->in_gaps == 0 && loop->out_gaps == 0,
            "Core Audio's sample times are unbroken",
            "%u input and %u output discontinuities%s",
            loop->in_gaps, loop->out_gaps,
            (loop->in_gaps || loop->out_gaps) ? "" : " over the run");
    if (loop->in_gaps || loop->out_gaps) {
        note("worst: input %+.0f frames, output %+.0f frames",
             loop->in_gap_worst, loop->out_gap_worst);
    }
    verdict(loop->outside == 0, "every captured frame landed in the window",
            "%llu frames outside", (unsigned long long)loop->outside);

    note("%u IO cycles, %u..%u frames each, longest gap %.2f ms",
         atomic_load((_Atomic unsigned*)&loop->cycles), loop->frames_min,
         loop->frames_max, host_gap_ms);
    /* One device driving both directions has a single cycle to compare; two
     * devices do not. */
    if (!loop->split)
    note("output cycle leads input by %.0f..%.0f frames (%.2f..%.2f ms)",
         loop->lead_min, loop->lead_max, loop->lead_min * 1000.0 / rate,
         loop->lead_max * 1000.0 / rate);
}

/* --- signal helpers ------------------------------------------------------- */

/*
 * Where the two tones sit. Both land exactly on an FFT bin so the played
 * signal is EMU_BLOCK-periodic (see loopback.h), near 1 kHz and 1.6 kHz at any
 * rate. The right channel is then nudged clear of the left one's harmonics, so
 * distortion can never be mistaken for a channel swap.
 */
static void choose_bins(double rate, unsigned* left, unsigned* right)
{
    unsigned l = (unsigned)(1000.0 * EMU_BLOCK / rate + 0.5);
    unsigned r = (unsigned)(1600.0 * EMU_BLOCK / rate + 0.5);
    if (l < 4) l = 4;
    if (r <= l + 2) r = l + 3;
    for (;;) {
        bool clash = false;
        for (unsigned m = 1; m <= 8 && !clash; m++) {
            if (labs((long)r - (long)(m * l)) <= 2) clash = true;
            if (labs((long)l - (long)(m * r)) <= 2) clash = true;
        }
        if (!clash) break;
        r++;
    }
    *left = l;
    *right = r;
}

static void deinterleave(const float* src, uint64_t frames, unsigned channel, float* dst)
{
    for (uint64_t i = 0; i < frames; i++) dst[i] = src[i * CHANNELS + channel];
}

/* Ramp the ends of the plan so starting and stopping is not a click. Both
 * ramps sit outside the analysis window. */
static void apply_fade(float* interleaved, uint64_t frames, uint64_t fade)
{
    if (fade * 2 >= frames) return;
    for (uint64_t i = 0; i < fade; i++) {
        float g = (float)i / (float)fade;
        for (unsigned c = 0; c < CHANNELS; c++) {
            interleaved[i * CHANNELS + c] *= g;
            interleaved[(frames - 1 - i) * CHANNELS + c] *= g;
        }
    }
}

/* 32-bit float WAV, so a capture can be looked at offline without a
 * quantisation step in the way. */
static void write_wav(const char* path, const float* interleaved, uint64_t frames,
                      double rate)
{
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "warning: cannot write %s\n", path); return; }

    uint32_t data_bytes = (uint32_t)(frames * CHANNELS * sizeof(float));
    uint32_t riff = 4 + (8 + 18) + (8 + 4) + (8 + data_bytes);
    uint16_t format = 3 /* IEEE float */, channels = CHANNELS, bits = 32, extension = 0;
    uint32_t fmt_size = 18, fact_size = 4, sample_rate = (uint32_t)(rate + 0.5);
    uint32_t byte_rate = sample_rate * CHANNELS * (uint32_t)sizeof(float);
    uint16_t align = CHANNELS * (uint16_t)sizeof(float);
    uint32_t fact = (uint32_t)frames;

    fwrite("RIFF", 1, 4, f);      fwrite(&riff, 4, 1, f);      fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);      fwrite(&fmt_size, 4, 1, f);
    fwrite(&format, 2, 1, f);     fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);      fwrite(&bits, 2, 1, f);      fwrite(&extension, 2, 1, f);
    fwrite("fact", 1, 4, f);      fwrite(&fact_size, 4, 1, f); fwrite(&fact, 4, 1, f);
    fwrite("data", 1, 4, f);      fwrite(&data_bytes, 4, 1, f);
    fwrite(interleaved, 1, data_bytes, f);
    fclose(f);
    printf("       wrote %s (%llu frames)\n", path, (unsigned long long)frames);
}

/* --- the tone test -------------------------------------------------------- */

/*
 * kDepthPrime runs a stream and looks at nothing.
 *
 * A bInterval 3 stream leaves exactly one following stream corrupt, at any
 * rate, and only running a stream clears it -- waiting does not. So a single
 * throwaway run before a measurement *creates* the corruption it was meant to
 * absorb, and the results alternate broken/clean in a way that looks like a
 * real difference between whatever two things are being compared. Two
 * throwaway runs land the measurement on the clean side of that alternation.
 *
 * This is left to the tool rather than the operator because it is not
 * discoverable from the output: a poisoned run fails in ways that read as
 * driver faults, and comparing single runs across it produces confident,
 * wrong conclusions.
 */
typedef enum { kDepthPrime, kDepthSmoke, kDepthFull } Depth;

static int tone_test(AudioObjectID device, const Options* options, double seconds,
                     Depth depth, const char* title)
{
    double rate = get_rate(device);
    if (rate <= 0) { fprintf(stderr, "error: no sample rate\n"); return 1; }

    unsigned bin[CHANNELS];
    choose_bins(rate, &bin[0], &bin[1]);

    uint64_t plan_frames = (uint64_t)((SETTLE_SECONDS + seconds + TAIL_SECONDS) * rate);
    uint64_t capacity = plan_frames + (uint64_t)(rate * 0.5);

    float* plan = calloc(plan_frames * CHANNELS, sizeof(float));
    float* capture = calloc(capacity * CHANNELS, sizeof(float));
    float* mono = calloc(capacity, sizeof(float));
    if (!plan || !capture || !mono) { fprintf(stderr, "error: out of memory\n"); return 1; }

    for (unsigned c = 0; c < CHANNELS; c++) {
        emu_gen_tone(mono, plan_frames, bin[c], options->amplitude);
        for (uint64_t i = 0; i < plan_frames; i++) plan[i * CHANNELS + c] = mono[i];
    }
    apply_fade(plan, plan_frames, (uint64_t)(rate * 0.02));

    if (depth != kDepthPrime) {
    printf("\n-- %s at %.0f Hz\n", title, rate);
    printf("   left %.1f Hz (bin %u), right %.1f Hz (bin %u), %.1f dBFS, %.1f s\n\n",
           bin[0] * rate / EMU_BLOCK, bin[0], bin[1] * rate / EMU_BLOCK, bin[1],
           20.0 * log10(options->amplitude), seconds);
    }

    Loop loop;
    memset(&loop, 0, sizeof loop);
    loop.plan = plan;
    loop.plan_frames = plan_frames;
    loop.capture = capture;
    loop.capture_capacity = capacity;
    loop.play_device = device;
    loop.capture_device = options->split ? options->capture : device;
    loop.split = options->split;

    Diag before, after, final;
    uint64_t analysis_start = 0;
    bool ran = run_stream(device, &loop, rate, &before, &after, &final, &analysis_start);

    if (depth == kDepthPrime) {
        free(plan); free(capture); free(mono);
        return ran ? 0 : 1;
    }

    uint64_t captured = atomic_load(&loop.captured);
    verdict(ran && captured > 0, "the stream ran and delivered audio",
            "%llu frames captured over %u IO cycles",
            (unsigned long long)captured, atomic_load(&loop.cycles));
    if (!ran || captured == 0) {
        note("Core Audio never delivered a buffer: the input was never driven.");
        free(plan); free(capture); free(mono);
        return 1;
    }

    uint64_t window = (uint64_t)(seconds * rate);
    if (analysis_start + window > captured) {
        window = captured > analysis_start ? captured - analysis_start : 0;
    }
    verdict(window >= EMU_BLOCK, "enough audio came back to analyse",
            "%llu frames from %llu", (unsigned long long)window,
            (unsigned long long)analysis_start);
    if (window < EMU_BLOCK) { free(plan); free(capture); free(mono); return 1; }

    report_cycles(&loop, rate);
    report_counters(&before, &after, &final, loop.frames_max);

    static const char* const kName[CHANNELS] = { "left", "right" };
    int failed_before = gFailures;

    for (unsigned c = 0; c < CHANNELS; c++) {
        deinterleave(capture + analysis_start * CHANNELS, window, c, mono);
        EmuToneSummary s;
        emu_tone_scan(mono, window, bin[c], bin[c ^ 1], &s);

        char what[64];
        snprintf(what, sizeof what, "%s: the tone is there, unclipped", kName[c]);
        verdict(s.level_mean_db > MIN_LEVEL_DB && s.peak < MAX_PEAK, what,
                "%.2f dBFS, peak %.4f", s.level_mean_db, s.peak);

        snprintf(what, sizeof what, "%s: it is this channel's tone", kName[c]);
        verdict(s.rival_worst_db - s.level_mean_db < MAX_CROSSTALK_DB, what,
                "the other channel's tone is %.1f dB down",
                s.level_mean_db - s.rival_worst_db);

        if (depth == kDepthSmoke) continue;

        /* Two crystals walk against each other by definition, so with a
         * separate capture device phase and drift stop being faults and become
         * measurements. What still has to hold is that the waveform does not
         * jump: the analyser separates a slow walk from a sudden step, and
         * selftest covers exactly that distinction. */
        snprintf(what, sizeof what, "%s: no frame moved", kName[c]);
        verdict(options->split || fabs(s.frames_slipped) < MAX_SLIPPED_FRAMES, what,
                "phase span %.3f deg = %.3f frames over %u blocks%s",
                s.phase_span_deg, s.frames_slipped, s.blocks,
                options->split ? "   (two clocks; measured, not judged)" : "");
        if (!options->split && fabs(s.frames_slipped) >= MAX_SLIPPED_FRAMES) {
            note("largest step %.2f deg in block %u, %.2f s in",
                 s.phase_step_max_deg, s.worst_phase_block,
                 s.worst_phase_block * (double)EMU_BLOCK / rate);
        }

        snprintf(what, sizeof what, "%s: the two clocks do not drift", kName[c]);
        double ppm = s.phase_slope_deg / (360.0 * bin[c] / EMU_BLOCK)   /* frames/block */
                   / (double)EMU_BLOCK * 1e6;
        verdict(options->split || fabs(ppm) < 20.0, what,
                "%.3f deg/block = %.2f ppm%s", s.phase_slope_deg, ppm,
                options->split ? "   (device against capture clock)" : "");

        snprintf(what, sizeof what, "%s: the level holds", kName[c]);
        verdict(s.level_max_db - s.level_min_db < MAX_LEVEL_SPREAD_DB, what,
                "%.2f dB spread, %.2f to %.2f dBFS",
                s.level_max_db - s.level_min_db, s.level_min_db, s.level_max_db);

        snprintf(what, sizeof what, "%s: no splice in the waveform", kName[c]);
        verdict(s.thdn_worst_db < MAX_THDN_DB, what,
                "worst block %.1f dB, mean %.1f dB (worst at %.2f s)",
                s.thdn_worst_db, s.thdn_mean_db,
                s.worst_thdn_block * (double)EMU_BLOCK / rate);

        note("residual to the previous period peaks at %.5f (%.1f dB) at %.2f s",
             s.discontinuity,
             s.discontinuity > 0 ? 20.0 * log10(s.discontinuity / options->amplitude) : -240.0,
             (double)s.discontinuity_frame / rate);
        note("DC %.1f dBFS, crosstalk %.1f dB, noise+distortion %.1f dB",
             s.dc_worst_db, s.rival_worst_db - s.level_mean_db, s.thdn_mean_db);
    }

    if (options->wav) write_wav(options->wav, capture + analysis_start * CHANNELS, window, rate);

    free(plan); free(capture); free(mono);
    return gFailures - failed_before;
}

/* --- the latency test ----------------------------------------------------- */

static int latency_test(AudioObjectID device, const Options* options)
{
    double rate = get_rate(device);
    if (rate <= 0) return 1;

    unsigned reps = options->reps ? options->reps : 5;
    unsigned chirp_len = (unsigned)(rate * 0.020);
    unsigned gap = (unsigned)(rate * 0.15);

    /*
     * The search runs from where the chirp would arrive with no delay at all.
     * That point is not the plan index: the capture array is indexed by input
     * sample time and the plan by output sample time, and a cycle's output
     * runs ahead of its input by two IO buffers plus both safety offsets --
     * 560 frames at a 64-frame buffer, 4528 at 2048. Anchoring the window on
     * that shift instead of spanning it keeps the window small however large
     * the buffer is, which is what stops the next chirp ever falling inside it.
     */
    const double kSearchBack = 0.005, kSearchAhead = 0.050;   /* seconds */

    uint64_t settle = (uint64_t)(SETTLE_SECONDS * rate) + (uint64_t)(rate * 0.1);
    uint64_t plan_frames = settle + (uint64_t)reps * (chirp_len + gap)
                         + (uint64_t)(TAIL_SECONDS * rate);
    uint64_t capacity = plan_frames + (uint64_t)(rate * 0.5);

    float* plan = calloc(plan_frames * CHANNELS, sizeof(float));
    float* capture = calloc(capacity * CHANNELS, sizeof(float));
    float* mono = calloc(capacity, sizeof(float));
    float* chirp = calloc(chirp_len, sizeof(float));
    if (!plan || !capture || !mono || !chirp) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    double f1 = rate * 0.4 < 8000.0 ? rate * 0.4 : 8000.0;
    emu_gen_chirp(chirp, chirp_len, 200.0 / rate, f1 / rate, options->amplitude * 2.0);

    uint64_t at[64];      /* bounded by the -n option check */
    for (unsigned r = 0; r < reps; r++) {
        at[r] = settle + (uint64_t)r * (chirp_len + gap);
        for (unsigned i = 0; i < chirp_len; i++) {
            plan[(at[r] + i) * CHANNELS + 0] = chirp[i];
            plan[(at[r] + i) * CHANNELS + 1] = chirp[i];
        }
    }

    printf("\n-- latency at %.0f Hz\n", rate);
    printf("   %u chirps of %.1f ms, 200 Hz to %.0f Hz\n\n",
           reps, chirp_len * 1000.0 / rate, f1);

    Loop loop;
    memset(&loop, 0, sizeof loop);
    loop.plan = plan;
    loop.plan_frames = plan_frames;
    loop.capture = capture;
    loop.capture_capacity = capacity;
    loop.play_device = device;
    loop.capture_device = options->split ? options->capture : device;
    loop.split = options->split;

    Diag before, after, final;
    uint64_t analysis_start = 0;
    bool ran = run_stream(device, &loop, rate, &before, &after, &final, &analysis_start);
    uint64_t captured = atomic_load(&loop.captured);

    verdict(ran && captured > 0, "the stream ran and delivered audio",
            "%llu frames captured", (unsigned long long)captured);
    if (!ran || captured == 0) { free(plan); free(capture); free(mono); free(chirp); return 1; }

    report_cycles(&loop, rate);
    report_counters(&before, &after, &final, loop.frames_max);

    /*
     * A frame put into the output buffer at sample time So and found in the
     * input buffer at sample time Si took Si - So to get round the cable. Both
     * are positions on the one timeline the driver publishes, so this is the
     * driver's own arithmetic being checked against a physical delay.
     *
     * The two arrays have different origins -- the first cycle's input and
     * output sample times -- so the difference between those origins goes back
     * in explicitly.
     */
    /* With one device the two index spaces share a clock, so the difference
     * between their origins is the correction that turns a capture offset into
     * a round trip. With two there is no shared base and no such correction:
     * the search starts at the beginning of the capture, over a window wide
     * enough to contain any plausible offset, and the latency figure below
     * becomes a raw offset rather than a round trip. */
    double origin_shift = 0.0;
    uint64_t from;
    unsigned search;
    if (options->split) {
        from = 0;
        search = (unsigned)(1.0 * rate);
    } else {
        origin_shift = loop.out_origin - loop.in_origin;
        uint64_t back = (uint64_t)(kSearchBack * rate);
        if ((double)back > origin_shift) back = (uint64_t)origin_shift;
        from = (uint64_t)(origin_shift + 0.5) - back;
        search = (unsigned)((kSearchBack + kSearchAhead) * rate);
    }

    static const char* const kName[CHANNELS] = { "left", "right" };
    double mean[CHANNELS] = {0}, spread[CHANNELS] = {0};
    int failed_before = gFailures;

    for (unsigned c = 0; c < CHANNELS; c++) {
        deinterleave(capture, captured, c, mono);

        double found[64];
        unsigned n = 0, at_edge = 0;
        double worst_quality = 1.0;
        int polarity = 1;
        for (unsigned r = 0; r < reps; r++) {
            uint64_t start = at[r] + from;
            if (start + search + chirp_len + 1 >= captured) break;
            double quality = 0.0;
            int sign = 0;
            double lag = emu_find_delay(chirp, chirp_len, mono + start,
                                        captured - start, search, &quality, &sign);
            if (lag < 0.0 || quality < 0.5) continue;
            /* A peak against the edge of the window is not a peak: the real one
             * may be outside it. */
            if (lag < 2.0 || lag > (double)search - 2.0) { at_edge++; continue; }
            found[n++] = (double)from + lag - origin_shift;
            if (quality < worst_quality) worst_quality = quality;
            polarity = sign;
        }

        char what[64];
        snprintf(what, sizeof what, "%s: the chirp came back", kName[c]);
        verdict(n == reps, what, "%u of %u found, worst correlation %.4f%s",
                n, reps, n ? worst_quality : 0.0,
                at_edge ? " (some against the window edge)" : "");
        if (n == 0) continue;

        double sum = 0.0, lo = found[0], hi = found[0];
        for (unsigned i = 0; i < n; i++) {
            sum += found[i];
            if (found[i] < lo) lo = found[i];
            if (found[i] > hi) hi = found[i];
        }
        mean[c] = sum / n;
        spread[c] = hi - lo;

        snprintf(what, sizeof what, "%s: the round trip is repeatable", kName[c]);
        verdict(spread[c] < MAX_LATENCY_SPREAD_FRAMES, what,
                "%.2f frames spread over %u chirps", spread[c], n);
        note("%s round trip %.2f frames = %.3f ms%s", kName[c], mean[c],
             mean[c] * 1000.0 / rate, polarity < 0 ? "  (polarity inverted)" : "");
    }

    /*
     * What Core Audio tells an application. A client that lines its input up
     * with its output uses exactly these two numbers, so the difference
     * between them and the cable is the error a client would make.
     */
    UInt32 latency_in = get_u32(device, kAudioDevicePropertyLatency,
                                kAudioObjectPropertyScopeInput);
    UInt32 latency_out = get_u32(device, kAudioDevicePropertyLatency,
                                 kAudioObjectPropertyScopeOutput);
    UInt32 safety_in = get_u32(device, kAudioDevicePropertySafetyOffset,
                               kAudioObjectPropertyScopeInput);
    UInt32 safety_out = get_u32(device, kAudioDevicePropertySafetyOffset,
                                kAudioObjectPropertyScopeOutput);
    UInt32 buffer = get_u32(device, kAudioDevicePropertyBufferFrameSize,
                            kAudioObjectPropertyScopeGlobal);

    double declared = (double)latency_in + (double)latency_out;
    double measured = (mean[0] + mean[1]) / 2.0;
    double error_ms = (measured - declared) * 1000.0 / rate;

    printf("\n");
    note("declared: latency in %u + out %u = %.0f frames (%.3f ms)",
         latency_in, latency_out, declared, declared * 1000.0 / rate);
    note("measured: %.2f frames (%.3f ms) -- error %+.3f ms",
         measured, measured * 1000.0 / rate, error_ms);
    note("safety offsets in %u out %u frames, IO buffer %u frames",
         safety_in, safety_out, buffer);

    verdict(fabs(error_ms) < MAX_LATENCY_ERROR_MS,
            "the declared latency matches the cable",
            "%+.3f ms out, tolerance %.1f ms", error_ms, MAX_LATENCY_ERROR_MS);
    verdict(fabs(mean[0] - mean[1]) < MAX_LATENCY_SPREAD_FRAMES,
            "both channels arrive together",
            "left and right differ by %.2f frames", fabs(mean[0] - mean[1]));

    /* What a client actually waits: it can neither read input before the HAL
     * hands it over nor write output after the offset has passed. */
    double client_ms = (measured + buffer + safety_in + safety_out) * 1000.0 / rate;
    note("a client processing input to output waits about %.2f ms end to end", client_ms);

    free(plan); free(capture); free(mono); free(chirp);
    return gFailures - failed_before;
}

/* --- driving ------------------------------------------------------------- */

static void header(AudioObjectID device)
{
    AudioObjectPropertyAddress address = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFStringRef name = NULL;
    UInt32 size = sizeof name;
    char text[128] = "?";
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &name) == noErr && name) {
        CFStringGetCString(name, text, sizeof text, kCFStringEncodingUTF8);
        CFRelease(name);
    }
    printf("%s, AudioObjectID %u, %.0f Hz, IO buffer %u frames\n", text, device,
           get_rate(device),
           get_u32(device, kAudioDevicePropertyBufferFrameSize,
                   kAudioObjectPropertyScopeGlobal));
    printf("loopback cable from the outputs to the inputs, both channels\n");
}

static void usage(void)
{
    fprintf(stderr,
        "usage: hal-loopback [options] [command]\n"
        "\n"
        "  all                 smoke, latency, glitches                (the default)\n"
        "  smoke               a tone goes out and comes back, on the right channels\n"
        "  glitches            a long tone: level, phase, splices, drift\n"
        "  latency             round trip by chirp, against what the driver declares\n"
        "  sweep               the glitch test at every rate the device offers\n"
        "  selftest            the analysers against synthetic faults; no hardware\n"
        "\n"
        "  -s <seconds>        how long the glitch test runs      (default 10)\n"
        "  -a <dBFS>           playback level                     (default -12)\n"
        "  -r <hz>             set the sample rate first\n"
        "  -b <frames>         set the IO buffer size first\n"
        "  -n <count>          chirps in the latency test         (default 5)\n"
        "  -w <file.wav>       write the captured audio (32-bit float)\n"
        "  -i <name|uid>       listen on another interface rather than this one\n"
        "                      (-i M4). Its clock is its own, so phase and drift\n"
        "                      are then measured rather than judged; splices and\n"
        "                      level still decide the result. Required to measure\n"
        "                      176.4/192 kHz with the input stream disabled.\n"
        "  -P                  do not prime before measuring (see below)\n"
        "  -N <runs>           repeat the glitch test and report how many ran\n"
        "                      clean. One run is not evidence at 176.4/192 kHz.\n"
        "\n"
        "At 176.4 and 192 kHz two throwaway streams run first: a bInterval 3\n"
        "stream leaves the next one corrupt whatever its rate, so a single\n"
        "warm-up run would put the measurement on the corrupt side of that\n"
        "alternation and make unrelated comparisons look significant.\n"
        "\n"
        "Needs the driver installed and a loopback cable. Set the input gain so\n"
        "the returned tone does not clip; the tests report the level they saw.\n");
}

int main(int argc, char** argv)
{
    Options options = { .seconds = 10.0, .amplitude = 0.25, .reps = 5, .runs = 1 };
    double level_db = -12.0;

    int opt;
    const char* capture_name = NULL;
    bool no_prime = false;
    while ((opt = getopt(argc, argv, "s:a:r:b:n:w:i:N:Ph")) != -1) {
        switch (opt) {
            case 's': options.seconds = atof(optarg); break;
            case 'a': level_db = atof(optarg); break;
            case 'r': options.rate = atof(optarg); break;
            case 'b': options.buffer = (unsigned)atoi(optarg); break;
            case 'n': {
                int n = atoi(optarg);
                if (n < 1 || n > 64) {
                    fprintf(stderr, "error: chirps must be between 1 and 64\n");
                    return 2;
                }
                options.reps = (unsigned)n;
                break;
            }
            case 'w': options.wav = optarg; break;
            case 'i': capture_name = optarg; break;
            case 'P': no_prime = true; break;
            case 'N': {
                int n = atoi(optarg);
                if (n < 1 || n > 32) {
                    fprintf(stderr, "error: runs must be between 1 and 32\n");
                    return 2;
                }
                options.runs = (unsigned)n;
                break;
            }
            default: usage(); return 2;
        }
    }
    options.amplitude = pow(10.0, level_db / 20.0);
    if (options.amplitude <= 0.0 || options.amplitude > 1.0) {
        fprintf(stderr, "error: %.1f dBFS is not a usable level\n", level_db);
        return 2;
    }
    if (options.seconds < 1.0) options.seconds = 1.0;

    const char* command = optind < argc ? argv[optind] : "all";

    if (strcmp(command, "selftest") == 0) return emu_analysis_selftest() ? 1 : 0;

    AudioObjectID device = find_device();
    if (!device) {
        fprintf(stderr, "error: no device with UID %s.\n"
                        "       Is the plug-in installed and coreaudiod restarted?\n",
                DEVICE_UID);
        return 1;
    }

    if (options.rate > 0 && !set_rate(device, options.rate)) {
        fprintf(stderr, "error: the device would not go to %.0f Hz\n", options.rate);
        return 1;
    }
    if (options.buffer > 0 && !set_buffer(device, options.buffer)) {
        fprintf(stderr, "error: the device would not take a %u-frame buffer\n", options.buffer);
        return 1;
    }

    if (capture_name) {
        options.capture = find_capture_device(capture_name);
        if (options.capture == kAudioObjectUnknown) {
            fprintf(stderr, "error: no input device matching \"%s\"\n", capture_name);
            return 1;
        }
        if (options.capture == device) {
            fprintf(stderr, "error: that is the device under test; omit -i to loop it back\n");
            return 1;
        }
        options.split = true;

        /* Both ends must agree on the rate: the analysis derives its bin
         * frequencies from one number, and a capture device an octave out
         * would land the tone in the wrong bin and read as silence. */
        double want = options.rate > 0 ? options.rate : get_rate(device);
        if (want > 0 && !set_rate(options.capture, want)) {
            fprintf(stderr, "error: the capture device would not go to %.0f Hz\n", want);
            return 1;
        }
        printf("listening on a separate interface at %.0f Hz;"
               " phase and drift are measured, not judged\n", want);
    }

    /* See kDepthPrime. Skipped for the sweep, which primes each rate itself. */
    if (!no_prime && get_rate(device) >= 176400.0 &&
        strcmp(command, "selftest") != 0 && strcmp(command, "sweep") != 0) {
        printf("priming twice at %.0f Hz: a bInterval 3 stream corrupts the next one,"
               " and one throwaway run would leave this measurement on the corrupt side\n",
               get_rate(device));
        tone_test(device, &options, 1.5, kDepthPrime, NULL);
        tone_test(device, &options, 1.5, kDepthPrime, NULL);
    }

    header(device);

    if (strcmp(command, "smoke") == 0) {
        tone_test(device, &options, 2.0, kDepthSmoke, "smoke");
    } else if (strcmp(command, "glitches") == 0 && options.runs > 1) {
        /*
         * Repeat, and report every run.
         *
         * A single glitch measurement at these rates is not evidence. The
         * poisoning alternates, priming only mostly absorbs it, and the first
         * run after a rate or mode change is unreliable whatever is done -- so
         * one run's verdict can be either the driver's behaviour or the state
         * it inherited, with nothing in the output to say which. Comparing two
         * single runs across that produced a confident wrong conclusion about
         * the input modes during this tool's own development.
         */
        int clean = 0;
        for (unsigned r = 0; r < options.runs; r++) {
            int before_run = gFailures;
            char title[64];
            snprintf(title, sizeof title, "glitches, run %u of %u", r + 1, options.runs);
            tone_test(device, &options, options.seconds, kDepthFull, title);
            if (gFailures == before_run) clean++;
        }
        printf("\n%d of %u runs clean\n", clean, options.runs);
        if (clean != (int)options.runs && clean > 0) {
            printf("  Mixed. At 176.4/192 kHz that is usually the stream-to-stream\n"
                   "  poisoning rather than a difference in what is being measured;\n"
                   "  judge by how many runs are clean, not by any single one.\n");
        }
    } else if (strcmp(command, "glitches") == 0) {
        tone_test(device, &options, options.seconds, kDepthFull, "glitches");
    } else if (strcmp(command, "latency") == 0) {
        latency_test(device, &options);
    } else if (strcmp(command, "sweep") == 0) {
        double rates[16];
        unsigned n = available_rates(device, rates, 16);
        double original = get_rate(device);
        for (unsigned i = 0; i < n; i++) {
            if (!set_rate(device, rates[i])) {
                verdict(false, "the device took the rate", "%.0f Hz refused", rates[i]);
                continue;
            }
            /*
             * Two streams per rate, and the first one is a test in its own
             * right: on this hardware the stream that follows a rate change
             * comes back wrong -- clipped, and carrying the other channel --
             * while every driver counter stays zero, and the next stream at
             * the same rate is clean. Waiting does not help, so it is state
             * consumed at the first StartIO after the change rather than the
             * device settling. Measuring the rate itself needs that stream out
             * of the way; noticing it needs it reported.
             */
            tone_test(device, &options, 1.5, kDepthSmoke, "the stream after the rate change");
            tone_test(device, &options, options.seconds, kDepthFull, "glitches");
        }
        set_rate(device, original);
    } else if (strcmp(command, "all") == 0) {
        tone_test(device, &options, 2.0, kDepthSmoke, "smoke");
        latency_test(device, &options);
        tone_test(device, &options, options.seconds, kDepthFull, "glitches");
    } else {
        usage();
        return 2;
    }

    printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
