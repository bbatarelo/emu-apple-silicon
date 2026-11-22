/* The hardware tests' own stream, on one named unit.
 *
 * The recovery test needs a stream to break, and used to ask for one: select
 * the device as the system output and play something. That changes the
 * machine's default device, and with two units attached it is easy to play to
 * the wrong one. This opens a duplex IOProc on the unit itself, by UID, and
 * plays silence -- no default changes, no other client is touched, and what
 * it does change (rate, buffer size, when asked) it puts back.
 *
 *     hal-stream --uid                 print the selected unit's UID and exit
 *     hal-stream --list                print every attached unit's UID
 *     hal-stream [--rate HZ] [--buffer FRAMES] [--seconds S]
 *
 * The unit is the one EMU_DEVICE names (any part of its UID or serial). With
 * more than one attached and no EMU_DEVICE, or one that matches several,
 * nothing is selected: a test that picks a unit for you has tested the wrong
 * one half the time.
 *
 * Streams until SIGTERM/SIGINT, or for --seconds. Prints "streaming" on
 * stdout once IO has started, which is what a script waits for.
 */
#include <CoreAudio/CoreAudio.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_UID "net.batarelo.EMUTrackerPre"
#define GLOBAL(sel) { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }

static volatile sig_atomic_t gQuit;
static void quit(int signal) { (void)signal; gQuit = 1; }

static bool uid_of(AudioObjectID device, char* out, size_t size)
{
    AudioObjectPropertyAddress address = GLOBAL(kAudioDevicePropertyDeviceUID);
    CFStringRef uid = NULL;
    UInt32 bytes = sizeof uid;
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &bytes, &uid) != noErr || !uid) return false;
    bool ok = CFStringGetCString(uid, out, (CFIndex)size, kCFStringEncodingUTF8);
    CFRelease(uid);
    return ok;
}

/* Every attached unit of this driver that EMU_DEVICE (if set) matches. */
static unsigned matching(AudioObjectID* out, unsigned capacity, bool all)
{
    AudioObjectPropertyAddress address = GLOBAL(kAudioHardwarePropertyDevices);
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &bytes)) return 0;
    AudioObjectID devices[128];
    if (bytes > sizeof devices) bytes = sizeof devices;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &bytes, devices)) return 0;

    const char* want = all ? NULL : getenv("EMU_DEVICE");
    unsigned n = 0;
    for (UInt32 i = 0; i < bytes / sizeof(AudioObjectID); i++) {
        char uid[256];
        if (!uid_of(devices[i], uid, sizeof uid)) continue;
        if (strncmp(uid, DEVICE_UID, strlen(DEVICE_UID)) != 0) continue;
        if (want && *want && !strcasestr(uid, want)) continue;
        if (n < capacity) out[n] = devices[i];
        n++;
    }
    return n;
}

static AudioObjectID select_unit(void)
{
    AudioObjectID found[8];
    unsigned n = matching(found, 8, false);
    const char* want = getenv("EMU_DEVICE");
    if (n == 1) return found[0];
    if (n == 0) {
        fprintf(stderr, "hal-stream: no attached E-MU unit%s%s\n",
                want && *want ? " matches EMU_DEVICE=" : "", want && *want ? want : "");
    } else {
        fprintf(stderr, "hal-stream: %u units %s; name one with EMU_DEVICE=<UID or part of it>:\n", n,
                want && *want ? "match that" : "are attached");
        for (unsigned i = 0; i < n && i < 8; i++) {
            char uid[256];
            if (uid_of(found[i], uid, sizeof uid)) fprintf(stderr, "  %s\n", uid);
        }
    }
    return kAudioObjectUnknown;
}

static OSStatus silent(AudioDeviceID device, const AudioTimeStamp* now,
                       const AudioBufferList* input, const AudioTimeStamp* input_time,
                       AudioBufferList* output, const AudioTimeStamp* output_time, void* context)
{
    (void)device; (void)now; (void)input; (void)input_time; (void)output_time; (void)context;
    for (UInt32 i = 0; i < output->mNumberBuffers; i++) {
        if (output->mBuffers[i].mData)
            memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
    }
    return noErr;
}

static bool get_rate(AudioObjectID device, Float64* rate)
{
    AudioObjectPropertyAddress address = GLOBAL(kAudioDevicePropertyNominalSampleRate);
    UInt32 bytes = sizeof *rate;
    return AudioObjectGetPropertyData(device, &address, 0, NULL, &bytes, rate) == noErr;
}

/* Asks, then waits for the device to say so: a rate change is performed by
 * the driver some time after the request is accepted. */
