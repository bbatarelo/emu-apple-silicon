/*
 * Records from the Tracker Pre through Core Audio and reports what arrived.
 *
 * Exists because nothing on a stock macOS can do this from a shell, and because
 * "the level meter did not move" has at least three different causes: Core Audio
 * never opening the input, the plug-in handing back silence, or microphone
 * permission being denied. This separates them.
 *
 * Reports peak and RMS per channel, and how many frames were exactly zero, so a
 * silent result can be told apart from a stream that never ran.
 */

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_UID "net.quantum-bit.EMUTrackerPre"
#define CHANNELS   2

typedef struct {
    AudioUnit unit;
    float*    buffer;          /* interleaved scratch for one render */
    UInt32    buffer_frames;

    double    peak[CHANNELS];
    double    sum_squares[CHANNELS];
    uint64_t  frames;
    uint64_t  zero_frames;
    uint32_t  render_errors;

    FILE*     wav;
    uint64_t  wav_frames;
} Recorder;

static AudioObjectID find_device(void)
{
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address,
                                       0, NULL, &size) != noErr) return kAudioObjectUnknown;

    UInt32 count = size / sizeof(AudioObjectID);
    if (count > 64) count = 64;
    AudioObjectID devices[64];
    size = count * sizeof(AudioObjectID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                   0, NULL, &size, devices) != noErr) {
        return kAudioObjectUnknown;
    }

    for (UInt32 i = 0; i < count; i++) {
        AudioObjectPropertyAddress uidAddress = {
            kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef uid = NULL;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(devices[i], &uidAddress, 0, NULL,
                                       &uidSize, &uid) != noErr || !uid) continue;
/* Devices are named by the plug-in's prefix plus the unit's serial, so the
         * match is on the prefix. EMU_DEVICE=<text> picks among them when more
         * than one is attached; without it the first found wins. */
        Boolean match = CFStringHasPrefix(uid, CFSTR(DEVICE_UID));
        const char* want = getenv("EMU_DEVICE");
        if (match && want && *want) {
            CFStringRef w = CFStringCreateWithCString(NULL, want, kCFStringEncodingUTF8);
            if (w) { match = CFStringFind(uid, w, 0).location != kCFNotFound; CFRelease(w); }
        }
        CFRelease(uid);
        if (match) return devices[i];
    }
    return kAudioObjectUnknown;
}

static OSStatus input_callback(void* refcon, AudioUnitRenderActionFlags* flags,
                               const AudioTimeStamp* timestamp, UInt32 bus,
                               UInt32 frames, AudioBufferList* io)
{
    (void)io;
    Recorder* rec = (Recorder*)refcon;
    if (frames > rec->buffer_frames) frames = rec->buffer_frames;

    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = CHANNELS;
    list.mBuffers[0].mDataByteSize = frames * CHANNELS * sizeof(float);
    list.mBuffers[0].mData = rec->buffer;

    OSStatus status = AudioUnitRender(rec->unit, flags, timestamp, bus, frames, &list);
    if (status != noErr) { rec->render_errors++; return noErr; }

    for (UInt32 i = 0; i < frames; i++) {
        bool silent = true;
        for (int ch = 0; ch < CHANNELS; ch++) {
            double v = rec->buffer[i * CHANNELS + ch];
            double a = fabs(v);
            if (a > rec->peak[ch]) rec->peak[ch] = a;
            rec->sum_squares[ch] += v * v;
            if (v != 0.0) silent = false;
        }
        if (silent) rec->zero_frames++;
    }
    rec->frames += frames;

    if (rec->wav) {
        for (UInt32 i = 0; i < frames * CHANNELS; i++) {
            float v = rec->buffer[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            int16_t s = (int16_t)(v * 32767.0f);
            fwrite(&s, sizeof s, 1, rec->wav);
        }
        rec->wav_frames += frames;
    }
    return noErr;
}

static void write_wav_header(FILE* f, uint32_t rate, uint64_t frames)
{
    uint32_t data_bytes = (uint32_t)(frames * CHANNELS * 2);
    uint32_t riff = 36 + data_bytes;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);          fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt_size = 16; uint16_t fmt = 1, ch = CHANNELS, bits = 16;
    uint32_t byte_rate = rate * CHANNELS * 2; uint16_t align = CHANNELS * 2;
    fwrite(&fmt_size, 4, 1, f); fwrite(&fmt, 2, 1, f);   fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);     fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);    fwrite(&data_bytes, 4, 1, f);
}

