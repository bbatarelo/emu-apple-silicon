/*
 * Traces the driver's diagnostics counters through a playback start.
 *
 * Starts a silent output IOProc on the device -- the same StartIO path any
 * player app takes -- then polls the diagnostics property every few
 * milliseconds and prints a timeline: IO cycles, frames moved, ring depth,
 * underruns. This is the instrument that pinned down the startup crackle: a
 * one-line-per-sample view makes a 50 ms stall in coreaudiod's IO cycles, or a
 * burst of ring underruns, impossible to miss and easy to time.
 *
 * Silence in, so it is inaudible on connected monitors; the counters do not
 * care what the samples are.
 */

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_UID "net.quantum-bit.EMUTrackerPre"

#define TRACE_SECONDS   3.0
#define POLL_USECS      4000
#define MAX_SAMPLES     2000

static AudioObjectID find_device(void)
{
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != noErr) {
        return 0;
    }
    UInt32 count = size / sizeof(AudioObjectID);
    AudioObjectID* devices = malloc(size);
    if (!devices) return 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, devices) != noErr) {
        free(devices);
        return 0;
    }
    AudioObjectPropertyAddress uidAddress = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    AudioObjectID found = 0;
    for (UInt32 i = 0; i < count && !found; i++) {
        CFStringRef uid = NULL;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(devices[i], &uidAddress, 0, NULL, &uidSize, &uid) != noErr || !uid) {
            continue;
        }
/* Devices are named by the plug-in's prefix plus the unit's serial, so the
         * match is on the prefix. EMU_DEVICE=<text> picks among them when more
         * than one is attached; without it the first found wins. */
        Boolean match = CFStringHasPrefix(uid, CFSTR(DEVICE_UID));
        const char* want = getenv("EMU_DEVICE");
        if (match && want && *want) {
            CFStringRef w = CFStringCreateWithCString(NULL, want, kCFStringEncodingUTF8);
            if (w) { match = CFStringFind(uid, w, 0).location != kCFNotFound; CFRelease(w); }
        }
        if (match) {
            found = devices[i];
        }
        CFRelease(uid);
    }
    free(devices);
    return found;
}

typedef struct {
    uint64_t ioCycles, framesToOutput, framesPlayed, outputLead, outputUnderruns;
    uint64_t inputUnderruns, tsFallbacks, tsResets, usbErrors, engineRunning;
    uint64_t resyncs, deadFrames, unfilledPlayback;
    uint64_t framesBound, unmappedFrames, unmappedAhead, bindRaces;
} Diag;

static bool read_diag(AudioObjectID device, Diag* out)
{
    AudioObjectPropertyAddress address = {
        'emuD', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    CFPropertyListRef plist = NULL;
    UInt32 size = sizeof(plist);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &plist) != noErr || !plist) {
        return false;
    }
    if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) { CFRelease(plist); return false; }
    CFDictionaryRef d = (CFDictionaryRef)plist;

    struct { const char* key; uint64_t* dst; } fields[] = {
        { "ioCycles", &out->ioCycles },
        { "framesToOutput", &out->framesToOutput },
        { "framesPlayed", &out->framesPlayed },
        { "outputLead", &out->outputLead },
        { "outputUnderruns", &out->outputUnderruns },
        { "inputUnderruns", &out->inputUnderruns },
        { "tsFallbacks", &out->tsFallbacks },
        { "tsResets", &out->tsResets },
        { "usbErrors", &out->usbErrors },
        { "engineRunning", &out->engineRunning },
        { "resyncs", &out->resyncs },
        { "deadFrames", &out->deadFrames },
        { "unfilledPlayback", &out->unfilledPlayback },
        { "framesBound", &out->framesBound },
        { "unmappedFrames", &out->unmappedFrames },
        { "unmappedAhead", &out->unmappedAhead },
        { "bindRaces", &out->bindRaces },
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        CFStringRef key = CFStringCreateWithCString(NULL, fields[i].key, kCFStringEncodingUTF8);
        long long v = 0;
        if (key) {
            CFNumberRef n = CFDictionaryGetValue(d, key);
            if (n) CFNumberGetValue(n, kCFNumberLongLongType, &v);
            CFRelease(key);
        }
        *fields[i].dst = (uint64_t)v;
    }
    CFRelease(plist);
    return true;
}