static bool set_rate(AudioObjectID device, Float64 rate)
{
    AudioObjectPropertyAddress address = GLOBAL(kAudioDevicePropertyNominalSampleRate);
    if (AudioObjectSetPropertyData(device, &address, 0, NULL, sizeof rate, &rate) != noErr) return false;
    for (int i = 0; i < 100; i++) {
        Float64 now = 0;
        if (get_rate(device, &now) && now == rate) return true;
        usleep(50000);
    }
    return false;
}

static bool get_buffer(AudioObjectID device, UInt32* frames)
{
    AudioObjectPropertyAddress address = GLOBAL(kAudioDevicePropertyBufferFrameSize);
    UInt32 bytes = sizeof *frames;
    return AudioObjectGetPropertyData(device, &address, 0, NULL, &bytes, frames) == noErr;
}

static bool set_buffer(AudioObjectID device, UInt32 frames)
{
    AudioObjectPropertyAddress address = GLOBAL(kAudioDevicePropertyBufferFrameSize);
    return AudioObjectSetPropertyData(device, &address, 0, NULL, sizeof frames, &frames) == noErr;
}

int main(int argc, char** argv)
{
    Float64 want_rate = 0;
    UInt32  want_buffer = 0;
    double  seconds = 0;
    bool    only_uid = false, list = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uid") == 0) only_uid = true;
        else if (strcmp(argv[i], "--list") == 0) list = true;
        else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) want_rate = atof(argv[++i]);
        else if (strcmp(argv[i], "--buffer") == 0 && i + 1 < argc) want_buffer = (UInt32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atof(argv[++i]);
        else {
            fprintf(stderr, "usage: hal-stream [--uid | --list] [--rate HZ] [--buffer FRAMES] [--seconds S]\n");
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (list) {
        AudioObjectID found[8];
        unsigned n = matching(found, 8, true);
        for (unsigned i = 0; i < n && i < 8; i++) {
            char uid[256];
            if (uid_of(found[i], uid, sizeof uid)) printf("%s\n", uid);
        }
        return n ? 0 : 1;
    }

    AudioObjectID device = select_unit();
    if (device == kAudioObjectUnknown) return 2;
    char uid[256] = "";
    uid_of(device, uid, sizeof uid);
    if (only_uid) { printf("%s\n", uid); return 0; }

    signal(SIGTERM, quit);
    signal(SIGINT, quit);
    signal(SIGHUP, quit);

    Float64 old_rate = 0;
    UInt32  old_buffer = 0;
    bool rate_changed = false, buffer_changed = false;
    int status = 0;

    if (want_rate > 0 && get_rate(device, &old_rate) && old_rate != want_rate) {
        if (!set_rate(device, want_rate)) {
            fprintf(stderr, "hal-stream: %s did not accept %.0f Hz\n", uid, want_rate);
            return 1;
        }
        rate_changed = true;
    }
    if (want_buffer > 0 && get_buffer(device, &old_buffer) && old_buffer != want_buffer) {
        if (!set_buffer(device, want_buffer)) {
            fprintf(stderr, "hal-stream: %s did not accept a %u-frame buffer\n", uid, (unsigned)want_buffer);
            status = 1;
            goto restore;
        }
        buffer_changed = true;
    }

    AudioDeviceIOProcID proc = NULL;
    if (AudioDeviceCreateIOProcID(device, silent, NULL, &proc) != noErr) {
        fprintf(stderr, "hal-stream: could not create an IOProc on %s\n", uid);
        status = 1;
        goto restore;
    }
    OSStatus error = AudioDeviceStart(device, proc);
    if (error != noErr) {
        fprintf(stderr, "hal-stream: %s did not start (%d)\n", uid, (int)error);
        AudioDeviceDestroyIOProcID(device, proc);
        status = 1;
        goto restore;
    }
    {
        Float64 rate = 0;
        UInt32 buffer = 0;
        get_rate(device, &rate);
        get_buffer(device, &buffer);
        printf("streaming %s %.0f Hz %u frames\n", uid, rate, (unsigned)buffer);
    }

    for (double waited = 0; !gQuit && (seconds <= 0 || waited < seconds); waited += 0.05) usleep(50000);

    /* A device the engine gave up on has already stopped IO; the error then
     * is the expected one and not this tool's to report. */
    AudioDeviceStop(device, proc);
    AudioDeviceDestroyIOProcID(device, proc);

restore:
    if (buffer_changed && !set_buffer(device, old_buffer)) {
        fprintf(stderr, "hal-stream: could not restore the %u-frame buffer on %s\n", (unsigned)old_buffer, uid);
        status = 1;
    }
    if (rate_changed && !set_rate(device, old_rate)) {
        fprintf(stderr, "hal-stream: could not restore %.0f Hz on %s\n", old_rate, uid);
        status = 1;
    }
    return status;
}
