/* Keeps the system's sound selections across a driver install.
 *
 * Installing restarts coreaudiod, and while the E-MU devices are gone the
 * system moves any default that pointed at one of them to something else --
 * and does not move it back. This records the three selections by device
 * UID beforehand and puts back the ones that changed, once their devices
 * have returned.
 *
 *     hal-defaults show
 *     hal-defaults save <file>
 *     hal-defaults restore <file>     waits up to 20 s for the devices
 *
 * The three are the default input, the default output and the device for
 * sound effects and alerts. Nothing else is touched, and a selection that
 * already matches is left alone.
 */
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const struct { const char* name; AudioObjectPropertySelector selector; } kRoles[] = {
    { "input",   kAudioHardwarePropertyDefaultInputDevice },
    { "output",  kAudioHardwarePropertyDefaultOutputDevice },
    { "effects", kAudioHardwarePropertyDefaultSystemOutputDevice },
};
#define ROLES (sizeof kRoles / sizeof kRoles[0])

static bool uid_of(AudioObjectID device, char* out, size_t size)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    CFStringRef uid = NULL;
    UInt32 bytes = sizeof uid;
    if (device == kAudioObjectUnknown ||
        AudioObjectGetPropertyData(device, &a, 0, NULL, &bytes, &uid) != noErr || !uid) return false;
    bool ok = CFStringGetCString(uid, out, (CFIndex)size, kCFStringEncodingUTF8);
    CFRelease(uid);
    return ok;
}

static bool current(AudioObjectPropertySelector selector, char* out, size_t size)
{
    AudioObjectPropertyAddress a = { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 bytes = sizeof device;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &bytes, &device) != noErr) return false;
    return uid_of(device, out, size);
}

static AudioObjectID device_for_uid(const char* wanted)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    AudioObjectID devices[128];
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &bytes)) return kAudioObjectUnknown;
    if (bytes > sizeof devices) bytes = sizeof devices;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &bytes, devices)) return kAudioObjectUnknown;
    for (UInt32 i = 0; i < bytes / sizeof devices[0]; i++) {
        char uid[256];
        if (uid_of(devices[i], uid, sizeof uid) && strcmp(uid, wanted) == 0) return devices[i];
    }
    return kAudioObjectUnknown;
}

int main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        for (size_t i = 0; i < ROLES; i++) {
            char uid[256] = "";
            current(kRoles[i].selector, uid, sizeof uid);
            printf("%s\t%s\n", kRoles[i].name, uid);
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "save") == 0) {
        FILE* f = fopen(argv[2], "w");
        if (!f) { perror(argv[2]); return 1; }
        for (size_t i = 0; i < ROLES; i++) {
            char uid[256] = "";
            current(kRoles[i].selector, uid, sizeof uid);
            fprintf(f, "%s\t%s\n", kRoles[i].name, uid);
            printf("  %-8s %s\n", kRoles[i].name, uid);
        }
        return fclose(f) == 0 ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "restore") == 0) {
        FILE* f = fopen(argv[2], "r");
        if (!f) { perror(argv[2]); return 1; }
        char line[400];
        int status = 0;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            char* tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            const char* wanted = tab + 1;
            size_t role = 0;
            while (role < ROLES && strcmp(kRoles[role].name, line) != 0) role++;
            if (role == ROLES || !*wanted) continue;

            AudioObjectID device = kAudioObjectUnknown;
            for (int i = 0; i < 200 && device == kAudioObjectUnknown; i++) {
                device = device_for_uid(wanted);
                if (device == kAudioObjectUnknown) usleep(100000);
            }
            if (device == kAudioObjectUnknown) {
                fprintf(stderr, "  %-8s %s did not come back; the selection was left as it is\n", line, wanted);
                status = 1;
                continue;
            }
            char now[256] = "";
            current(kRoles[role].selector, now, sizeof now);
            if (strcmp(now, wanted) == 0) {
                printf("  %-8s unchanged\n", line);
                continue;
            }
            AudioObjectPropertyAddress a = { kRoles[role].selector, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain };
            if (AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, sizeof device, &device) != noErr) {
                fprintf(stderr, "  %-8s could not be set back to %s\n", line, wanted);
                status = 1;
            } else {
                printf("  %-8s restored to %s (had moved to %s)\n", line, wanted, now);
            }
        }
        fclose(f);
        return status;
    }
    fprintf(stderr, "usage: hal-defaults show | save <file> | restore <file>\n");
    return 2;
}