static OSStatus silence_proc(AudioObjectID device, const AudioTimeStamp* now,
                             const AudioBufferList* inputData, const AudioTimeStamp* inputTime,
                             AudioBufferList* outputData, const AudioTimeStamp* outputTime,
                             void* clientData)
{
    (void)device; (void)now; (void)inputData; (void)inputTime; (void)outputTime; (void)clientData;
    if (outputData) {
        for (UInt32 i = 0; i < outputData->mNumberBuffers; i++) {
            if (outputData->mBuffers[i].mData) {
                memset(outputData->mBuffers[i].mData, 0, outputData->mBuffers[i].mDataByteSize);
            }
        }
    }
    return noErr;
}

int main(void)
{
    AudioObjectID device = find_device();
    if (!device) {
        fprintf(stderr, "no device with UID %s -- is the driver installed?\n", DEVICE_UID);
        return 1;
    }

    Diag before;
    if (!read_diag(device, &before)) {
        fprintf(stderr, "diagnostics unreadable\n");
        return 1;
    }

    AudioDeviceIOProcID procID = NULL;
    if (AudioDeviceCreateIOProcID(device, silence_proc, NULL, &procID) != noErr || !procID) {
        fprintf(stderr, "CreateIOProcID failed\n");
        return 1;
    }

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t t0 = mach_absolute_time();

    if (AudioDeviceStart(device, procID) != noErr) {
        fprintf(stderr, "AudioDeviceStart failed\n");
        AudioDeviceDestroyIOProcID(device, procID);
        return 1;
    }

    static double t_ms[MAX_SAMPLES];
    static Diag samples[MAX_SAMPLES];
    int n = 0;

    while (n < MAX_SAMPLES) {
        double ms = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
        if (ms > TRACE_SECONDS * 1000.0) break;
        if (read_diag(device, &samples[n])) {
            t_ms[n] = ms;
            n++;
        }
        usleep(POLL_USECS);
    }

    AudioDeviceStop(device, procID);
    AudioDeviceDestroyIOProcID(device, procID);

    /* Counter columns are deltas against the pre-start baseline, so a fresh
     * stream reads from zero whatever earlier sessions left behind. */
    printf("#   t_ms  engine  ioCycles  framesOut  framesBound  framesPlayed  outLead"
           "  underrun  unmapped  ahead  races  inUnder  tsFall  tsReset  resync  dead"
           "  unfill  usbErr\n");
    Diag prev;
    memset(&prev, 0xff, sizeof prev);
    for (int i = 0; i < n; i++) {
        Diag* s = &samples[i];
        bool interesting =
            i == 0 || i == n - 1 ||
            s->outputUnderruns != prev.outputUnderruns ||
            s->unmappedFrames != prev.unmappedFrames ||
            s->bindRaces != prev.bindRaces ||
            s->inputUnderruns != prev.inputUnderruns ||
            s->tsResets != prev.tsResets ||
            s->resyncs != prev.resyncs ||
            s->deadFrames != prev.deadFrames ||
            s->unfilledPlayback != prev.unfilledPlayback ||
            s->engineRunning != prev.engineRunning ||
            (i % 10 == 0);
        if (interesting) {
            printf("%8.1f  %6llu  %8llu  %9llu  %11llu  %12llu  %7llu  %8llu"
                   "  %8llu  %5llu  %5llu  %7llu  %6llu  %7llu  %6llu  %4llu"
                   "  %6llu  %6llu\n",
                   t_ms[i],
                   (unsigned long long)s->engineRunning,
                   (unsigned long long)(s->ioCycles - before.ioCycles),
                   (unsigned long long)(s->framesToOutput - before.framesToOutput),
                   (unsigned long long)(s->framesBound - before.framesBound),
                   (unsigned long long)s->framesPlayed,
                   (unsigned long long)s->outputLead,
                   (unsigned long long)s->outputUnderruns,
                   (unsigned long long)s->unmappedFrames,
                   (unsigned long long)s->unmappedAhead,
                   (unsigned long long)s->bindRaces,
                   (unsigned long long)s->inputUnderruns,
                   (unsigned long long)s->tsFallbacks,
                   (unsigned long long)s->tsResets,
                   (unsigned long long)s->resyncs,
                   (unsigned long long)s->deadFrames,
                   (unsigned long long)s->unfilledPlayback,
                   (unsigned long long)s->usbErrors);
        }
        prev = *s;
    }
    return 0;
}
