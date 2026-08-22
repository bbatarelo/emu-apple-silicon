/*
 * Queries the Tracker Pre HAL device through Core Audio, as an application
 * sees it.
 *
 * The plug-in's own logging cannot show whether the property surface is
 * correct, only what it was asked for. This asks from the client side, which is
 * the view that actually matters.
 */

#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>

#define DEVICE_UID "net.quantum-bit.EMUTrackerPre"

static AudioObjectID find_device(void)
{
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address,
                                       0, NULL, &size) != noErr) return kAudioObjectUnknown;

    UInt32 count = size / sizeof(AudioObjectID);
    AudioObjectID devices[64];
    if (count > 64) count = 64;
    size = count * sizeof(AudioObjectID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                   0, NULL, &size, devices) != noErr) {
        return kAudioObjectUnknown;
    }

    for (UInt32 i = 0; i < count; i++) {
        AudioObjectPropertyAddress uidAddress = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef uid = NULL;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(devices[i], &uidAddress, 0, NULL,
                                       &uidSize, &uid) != noErr || !uid) continue;

        Boolean match = CFStringCompare(uid, CFSTR(DEVICE_UID), 0) == kCFCompareEqualTo;
        CFRelease(uid);
        if (match) return devices[i];
    }
    return kAudioObjectUnknown;
}

static UInt32 get_u32(AudioObjectID device, AudioObjectPropertySelector selector,
                      AudioObjectPropertyScope scope, OSStatus* status)
{
    AudioObjectPropertyAddress address = { selector, scope, kAudioObjectPropertyElementMain };
    UInt32 value = 0, size = sizeof(value);
    OSStatus s = AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &value);
    if (status) *status = s;
    return value;
}

static UInt32 channels_in_scope(AudioObjectID device, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, NULL, &size) != noErr) return 0;

    AudioBufferList* list = malloc(size);
    if (!list) return 0;

    UInt32 total = 0;
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, list) == noErr) {
        for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
            total += list->mBuffers[i].mNumberChannels;
        }
    }
    free(list);
    return total;
}

/* Reads or writes the driver's clock-source setting. Returns 0 on success. */
static int clock_source(AudioObjectID device, const char* wanted)
{
    AudioObjectPropertyAddress address = {
        'emuK', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };

    if (wanted) {
        CFStringRef value = CFStringCreateWithCString(NULL, wanted, kCFStringEncodingUTF8);
        if (!value) return 1;
        OSStatus s = AudioObjectSetPropertyData(device, &address, 0, NULL,
                                                sizeof(value), &value);
        CFRelease(value);
        if (s != noErr) {
            fprintf(stderr, "error: could not set clock source to '%s' (0x%x)\n", wanted, s);
            fprintf(stderr, "       valid values are 'device' and 'host'\n");
            return 1;
        }
    }

    CFStringRef current = NULL;
    UInt32 size = sizeof(current);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &current) != noErr
        || !current) {
        fprintf(stderr, "error: could not read the clock source\n");
        return 1;
    }
    char buffer[32] = {0};
    CFStringGetCString(current, buffer, sizeof buffer, kCFStringEncodingUTF8);
    CFRelease(current);

    printf("clock source: %s\n", buffer);
    if (strcmp(buffer, "host") == 0) {
        printf("  smooth, but Core Audio and the device drift apart over hours\n");
    } else {
        printf("  correct long-term; the anchor moves in 8 ms steps\n");
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: hal-check                       report what the driver is doing\n"
        "       hal-check clock                 show which clock the timeline follows\n"
        "       hal-check clock device|host     switch it, immediately\n");
}