int main(int argc, char** argv)
{
    double seconds = (argc > 1) ? atof(argv[1]) : 5.0;
    const char* wav_path = (argc > 2) ? argv[2] : NULL;

    AudioObjectID device = find_device();
    if (device == kAudioObjectUnknown) {
        fprintf(stderr, "error: %s not found\n", DEVICE_UID);
        return 1;
    }
    printf("recording %.1f s from AudioObjectID %u\n", seconds, device);

    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent component = AudioComponentFindNext(NULL, &desc);
    if (!component) { fprintf(stderr, "error: no HAL output component\n"); return 1; }

    Recorder rec;
    memset(&rec, 0, sizeof rec);
    if (AudioComponentInstanceNew(component, &rec.unit) != noErr) {
        fprintf(stderr, "error: AudioComponentInstanceNew\n");
        return 1;
    }

    /* Element 1 is input, element 0 output. A HAL unit used for capture enables
     * the former and disables the latter. */
    UInt32 enable = 1, disable = 0;
    AudioUnitSetProperty(rec.unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Input, 1, &enable, sizeof enable);
    AudioUnitSetProperty(rec.unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output, 0, &disable, sizeof disable);

    OSStatus status = AudioUnitSetProperty(rec.unit, kAudioOutputUnitProperty_CurrentDevice,
                                           kAudioUnitScope_Global, 0, &device, sizeof device);
    if (status != noErr) {
        fprintf(stderr, "error: could not select the device: %d\n", (int)status);
        return 1;
    }

    Float64 rate = 0;
    UInt32 rateSize = sizeof(rate);
    AudioObjectPropertyAddress rateAddress = {
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(device, &rateAddress, 0, NULL, &rateSize, &rate);
    if (rate <= 0) rate = 48000.0;

    AudioStreamBasicDescription format = {
        .mSampleRate = rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian
                      | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = CHANNELS * sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = CHANNELS * sizeof(float),
        .mChannelsPerFrame = CHANNELS,
        .mBitsPerChannel = 32,
    };
    status = AudioUnitSetProperty(rec.unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output, 1, &format, sizeof format);
    if (status != noErr) {
        fprintf(stderr, "error: stream format rejected: %d\n", (int)status);
        return 1;
    }

    rec.buffer_frames = 8192;
    rec.buffer = calloc(rec.buffer_frames * CHANNELS, sizeof(float));
    if (!rec.buffer) return 1;

    if (wav_path) {
        rec.wav = fopen(wav_path, "wb");
        if (rec.wav) write_wav_header(rec.wav, (uint32_t)rate, 0);
    }

    AURenderCallbackStruct callback = { input_callback, &rec };
    AudioUnitSetProperty(rec.unit, kAudioOutputUnitProperty_SetInputCallback,
                         kAudioUnitScope_Global, 0, &callback, sizeof callback);

    if (AudioUnitInitialize(rec.unit) != noErr) {
        fprintf(stderr, "error: AudioUnitInitialize failed\n");
        return 1;
    }
    status = AudioOutputUnitStart(rec.unit);
    if (status != noErr) {
        fprintf(stderr, "error: AudioOutputUnitStart: %d\n", (int)status);
        fprintf(stderr, "       -2004 usually means microphone permission was denied.\n");
        return 1;
    }

    usleep((useconds_t)(seconds * 1e6));
    AudioOutputUnitStop(rec.unit);
    AudioUnitUninitialize(rec.unit);
    AudioComponentInstanceDispose(rec.unit);

    printf("\n  frames captured   %llu  (%.2f s at %.0f Hz)\n",
           (unsigned long long)rec.frames, rec.frames / rate, rate);
    printf("  render errors     %u\n", rec.render_errors);
    printf("  all-zero frames   %llu", (unsigned long long)rec.zero_frames);
    if (rec.frames) {
        printf("  (%.1f%%)", 100.0 * (double)rec.zero_frames / (double)rec.frames);
    }
    printf("\n");

    for (int ch = 0; ch < CHANNELS; ch++) {
        double rms = rec.frames ? sqrt(rec.sum_squares[ch] / (double)rec.frames) : 0.0;
        printf("  channel %d         peak %.6f (%.1f dBFS), rms %.6f\n", ch,
               rec.peak[ch], rec.peak[ch] > 0 ? 20.0 * log10(rec.peak[ch]) : -999.0, rms);
    }

    if (rec.frames == 0) {
        printf("\n  Core Audio never delivered a buffer: the input was never driven.\n");
    } else if (rec.zero_frames == rec.frames) {
        printf("\n  The stream ran but every sample was zero: the plug-in handed back\n"
               "  silence, or nothing is connected to the inputs.\n");
    } else {
        printf("\n  Audio is arriving.\n");
    }

    if (rec.wav) {
        write_wav_header(rec.wav, (uint32_t)rate, rec.wav_frames);
        fclose(rec.wav);
        printf("  wrote %s\n", wav_path);
    }
    free(rec.buffer);
    return 0;
}
