/* Installed-driver smoke test for issue #7. Streams only zeros. Reuse
 * hal-check's UID selection and custom-property access without changing the
 * system default device. This measures binding, not analog audio quality. */
#define main hal_check_unused_main
#include "../../tools/hal-check/main.c"
#undef main
#include <stdatomic.h>
#include <unistd.h>

static _Atomic unsigned cycles;

static OSStatus silent(AudioDeviceID device, const AudioTimeStamp* now,
                       const AudioBufferList* input, const AudioTimeStamp* input_time,
                       AudioBufferList* output, const AudioTimeStamp* output_time,
                       void* context)
{
    (void)device; (void)now; (void)input; (void)input_time;
    (void)output_time; (void)context;
    for (UInt32 i = 0; i < output->mNumberBuffers; i++) {
        if (output->mBuffers[i].mData)
            memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
    }
    atomic_fetch_add_explicit(&cycles, 1, memory_order_relaxed);
    return noErr;
}

static CFDictionaryRef stats(AudioObjectID device)
{
    AudioObjectPropertyAddress address = {
        'emuD', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    CFPropertyListRef value = NULL;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &value) || !value)
        return NULL;
    if (CFGetTypeID(value) != CFDictionaryGetTypeID()) {
        CFRelease(value);
        return NULL;
    }
    return value;
}

static long long val(CFDictionaryRef dict, CFStringRef key)
{
    long long number = -1;
    if (!dict) return number;
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(value, kCFNumberLongLongType, &number);
    return number;
}

static bool wait_idle(AudioObjectID device)
{
    for (unsigned i = 0; i < 40; i++) {
        CFDictionaryRef dict = stats(device);
        bool idle = dict && val(dict, CFSTR("engineStreaming")) == 0;
        if (dict) CFRelease(dict);
        if (idle) return true;
        usleep(50000);
    }
    return false;
}

int main(int argc, char** argv)
{
    bool inject = argc == 2 && strcmp(argv[1], "--startup-stale") == 0;
    if (argc > 1 && !inject) {
        fprintf(stderr, "usage: hal-restart-test [--startup-stale]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    AudioObjectID device = find_device();
    if (device == kAudioObjectUnknown) {
        fprintf(stderr, "No matching E-MU device (check EMU_DEVICE).\n");
        return 2;
    }
    if (!wait_idle(device)) {
        fprintf(stderr, "Need an idle E-MU device; stop other playback/recording first.\n");
        return 2;
    }
    print_version(device);
    printf("Silent restart test: device %u, 20 cycles, startup fault %s\n",
           device, inject ? "on" : "off");
    unsigned failures = 0;
    for (unsigned i = 0; i < 20; i++) {
        AudioDeviceIOProcID proc = NULL;
        OSStatus error = AudioDeviceCreateIOProcID(device, silent, NULL, &proc);
        if (error) {
            fprintf(stderr, "create failed: %d\n", (int)error);
            return 2;
        }
        if (inject && fault_inject(device, "startup-stale") != 0) {
            AudioDeviceDestroyIOProcID(device, proc);
            return 2;
        }
        atomic_store(&cycles, 0);
        error = AudioDeviceStart(device, proc);
        if (error) {
            fprintf(stderr, "start failed: %d\n", (int)error);
            if (inject) fault_inject(device, "none");
            AudioDeviceDestroyIOProcID(device, proc);
            return 1;
        }
        usleep(400000);
        CFDictionaryRef before = stats(device);
        usleep(600000);
        CFDictionaryRef after = stats(device);
        long long written = val(after, CFSTR("framesToOutput")) - val(before, CFSTR("framesToOutput"));
        long long bound = val(after, CFSTR("framesBound")) - val(before, CFSTR("framesBound"));
        long long unmapped = val(after, CFSTR("unmappedFrames")) - val(before, CFSTR("unmappedFrames"));
        bool ok = before && after && written > 0 && bound == written && unmapped == 0 &&
                  val(after, CFSTR("engineStreaming")) == 1 &&
                  (!inject || val(after, CFSTR("faultInjected")) == 0);
        printf("%02u %s callbacks=%u written=%lld bound=%lld unmapped=%lld startIO=%lld\n",
               i + 1, ok ? "PASS" : "FAIL", atomic_load(&cycles), written, bound,
               unmapped, val(after, CFSTR("startIOCalls")));
        if (!ok) failures++;
        if (before) CFRelease(before);
        if (after) CFRelease(after);
        error = AudioDeviceStop(device, proc);
        AudioDeviceDestroyIOProcID(device, proc);
        if (error) {
            fprintf(stderr, "stop failed: %d\n", (int)error);
            return 2;
        }
        if (!wait_idle(device)) {
            fprintf(stderr, "Stream remains active; another client may be using it.\n");
            return 2;
        }
        usleep((i % 4) * 30000);
    }
    printf("%u failures out of 20 silent restarts\n", failures);
    return failures ? 1 : 0;
}