int main(int argc, char** argv)
{
    AudioObjectID device = find_device();
    if (device == kAudioObjectUnknown) {
        fprintf(stderr, "error: device %s not found.\n"
                        "       Is the plug-in installed and coreaudiod restarted?\n",
                DEVICE_UID);
        return 1;
    }
    if (argc > 1) {
        if (strcmp(argv[1], "clock") == 0) {
            return clock_source(device, argc > 2 ? argv[2] : NULL);
        }
        usage();
        return 2;
    }

    printf("device found: AudioObjectID %u\n\n", device);

    AudioObjectPropertyAddress address = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFStringRef name = NULL;
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &name) == noErr && name) {
        char buffer[256];
        CFStringGetCString(name, buffer, sizeof buffer, kCFStringEncodingUTF8);
        printf("  name              %s\n", buffer);
        CFRelease(name);
    }

    printf("  input channels    %u\n", channels_in_scope(device, kAudioObjectPropertyScopeInput));
    printf("  output channels   %u\n", channels_in_scope(device, kAudioObjectPropertyScopeOutput));

    AudioObjectPropertyAddress rateAddress = {
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    Float64 rate = 0;
    size = sizeof(rate);
    if (AudioObjectGetPropertyData(device, &rateAddress, 0, NULL, &size, &rate) == noErr) {
        printf("  sample rate       %.0f Hz\n", rate);
    }

    OSStatus status = noErr;
    UInt32 running = get_u32(device, kAudioDevicePropertyDeviceIsRunning,
                             kAudioObjectPropertyScopeGlobal, &status);
    printf("  IO running        %s%s\n", running ? "yes" : "no",
           status == noErr ? "" : "  (property failed)");

    printf("  alive             %u\n", get_u32(device, kAudioDevicePropertyDeviceIsAlive,
                                               kAudioObjectPropertyScopeGlobal, NULL));
    printf("  transport         %.4s\n", (char*)&(UInt32){
        CFSwapInt32HostToBig(get_u32(device, kAudioDevicePropertyTransportType,
                                     kAudioObjectPropertyScopeGlobal, NULL)) });
    printf("  safety offset in  %u frames\n",
           get_u32(device, kAudioDevicePropertySafetyOffset,
                   kAudioObjectPropertyScopeInput, NULL));
    printf("  safety offset out %u frames\n",
           get_u32(device, kAudioDevicePropertySafetyOffset,
                   kAudioObjectPropertyScopeOutput, NULL));
    printf("  latency in        %u frames\n",
           get_u32(device, kAudioDevicePropertyLatency,
                   kAudioObjectPropertyScopeInput, NULL));
    printf("  latency out       %u frames\n",
           get_u32(device, kAudioDevicePropertyLatency,
                   kAudioObjectPropertyScopeOutput, NULL));

    /* Volume and mute, as System Settings sees them. */
    AudioObjectPropertyAddress volAddress = {
        kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    Float32 volume = 0;
    UInt32 volSize = sizeof(volume);
    OSStatus volStatus = AudioObjectGetPropertyData(device, &volAddress, 0, NULL,
                                                    &volSize, &volume);
    if (volStatus == noErr) {
        Float32 db = 0;
        UInt32 dbSize = sizeof(db);
        AudioObjectPropertyAddress dbAddress = {
            kAudioDevicePropertyVolumeDecibels, kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(device, &dbAddress, 0, NULL, &dbSize, &db) == noErr) {
            printf("  output volume     %.3f  (%.1f dB)\n", volume, db);
        } else {
            printf("  output volume     %.3f\n", volume);
        }
    } else {
        printf("  output volume     unavailable (0x%x)\n", volStatus);
    }

    AudioObjectPropertyAddress muteAddress = {
        kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    UInt32 muted = 0, muteSize = sizeof(muted);
    if (AudioObjectGetPropertyData(device, &muteAddress, 0, NULL, &muteSize, &muted) == noErr) {
        printf("  output mute       %s\n", muted ? "on" : "off");
    }

    /* The plug-in's diagnostic counters, carried as one declared custom
     * property. These say what the driver has actually done, independently of
     * whether log queries happen to work. */
    AudioObjectPropertyAddress diagAddress = {
        'emuD', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    CFPropertyListRef diagnostics = NULL;
    UInt32 diagSize = sizeof(diagnostics);
    OSStatus diagStatus = AudioObjectGetPropertyData(device, &diagAddress, 0, NULL,
                                                     &diagSize, &diagnostics);
    printf("\n");
    if (diagStatus == noErr && diagnostics &&
        CFGetTypeID(diagnostics) == CFDictionaryGetTypeID()) {
        /* Enumerate whatever the driver reports rather than a list kept here.
         * A hardcoded list silently hid seven counters the moment the driver
         * started publishing them. */
        CFDictionaryRef dict = (CFDictionaryRef)diagnostics;
        CFIndex count = CFDictionaryGetCount(dict);
        const void** keys = calloc((size_t)count, sizeof(void*));
        const void** values = calloc((size_t)count, sizeof(void*));
        if (keys && values) {
            CFDictionaryGetKeysAndValues(dict, keys, values);
            for (CFIndex i = 0; i < count; i++) {
                char name[64] = {0};
                CFStringGetCString((CFStringRef)keys[i], name, sizeof name, kCFStringEncodingUTF8);
                long long value = 0;
                CFNumberGetValue((CFNumberRef)values[i], kCFNumberLongLongType, &value);
                printf("  %-18s %lld\n", name, value);
            }
        }
        free(keys);
        free(values);
    } else {
        printf("  diagnostics unavailable (0x%x)\n", diagStatus);
    }
    if (diagnostics) CFRelease(diagnostics);
    printf("\n");

    AudioObjectPropertyAddress ratesAddress = {
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyDataSize(device, &ratesAddress, 0, NULL, &size) == noErr) {
        UInt32 n = size / sizeof(AudioValueRange);
        AudioValueRange ranges[32];
        if (n > 32) n = 32;
        size = n * sizeof(AudioValueRange);
        if (AudioObjectGetPropertyData(device, &ratesAddress, 0, NULL, &size, ranges) == noErr) {
            printf("  available rates   ");
            for (UInt32 i = 0; i < n; i++) printf("%.0f ", ranges[i].mMinimum);
            printf("\n");
        }
    }

    return 0;
}
