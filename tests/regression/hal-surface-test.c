/* The release HAL surface, pinned.
 *
 * Drives the production plug-in (driver/plugin.c, included so its static
 * handlers are the real ones) through its AudioServerPlugInDriverInterface
 * against a fake host, a fake bus and a fake engine, and prints what a client
 * would see: objects, UIDs, rates, offsets, controls, custom properties,
 * diagnostics keys, notifications, and what the plug-in asks of the engine.
 * `make test-hal-surface` diffs that against hal-surface.expected.
 *
 * Two uses. It is the inventory docs/SPLIT-DRIVER-PLAN.md stage 1 asks for, in
 * a form that can be re-run after the split instead of re-read. And the fakes
 * below are the complete list of engine-facing calls the plug-in makes: a new
 * dependency on the engine or on IOKit fails to link here before it can reach
 * the boundary unannounced.
 *
 * No USB device is opened and no driver is installed. Nothing here says the
 * values are right -- only that they are what the release driver publishes.
 * A deliberate change updates the expected file in the same commit.
 */
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "../../driver/plugin.c"

/* --- the fake bus --------------------------------------------------------- */

static EmuUnit gBus[EMU_MAX_DEVICES];
static unsigned gBusCount;
static void (*gObserver)(void);

unsigned emu_enumerate_units(EmuUnit* out, unsigned max)
{
    unsigned n = gBusCount < max ? gBusCount : max;
    memcpy(out, gBus, n * sizeof *out);
    return n;
}

/* --- the fake engine: every engine-facing call plugin.c makes -------------- */

struct Engine {
    uint16_t product;
    uint64_t location;
    bool     running;
    uint64_t frames, host;          /* what emu_engine_timeline hands back */
    uint32_t fault;
    void   (*failed)(void*);
    void*    failed_context;
    unsigned writes, reads;
    uint64_t last_write_pos, last_read_pos;
};

static unsigned gFailed;
static bool gQuiet;
static unsigned gStartDelayMs;      /* how long the next engine start blocks */
static _Atomic bool gStarting;      /* an engine start is in progress */

static void say(const char* fmt, ...)
{
    if (gQuiet) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

#define CHECK(cond) do { if (!(cond)) { gFailed++; \
        printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

bool emu_engine_set_identity_observer(void (*observer)(void))
{
    gObserver = observer;
    return true;
}

const char* emu_engine_device_name(void) { return "fake engine name"; }

EmuEngine* emu_engine_create(uint16_t product_id, uint64_t location_id)
{
    EmuEngine* e = calloc(1, sizeof *e);
    e->product = product_id;
    e->location = location_id;
    say("  engine: create product=%04x location=%08llx\n", product_id,
        (unsigned long long)location_id);
    return e;
}

void emu_engine_destroy(EmuEngine* e)
{
    say("  engine: destroy product=%04x%s\n", e->product, e->running ? " (was running)" : "");
    free(e);
}

void emu_engine_set_failure_handler(EmuEngine* e, void (*handler)(void*), void* context)
{
    e->failed = handler;
    e->failed_context = context;
}

bool emu_engine_start(EmuEngine* e, uint32_t sample_rate, uint32_t output_safety_us,
                      bool with_input)
{
    say("  engine: start product=%04x rate=%u safety_us=%u input=%d\n",
        e->product, sample_rate, output_safety_us, (int)with_input);
    if (gStartDelayMs) {
        atomic_store(&gStarting, true);
        usleep(gStartDelayMs * 1000);
        atomic_store(&gStarting, false);
    }
    e->running = true;
    return true;
}

void emu_engine_stop(EmuEngine* e)
{
    say("  engine: stop product=%04x\n", e->product);
    e->running = false;
}

bool emu_engine_running(EmuEngine* e) { return e && e->running; }

bool emu_engine_timeline(EmuEngine* e, uint64_t* frames, uint64_t* host_time)
{
    if (!e || !e->running || !e->host) return false;
    *frames = e->frames;
    *host_time = e->host;
    return true;
}

void emu_engine_write_output(EmuEngine* e, const float* frames, uint32_t count, uint64_t sample_pos)
{
    (void)frames; (void)count;
    e->writes++;
    e->last_write_pos = sample_pos;
}

void emu_engine_read_input(EmuEngine* e, float* frames, uint32_t count, uint64_t sample_pos)
{
    for (uint32_t i = 0; i < count * 2; i++) frames[i] = 0.25f;
    e->reads++;
    e->last_read_pos = sample_pos;
}

void emu_engine_set_output_gain(EmuEngine* e, float gain)
{
    if (e) say("  engine: gain product=%04x %.6f\n", e->product, gain);
}

void emu_engine_reset_counters(EmuEngine* e) { say("  engine: reset counters product=%04x\n", e->product); }

void emu_engine_stats(EmuEngine* e, EmuEngineStats* stats)
{
    memset(stats, 0, sizeof *stats);
    if (e) stats->fault_mode = e->fault;
}

void emu_engine_inject_fault(EmuEngine* e, EmuFaultMode mode)
{
    e->fault = (uint32_t)mode;
    say("  engine: fault product=%04x mode=%u\n", e->product, (unsigned)mode);
}

/* --- the fake host --------------------------------------------------------- */

static const char* cc(UInt32 v)
{
    static char buf[8][8];
    static unsigned next;
    char* s = buf[next++ % 8];
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)(v >> (24 - 8 * i));
        s[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    s[4] = 0;
    return s;
}

static OSStatus host_changed(AudioServerPlugInHostRef host, AudioObjectID object,
                             UInt32 count, const AudioObjectPropertyAddress* addresses)
{
    (void)host;
    say("  host: changed object=%u", (unsigned)object);
    for (UInt32 i = 0; i < count; i++)
        say(" %s/%s", cc(addresses[i].mSelector), cc(addresses[i].mScope));
    say("\n");
    return kAudioHardwareNoError;
}

static AudioObjectID gPendingObject;
static UInt64 gPendingAction;

static OSStatus host_request_change(AudioServerPlugInHostRef host, AudioObjectID object,
                                    UInt64 action, void* info)
{
    (void)host; (void)info;
    say("  host: configuration change requested object=%u action=%llu\n",
        (unsigned)object, (unsigned long long)action);
    gPendingObject = object;
    gPendingAction = action;
    return kAudioHardwareNoError;
}

static const AudioServerPlugInHostInterface gFakeHost = {
    .PropertiesChanged = host_changed,
    .RequestDeviceConfigurationChange = host_request_change,
};

/* The real HAL performs a requested change later, with IO quiesced. */
static void perform_pending(void)
{
    if (!gPendingObject) return;
    AudioObjectID object = gPendingObject;
    gPendingObject = 0;
    CHECK(gEMUInterface.PerformDeviceConfigurationChange(gEMUDriverRef, object,
                                                         gPendingAction, NULL) == 0);
}

/* --- client-side helpers --------------------------------------------------- */

#define D gEMUDriverRef
#define GLOBAL kAudioObjectPropertyScopeGlobal
#define INPUT  kAudioObjectPropertyScopeInput
#define OUTPUT kAudioObjectPropertyScopeOutput

static AudioObjectPropertyAddress addr(UInt32 selector, UInt32 scope)
{
    AudioObjectPropertyAddress a = { selector, scope, kAudioObjectPropertyElementMain };
    return a;
}

static bool has(AudioObjectID object, UInt32 selector)
{
    AudioObjectPropertyAddress a = addr(selector, GLOBAL);
    return gEMUInterface.HasProperty(D, object, 0, &a);
}

static bool settable(AudioObjectID object, UInt32 selector)
{
    AudioObjectPropertyAddress a = addr(selector, GLOBAL);
    Boolean s = false;
    CHECK(gEMUInterface.IsPropertySettable(D, object, 0, &a, &s) == 0);
    return s;
}

static OSStatus get(AudioObjectID object, UInt32 selector, UInt32 scope,
                    UInt32 capacity, void* out, UInt32* size)
{
    AudioObjectPropertyAddress a = addr(selector, scope);
    UInt32 used = 0;
    OSStatus s = gEMUInterface.GetPropertyData(D, object, 0, &a, 0, NULL, capacity, &used, out);
    if (size) *size = used;
    return s;
}

static OSStatus set(AudioObjectID object, UInt32 selector, UInt32 size, const void* data)
{
    AudioObjectPropertyAddress a = addr(selector, GLOBAL);
    return gEMUInterface.SetPropertyData(D, object, 0, &a, 0, NULL, size, data);
}

static UInt32 u32(AudioObjectID object, UInt32 selector, UInt32 scope)
{
    UInt32 v = 0xdeadbeef;
    CHECK(get(object, selector, scope, sizeof v, &v, NULL) == 0);
    return v;
}

static UInt32 size_of(AudioObjectID object, UInt32 selector, UInt32 scope)
{
    AudioObjectPropertyAddress a = addr(selector, scope);
    UInt32 size = 0;
    CHECK(gEMUInterface.GetPropertyDataSize(D, object, 0, &a, 0, NULL, &size) == 0);
    return size;
}

static const char* str(AudioObjectID object, UInt32 selector)
{
    static char buf[4][200];
    static unsigned next;
    char* s = buf[next++ % 4];
    CFStringRef v = NULL;
    snprintf(s, sizeof buf[0], "<error>");
    if (get(object, selector, GLOBAL, sizeof v, &v, NULL) == 0 && v) {
        CFStringGetCString(v, s, sizeof buf[0], kCFStringEncodingUTF8);
        CFRelease(v);
    } else {
        CHECK(!"string property");
    }
    return s;
}

static void set_str(AudioObjectID object, UInt32 selector, const char* value, OSStatus want)
{
    CFStringRef v = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
    OSStatus s = set(object, selector, sizeof v, &v);
    CFRelease(v);
    if (s != want) say("  unexpected status %d setting %s to %s\n", (int)s, cc(selector), value);
    CHECK(s == want);
}

static void ids(const char* label, AudioObjectID object, UInt32 selector, UInt32 scope)
{
    AudioObjectID list[16];
    UInt32 size = 0;
    CHECK(get(object, selector, scope, sizeof list, list, &size) == 0);
    CHECK(size == size_of(object, selector, scope));
    say("%s:", label);
    for (UInt32 i = 0; i < size / sizeof list[0]; i++) say(" %u", (unsigned)list[i]);
    if (!size) say(" (none)");
    say("\n");
}

static void format_line(const char* label, AudioObjectID stream, UInt32 selector)
{
    AudioStreamBasicDescription f;
    CHECK(get(stream, selector, GLOBAL, sizeof f, &f, NULL) == 0);
    say("%s: %s %.0f Hz flags=0x%x %u ch %u bits %u bytes/frame\n", label, cc(f.mFormatID),
        f.mSampleRate, (unsigned)f.mFormatFlags, (unsigned)f.mChannelsPerFrame,
        (unsigned)f.mBitsPerChannel, (unsigned)f.mBytesPerFrame);
}

static int by_name(const void* a, const void* b)
{
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static void set_rate(AudioObjectID device, Float64 rate)
{
    CHECK(set(device, kAudioDevicePropertyNominalSampleRate, sizeof rate, &rate) == 0);
    perform_pending();
}

/* --- the inventory --------------------------------------------------------- */

static void dump_identity(AudioObjectID dev)
{
    say("device object %u\n", (unsigned)dev);
    say("  name: %s\n", str(dev, kAudioObjectPropertyName));
    say("  uid: %s\n", str(dev, kAudioDevicePropertyDeviceUID));
    say("  model uid: %s\n", str(dev, kAudioDevicePropertyModelUID));
    say("  manufacturer: %s\n", str(dev, kAudioObjectPropertyManufacturer));
    say("  class %s base %s owner %u\n", cc(u32(dev, kAudioObjectPropertyClass, GLOBAL)),
        cc(u32(dev, kAudioObjectPropertyBaseClass, GLOBAL)),
        (unsigned)u32(dev, kAudioObjectPropertyOwner, GLOBAL));
    ids("  streams global", dev, kAudioDevicePropertyStreams, GLOBAL);
    ids("  streams input", dev, kAudioDevicePropertyStreams, INPUT);
    ids("  streams output", dev, kAudioDevicePropertyStreams, OUTPUT);
    ids("  owned global", dev, kAudioObjectPropertyOwnedObjects, GLOBAL);
    ids("  owned input", dev, kAudioObjectPropertyOwnedObjects, INPUT);
    ids("  owned output", dev, kAudioObjectPropertyOwnedObjects, OUTPUT);
    ids("  controls", dev, kAudioObjectPropertyControlList, GLOBAL);
}

static void dump_device(AudioObjectID dev)
{
    AudioObjectID in = dev + kRole_StreamInput, out = dev + kRole_StreamOutput;
    AudioObjectID vol = dev + kRole_VolumeOutput, mute = dev + kRole_MuteOutput;

    say("  transport %s clock domain %u\n", cc(u32(dev, kAudioDevicePropertyTransportType, GLOBAL)),
        (unsigned)u32(dev, kAudioDevicePropertyClockDomain, GLOBAL));
    say("  alive %u running %u default %u system default %u hidden %u\n",
        (unsigned)u32(dev, kAudioDevicePropertyDeviceIsAlive, GLOBAL),
        (unsigned)u32(dev, kAudioDevicePropertyDeviceIsRunning, GLOBAL),
        (unsigned)u32(dev, kAudioDevicePropertyDeviceCanBeDefaultDevice, GLOBAL),
        (unsigned)u32(dev, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, GLOBAL),
        (unsigned)u32(dev, kAudioDevicePropertyIsHidden, GLOBAL));
    say("  zero timestamp period %u\n",
        (unsigned)u32(dev, kAudioDevicePropertyZeroTimeStampPeriod, GLOBAL));
    /* The plug-in publishes no buffer size or range; the HAL derives both. */
    say("  buffer frame size published %d, range published %d\n",
        (int)has(dev, 'fsiz'),      /* kAudioDevicePropertyBufferFrameSize */
        (int)has(dev, 'fsz#'));     /* kAudioDevicePropertyBufferFrameSizeRange */
    AudioObjectID related[4];
    UInt32 related_size = 99;
    CHECK(get(dev, kAudioDevicePropertyRelatedDevices, GLOBAL, sizeof related, related, &related_size) == 0);
    say("  related devices: size query %u bytes, data %u bytes\n",
        (unsigned)size_of(dev, kAudioDevicePropertyRelatedDevices, GLOBAL), (unsigned)related_size);

    UInt32 stereo[2] = { 0, 0 };
    CHECK(get(dev, kAudioDevicePropertyPreferredChannelsForStereo, OUTPUT, sizeof stereo, stereo, NULL) == 0);
    say("  preferred stereo channels %u %u\n", (unsigned)stereo[0], (unsigned)stereo[1]);

    Float64 rate = 0;
    CHECK(get(dev, kAudioDevicePropertyNominalSampleRate, GLOBAL, sizeof rate, &rate, NULL) == 0);
    say("  nominal rate %.0f settable %d\n", rate,
        (int)settable(dev, kAudioDevicePropertyNominalSampleRate));
    AudioValueRange rates[16];
    UInt32 size = 0;
    CHECK(get(dev, kAudioDevicePropertyAvailableNominalSampleRates, GLOBAL, sizeof rates, rates, &size) == 0);
    say("  available rates:");
    for (UInt32 i = 0; i < size / sizeof rates[0]; i++) {
        CHECK(rates[i].mMinimum == rates[i].mMaximum);
        say(" %.0f", rates[i].mMinimum);
    }
    say("\n");

    for (int i = 0; i < 2; i++) {
        AudioObjectID s = i ? out : in;
        say("  stream %u: class %s owner %u direction %u terminal %s start channel %u latency %u "
            "active %u (settable %d)\n", (unsigned)s,
            cc(u32(s, kAudioObjectPropertyClass, GLOBAL)),
            (unsigned)u32(s, kAudioObjectPropertyOwner, GLOBAL),
            (unsigned)u32(s, kAudioStreamPropertyDirection, GLOBAL),
            cc(u32(s, kAudioStreamPropertyTerminalType, GLOBAL)),
            (unsigned)u32(s, kAudioStreamPropertyStartingChannel, GLOBAL),
            (unsigned)u32(s, kAudioStreamPropertyLatency, GLOBAL),
            (unsigned)u32(s, kAudioStreamPropertyIsActive, GLOBAL),
            (int)settable(s, kAudioStreamPropertyIsActive));
        format_line("    virtual format", s, kAudioStreamPropertyVirtualFormat);
        format_line("    physical format", s, kAudioStreamPropertyPhysicalFormat);
        say("    available formats: %u virtual, %u physical\n",
            (unsigned)(size_of(s, kAudioStreamPropertyAvailableVirtualFormats, GLOBAL)
                       / sizeof(AudioStreamRangedDescription)),
            (unsigned)(size_of(s, kAudioStreamPropertyAvailablePhysicalFormats, GLOBAL)
                       / sizeof(AudioStreamRangedDescription)));
    }

    Float32 scalar = -1, db = 1;
    AudioValueRange range = { 0, 0 };
    CHECK(get(vol, kAudioLevelControlPropertyScalarValue, GLOBAL, sizeof scalar, &scalar, NULL) == 0);
    CHECK(get(vol, kAudioLevelControlPropertyDecibelValue, GLOBAL, sizeof db, &db, NULL) == 0);
    CHECK(get(vol, kAudioLevelControlPropertyDecibelRange, GLOBAL, sizeof range, &range, NULL) == 0);
    say("  volume %u: class %s base %s scope %s element %u scalar %.3f dB %.1f range %.1f..%.1f "
        "settable %d\n", (unsigned)vol, cc(u32(vol, kAudioObjectPropertyClass, GLOBAL)),
        cc(u32(vol, kAudioObjectPropertyBaseClass, GLOBAL)),
        cc(u32(vol, kAudioControlPropertyScope, GLOBAL)),
        (unsigned)u32(vol, kAudioControlPropertyElement, GLOBAL), scalar, db,
        range.mMinimum, range.mMaximum, (int)settable(vol, kAudioLevelControlPropertyScalarValue));
    say("  mute %u: class %s base %s scope %s value %u settable %d\n", (unsigned)mute,
        cc(u32(mute, kAudioObjectPropertyClass, GLOBAL)),
        cc(u32(mute, kAudioObjectPropertyBaseClass, GLOBAL)),
        cc(u32(mute, kAudioControlPropertyScope, GLOBAL)),
        (unsigned)u32(mute, kAudioBooleanControlPropertyValue, GLOBAL),
        (int)settable(mute, kAudioBooleanControlPropertyValue));

    AudioServerPlugInCustomPropertyInfo custom[16];
    CHECK(get(dev, kAudioObjectPropertyCustomPropertyInfoList, GLOBAL, sizeof custom, custom, &size) == 0);
    CHECK(size == size_of(dev, kAudioObjectPropertyCustomPropertyInfoList, GLOBAL));
    say("  custom properties:\n");
    for (UInt32 i = 0; i < size / sizeof custom[0]; i++) {
        CHECK(has(dev, custom[i].mSelector));
        say("    %s type %s qualifier %s settable %d\n", cc(custom[i].mSelector),
            cc(custom[i].mPropertyDataType),
            custom[i].mQualifierDataType == kAudioServerPlugInCustomPropertyDataTypeNone
                ? "none" : cc(custom[i].mQualifierDataType),
            (int)settable(dev, custom[i].mSelector));
    }
    say("  emuK clock source: %s\n", str(dev, kEMUProperty_ClockSource));
    say("  emuI input mode: %s\n", str(dev, kEMUProperty_InputMode));
    say("  emuX fault: %s\n", str(dev, kEMUProperty_FaultInject));
    say("  emuR resets: %s\n", str(dev, kEMUProperty_ResetCounters));
    say("  emuV is this build's version: %d\n",
        strcmp(str(dev, kEMUProperty_Version), EMU_VERSION " (git " EMU_GITREV ")") == 0);
    CFNumberRef us = NULL;
    long long safety = -1;
    CHECK(get(dev, kEMUProperty_SafetyOffset, GLOBAL, sizeof us, &us, NULL) == 0 && us);
    if (us) { CFNumberGetValue(us, kCFNumberLongLongType, &safety); CFRelease(us); }
    say("  emuS output safety: %lld us\n", safety);

    CFDictionaryRef diag = NULL;
    CHECK(get(dev, kEMUProperty_Diagnostics, GLOBAL, sizeof diag, &diag, NULL) == 0 && diag);
    if (diag) {
        CFIndex n = CFDictionaryGetCount(diag);
        const void* keys[128];
        static char names[128][48];
        const char* sorted[128];
        CHECK(n <= 128);
        CFDictionaryGetKeysAndValues(diag, keys, NULL);
        for (CFIndex i = 0; i < n; i++) {
            CFStringGetCString(keys[i], names[i], sizeof names[i], kCFStringEncodingUTF8);
            sorted[i] = names[i];
        }
        qsort(sorted, (size_t)n, sizeof sorted[0], by_name);
        say("  emuD diagnostics: %ld keys\n   ", (long)n);
        for (CFIndex i = 0; i < n; i++)
            say(" %s%s", sorted[i], (i % 6 == 5 && i + 1 < n) ? "\n   " : "");
        say("\n");
        CFRelease(diag);
    }
}

static void rate_table(AudioObjectID dev)
{
    static const Float64 kRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
    say("  rate | latency in/out | safety in/out (frames)\n");
    for (unsigned i = 0; i < 6; i++) {
        set_rate(dev, kRates[i]);
        Float64 now = 0;
        CHECK(get(dev, kAudioDevicePropertyNominalSampleRate, GLOBAL, sizeof now, &now, NULL) == 0);
        CHECK(now == kRates[i]);
        say("  %6.0f | %u / %u | %u / %u\n", now,
            (unsigned)u32(dev, kAudioDevicePropertyLatency, INPUT),
            (unsigned)u32(dev, kAudioDevicePropertyLatency, OUTPUT),
            (unsigned)u32(dev, kAudioDevicePropertySafetyOffset, INPUT),
            (unsigned)u32(dev, kAudioDevicePropertySafetyOffset, OUTPUT));
    }
    Float64 bad = 32000;
    CHECK(set(dev, kAudioDevicePropertyNominalSampleRate, sizeof bad, &bad) != 0);
    set_rate(dev, 48000);
}

/* `sample` false where the answer extrapolates from the host clock and so is
 * not reproducible; the seed still is. */
static void zero_timestamp(AudioObjectID dev, const char* label, bool sample)
{
    Float64 time = -1;
    UInt64 host = 0, seed = 0;
    CHECK(gEMUInterface.GetZeroTimeStamp(D, dev, 0, &time, &host, &seed) == 0);
    CHECK(time >= 0 && fmod(time, EMU_ZERO_TIMESTAMP_PERIOD) == 0);
    if (sample) say("  zero timestamp %s: sample %.0f seed %llu\n", label, time,
                    (unsigned long long)seed);
    else        say("  zero timestamp %s: host clock, seed %llu\n", label,
                    (unsigned long long)seed);
}

static void io_seam(AudioObjectID dev)
{
    Device* d = device_for_object(dev, NULL);
    EmuEngine* e = d->engine;
    float buffer[64 * 2] = { 0 };
    AudioServerPlugInIOCycleInfo cycle;
    memset(&cycle, 0, sizeof cycle);

    static const struct { UInt32 op; const char* name; } kOps[] = {
        { kAudioServerPlugInIOOperationThread, "Thread" },
        { kAudioServerPlugInIOOperationCycle, "Cycle" },
        { kAudioServerPlugInIOOperationReadInput, "ReadInput" },
        { kAudioServerPlugInIOOperationConvertInput, "ConvertInput" },
        { kAudioServerPlugInIOOperationProcessInput, "ProcessInput" },
        { kAudioServerPlugInIOOperationProcessOutput, "ProcessOutput" },
        { kAudioServerPlugInIOOperationMixOutput, "MixOutput" },
        { kAudioServerPlugInIOOperationProcessMix, "ProcessMix" },
        { kAudioServerPlugInIOOperationConvertMix, "ConvertMix" },
        { kAudioServerPlugInIOOperationWriteMix, "WriteMix" },
    };
    say("  operations done:");
    for (unsigned i = 0; i < sizeof kOps / sizeof kOps[0]; i++) {
        Boolean will = false, in_place = false;
        CHECK(gEMUInterface.WillDoIOOperation(D, dev, 0, kOps[i].op, &will, &in_place) == 0);
        if (will) say(" %s%s", kOps[i].name, in_place ? " (in place)" : "");
    }
    say("\n");

    e->host = 1000000;              /* the engine's start anchor: frame 0 */
    e->frames = 0;
    CHECK(gEMUInterface.StartIO(D, dev, 1) == 0);
    CHECK(gEMUInterface.StartIO(D, dev, 2) == 0);     /* second client: no second start */
    say("  running %u after two StartIO\n",
        (unsigned)u32(dev, kAudioDevicePropertyDeviceIsRunning, GLOBAL));
    zero_timestamp(dev, "at start", true);
    e->frames = 2 * EMU_ZERO_TIMESTAMP_PERIOD + 100;  /* two periods and a bit later */
    e->host = 500000000;
    zero_timestamp(dev, "after 2 periods + 100 frames", true);
    zero_timestamp(dev, "asked again", true);

    /* Sample times are honoured as given; a negative one must not reach the
     * engine as a wrapped index. */
    cycle.mOutputTime.mSampleTime = 4096.0;
    cycle.mInputTime.mSampleTime = 2048.0;
    CHECK(gEMUInterface.DoIOOperation(D, dev, dev + kRole_StreamOutput, 1,
              kAudioServerPlugInIOOperationWriteMix, 64, &cycle, buffer, NULL) == 0);
    CHECK(gEMUInterface.DoIOOperation(D, dev, dev + kRole_StreamInput, 1,
              kAudioServerPlugInIOOperationReadInput, 64, &cycle, buffer, NULL) == 0);
    say("  write at 4096 reached the engine at %llu, read at 2048 at %llu, input sample %.2f\n",
        (unsigned long long)e->last_write_pos, (unsigned long long)e->last_read_pos, buffer[0]);
    cycle.mOutputTime.mSampleTime = -64.0;
    cycle.mInputTime.mSampleTime = -64.0;
    CHECK(gEMUInterface.DoIOOperation(D, dev, dev + kRole_StreamOutput, 1,
              kAudioServerPlugInIOOperationWriteMix, 64, &cycle, buffer, NULL) == 0);
    CHECK(gEMUInterface.DoIOOperation(D, dev, dev + kRole_StreamInput, 1,
              kAudioServerPlugInIOOperationReadInput, 64, &cycle, buffer, NULL) == 0);
    say("  negative sample time: engine writes %u reads %u, input sample %.2f\n",
        e->writes, e->reads, buffer[0]);

    /* The engine giving up is how a transport loss reaches Core Audio; a new
     * StartIO after the clients have gone is the way back. */
    say("  engine reports failure:\n");
    e->running = false;
    e->failed(e->failed_context);
    say("  alive %u\n", (unsigned)u32(dev, kAudioDevicePropertyDeviceIsAlive, GLOBAL));
    zero_timestamp(dev, "with the engine gone", false);
    CHECK(gEMUInterface.StopIO(D, dev, 1) == 0);
    CHECK(gEMUInterface.StopIO(D, dev, 2) == 0);
    CHECK(gEMUInterface.StartIO(D, dev, 3) == 0);
    say("  alive %u\n", (unsigned)u32(dev, kAudioDevicePropertyDeviceIsAlive, GLOBAL));
    e->frames = 0;
    e->host = 1000000;
    zero_timestamp(dev, "new session", true);
    CHECK(gEMUInterface.StopIO(D, dev, 3) == 0);
}

/* Whose session is it. Core Audio names the client on every StartIO and
 * StopIO; a session can end without any of them asking (the engine gives up,
 * or in the two-part build goes away), and a stop can arrive long after the
 * start it belongs to. */
static void clients_and_sessions(AudioObjectID dev)
{
    Device* d = device_for_object(dev, NULL);

    say("  client 11 starts:\n");
    CHECK(gEMUInterface.StartIO(D, dev, 11) == 0);
    say("  the engine is lost while client 11 is still open:\n");
    d->engine->running = false;
    d->engine->failed(d->engine->failed_context);
    say("  client 22 starts:\n");
    CHECK(gEMUInterface.StartIO(D, dev, 22) == 0);
    say("  engine running %d, alive %u, clients %u\n", (int)d->engine->running,
        (unsigned)u32(dev, kAudioDevicePropertyDeviceIsAlive, GLOBAL), (unsigned)d->iOClients);
    CHECK(d->engine->running);                 /* success means a live session */
    say("  client 11 stops, then stops again:\n");
    CHECK(gEMUInterface.StopIO(D, dev, 11) == 0);
    CHECK(gEMUInterface.StopIO(D, dev, 11) == 0);
    say("  engine running %d, clients %u\n", (int)d->engine->running, (unsigned)d->iOClients);
    CHECK(d->engine->running);                 /* client 22's session */
    say("  a client that never started stops:\n");
    CHECK(gEMUInterface.StopIO(D, dev, 99) == 0);
    CHECK(d->engine->running);
    say("  client 22 stops:\n");
    CHECK(gEMUInterface.StopIO(D, dev, 22) == 0);
    CHECK(!d->engine->running);

    say("  client 11 starts, the unit is unplugged and plugged back, client 33 starts:\n");
    CHECK(gEMUInterface.StartIO(D, dev, 11) == 0);
    EmuUnit keep = gBus[0];
    gBus[0] = gBus[1];
    gBusCount = 1;
    gObserver();
    gBus[1] = gBus[0];
    gBus[0] = keep;
    gBusCount = 2;
    gObserver();
    d = device_for_object(dev, NULL);
    CHECK(gEMUInterface.StartIO(D, dev, 33) == 0);
    say("  client 11's stop arrives late:\n");
    CHECK(gEMUInterface.StopIO(D, dev, 11) == 0);
    say("  engine running %d, clients %u\n", (int)d->engine->running, (unsigned)d->iOClients);
    CHECK(d->engine->running);                 /* not client 11's to stop */
    CHECK(gEMUInterface.StopIO(D, dev, 33) == 0);
    CHECK(!d->engine->running);
}

static void client(AudioObjectID dev, UInt32 id, bool add)
{
    AudioServerPlugInClientInfo info = { .mClientID = id, .mProcessID = (pid_t)(1000 + id) };
    CHECK((add ? gEMUInterface.AddDeviceClient : gEMUInterface.RemoveDeviceClient)(D, dev, &info) == 0);
}

/* What coreaudiod was measured to send: one StartIO when the first client
 * starts and one StopIO when the last one stops, whoever those are. On the
 * Tracker Pre a session whose stop carried another client's ID ran on with
 * nobody playing until coreaudiod restarted. */
static void clients_as_core_audio_sends_them(AudioObjectID dev)
{
    Device* d = device_for_object(dev, NULL);

    say("  clients 11 and 22 open the device; 11 starts first, 22 stops last:\n");
    client(dev, 11, true);
    client(dev, 22, true);
    CHECK(gEMUInterface.StartIO(D, dev, 11) == 0);
    CHECK(gEMUInterface.StopIO(D, dev, 22) == 0);
    say("  engine running %d, clients %u\n", (int)d->engine->running, (unsigned)d->iOClients);
    CHECK(!d->engine->running && d->iOClients == 0 && !d->ioUnowned);
    CHECK(u32(dev, kAudioDevicePropertyDeviceIsRunning, GLOBAL) == 0);

    say("  both start; 11 goes without stopping; 22 stops:\n");
    CHECK(gEMUInterface.StartIO(D, dev, 11) == 0);
    CHECK(gEMUInterface.StartIO(D, dev, 22) == 0);
    client(dev, 11, false);
    say("  engine running %d, clients %u\n", (int)d->engine->running, (unsigned)d->iOClients);
    CHECK(d->engine->running && d->iOClients == 1);     /* 22 is still playing */
    CHECK(gEMUInterface.StopIO(D, dev, 22) == 0);
    say("  engine running %d, clients %u\n", (int)d->engine->running, (unsigned)d->iOClients);
    CHECK(!d->engine->running && d->iOClients == 0);    /* and nothing of 11's is left */

    say("  11 starts, then goes without stopping while 22 is still open:\n");
    client(dev, 11, true);
    CHECK(gEMUInterface.StartIO(D, dev, 11) == 0);
    client(dev, 11, false);
    say("  engine running %d, unowned %d\n", (int)d->engine->running, (int)d->ioUnowned);
    CHECK(d->engine->running);                  /* 22 may be the one playing */
    CHECK(u32(dev, kAudioDevicePropertyDeviceIsRunning, GLOBAL) == 1);
    CHECK(gEMUInterface.StopIO(D, dev, 22) == 0);
    CHECK(!d->engine->running && !d->ioUnowned);

    say("  22 starts and is the only client left when it goes without stopping:\n");
    CHECK(gEMUInterface.StartIO(D, dev, 22) == 0);
    client(dev, 22, false);
    say("  engine running %d, clients %u\n", (int)d->engine->running, (unsigned)d->iOClients);
    CHECK(!d->engine->running && d->iOClients == 0 && !d->ioUnowned);
    CHECK(d->sessionsReaped == 1 && d->startersRemoved == 3 && d->foreignStops == 2);
}

static void* slow_start(void* arg)
{
    AudioObjectID dev = *(AudioObjectID*)arg;
    CHECK(gEMUInterface.StartIO(D, dev, 1) == 0);
    return NULL;
}

/* One unit taking its time to start must not hold up another's clock: the IO
 * thread of a device that is already playing calls GetZeroTimeStamp every
 * cycle, and a quarter of a second there is a quarter of a second of dropout
 * on a device nobody touched. */
static void start_does_not_stall_others(AudioObjectID playing, AudioObjectID starting)
{
    Device* d = device_for_object(playing, NULL);
    d->engine->host = 1000000;
    d->engine->frames = 0;
    CHECK(gEMUInterface.StartIO(D, playing, 1) == 0);

    gQuiet = true;                      /* timing below; the order of lines is not fixed */
    gStartDelayMs = 250;
    pthread_t thread;
    pthread_create(&thread, NULL, slow_start, &starting);
    while (!atomic_load(&gStarting)) usleep(1000);

    double worst_ms = 0;
    unsigned during = 0;
    while (atomic_load(&gStarting)) {
        Float64 sample;
        UInt64 host, seed;
        uint64_t t0 = mach_absolute_time();
        CHECK(gEMUInterface.GetZeroTimeStamp(D, playing, 0, &sample, &host, &seed) == 0);
        double ms = (double)(mach_absolute_time() - t0) * 1000.0 / host_ticks_per_second();
        if (ms > worst_ms) worst_ms = ms;
        (void)u32(playing, kAudioDevicePropertyDeviceIsRunning, GLOBAL);
        during++;
        usleep(2000);
    }
    pthread_join(thread, NULL);
    gStartDelayMs = 0;
    gQuiet = false;

    CHECK(during > 20);
    CHECK(worst_ms < 20.0);
    say("  while the other unit spent 250 ms starting: clock asked %s, slowest answer %s\n",
        during > 20 ? "throughout" : "TOO FEW TIMES", worst_ms < 20.0 ? "under 20 ms" : "TOO SLOW");
    if (worst_ms >= 20.0) printf("  slowest GetZeroTimeStamp: %.1f ms\n", worst_ms);
    CHECK(gEMUInterface.StopIO(D, playing, 1) == 0);
    CHECK(gEMUInterface.StopIO(D, starting, 1) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    gBus[0] = (EmuUnit){ emu_device_for_product(0x3f0a), 0x14100000, "E-MU-C7-3F0A-FAKE0001" };
    gBus[1] = (EmuUnit){ emu_device_for_product(0x3f04), 0x14200000, "E-MU-69-3F04-FAKE0002" };
    gBusCount = 2;

    say("== factory and initialize\n");
    CHECK(EMUTrackerPreFactory(NULL, kAudioServerPlugInTypeUUID) == gEMUDriverRef);
    CHECK(EMUTrackerPreFactory(NULL, IUnknownUUID) == NULL);
    CHECK(gEMUInterface.Initialize(D, &gFakeHost) == 0);

    say("== plug-in object %u\n", (unsigned)kObjectID_PlugIn);
    say("  class %s manufacturer: %s\n", cc(u32(kObjectID_PlugIn, kAudioObjectPropertyClass, GLOBAL)),
        str(kObjectID_PlugIn, kAudioObjectPropertyManufacturer));
    ids("  device list", kObjectID_PlugIn, kAudioPlugInPropertyDeviceList, GLOBAL);
    ids("  owned", kObjectID_PlugIn, kAudioObjectPropertyOwnedObjects, GLOBAL);

    AudioObjectID tracker = DEVICE_OBJECT_BASE, emu0404 = DEVICE_OBJECT_BASE + DEVICE_OBJECT_STRIDE;

    say("== Tracker Pre, slot 0\n");
    dump_identity(tracker);
    dump_device(tracker);
    say("== 0404 USB, slot 1\n");
    dump_identity(emu0404);

    {
        AudioObjectPropertyAddress a = addr(kAudioPlugInPropertyTranslateUIDToDevice, GLOBAL);
        CFStringRef uid = CFSTR("net.batarelo.EMUTrackerPre.E-MU-69-3F04-FAKE0002");
        AudioObjectID found = 0;
        UInt32 size = 0;
        CHECK(gEMUInterface.GetPropertyData(D, kObjectID_PlugIn, 0, &a, sizeof uid, &uid,
                                            sizeof found, &size, &found) == 0);
        say("  uid translates to object %u\n", (unsigned)found);
        uid = CFSTR("net.batarelo.EMUTrackerPre");
        CHECK(gEMUInterface.GetPropertyData(D, kObjectID_PlugIn, 0, &a, sizeof uid, &uid,
                                            sizeof found, &size, &found) == 0);
        say("  bare prefix translates to object %u\n", (unsigned)found);
    }

    say("== rates, Tracker Pre\n");
    rate_table(tracker);

    say("== output safety offset 'emuS', Tracker Pre\n");
    static const long long kSafety[] = { 1000, 10000, 50000, 4000 };
    for (unsigned i = 0; i < 4; i++) {
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongLongType, &kSafety[i]);
        CHECK(set(tracker, kEMUProperty_SafetyOffset, sizeof n, &n) == 0);
        CFRelease(n);
        say("  asked %lld us: output safety %u frames at 48000\n", kSafety[i],
            (unsigned)u32(tracker, kAudioDevicePropertySafetyOffset, OUTPUT));
    }

    say("== input mode 'emuI', Tracker Pre\n");
    set_str(tracker, kEMUProperty_InputMode, "off", 0);
    perform_pending();
    ids("  off: streams input", tracker, kAudioDevicePropertyStreams, INPUT);
    ids("  off: owned global", tracker, kAudioObjectPropertyOwnedObjects, GLOBAL);
    set_str(tracker, kEMUProperty_InputMode, "auto", 0);
    perform_pending();
    ids("  auto at 48000: streams input", tracker, kAudioDevicePropertyStreams, INPUT);
    set_rate(tracker, 176400);
    ids("  auto at 176400: streams input", tracker, kAudioDevicePropertyStreams, INPUT);
    set_rate(tracker, 48000);
    set_str(tracker, kEMUProperty_InputMode, "on", 0);
    perform_pending();
    set_str(tracker, kEMUProperty_InputMode, "sometimes", kAudioHardwareIllegalOperationError);

    /* Baseline defect, pinned so its fix is a visible change: the visibility
     * notification names slot 0 whichever device the mode was set on. */
    say("== input mode on the 0404, slot 1\n");
    set_str(emu0404, kEMUProperty_InputMode, "off", 0);
    perform_pending();
    ids("  0404 streams input", emu0404, kAudioDevicePropertyStreams, INPUT);
    ids("  Tracker Pre streams input", tracker, kAudioDevicePropertyStreams, INPUT);
    set_str(emu0404, kEMUProperty_InputMode, "on", 0);
    perform_pending();

    say("== clock source, fault and reset, Tracker Pre\n");
    set_str(tracker, kEMUProperty_ClockSource, "host", 0);
    say("  clock source: %s\n", str(tracker, kEMUProperty_ClockSource));
    set_str(tracker, kEMUProperty_ClockSource, "device", 0);
    set_str(tracker, kEMUProperty_ClockSource, "wall", kAudioHardwareIllegalOperationError);
    set_str(tracker, kEMUProperty_FaultInject, "startup-stale", 0);
    say("  fault: %s\n", str(tracker, kEMUProperty_FaultInject));
    set_str(tracker, kEMUProperty_FaultInject, "none", 0);
    set_str(tracker, kEMUProperty_ResetCounters, "1", 0);
    say("  resets: %s\n", str(tracker, kEMUProperty_ResetCounters));

    say("== volume and mute, Tracker Pre\n");
    Float32 half = 0.5f;
    UInt32 on = 1, off = 0;
    CHECK(set(tracker + kRole_VolumeOutput, kAudioLevelControlPropertyScalarValue, sizeof half, &half) == 0);
    CHECK(set(tracker + kRole_MuteOutput, kAudioBooleanControlPropertyValue, sizeof on, &on) == 0);
    CHECK(set(tracker + kRole_MuteOutput, kAudioBooleanControlPropertyValue, sizeof off, &off) == 0);
    Float32 full = 1.0f;
    CHECK(set(tracker + kRole_VolumeOutput, kAudioLevelControlPropertyScalarValue, sizeof full, &full) == 0);

    say("== IO seam, Tracker Pre\n");
    io_seam(tracker);

    say("== clients and sessions, Tracker Pre\n");
    clients_and_sessions(tracker);

    say("== clients as Core Audio sends them\n");
    clients_as_core_audio_sends_them(tracker);

    say("== one unit starting while another plays\n");
    start_does_not_stall_others(tracker, emu0404);

    say("== hot-plug\n");
    say("  Tracker Pre unplugged:\n");
    gBus[0] = gBus[1];
    gBusCount = 1;
    gObserver();
    ids("  device list", kObjectID_PlugIn, kAudioPlugInPropertyDeviceList, GLOBAL);
    say("  Tracker Pre back, on another port:\n");
    gBus[1] = (EmuUnit){ emu_device_for_product(0x3f0a), 0x14300000, "E-MU-C7-3F0A-FAKE0001" };
    gBusCount = 2;
    gObserver();
    ids("  device list", kObjectID_PlugIn, kAudioPlugInPropertyDeviceList, GLOBAL);
    say("  object %u uid: %s\n", (unsigned)tracker, str(tracker, kAudioDevicePropertyDeviceUID));
    say("  everything unplugged:\n");
    gBusCount = 0;
    gObserver();
    ids("  device list", kObjectID_PlugIn, kAudioPlugInPropertyDeviceList, GLOBAL);
    CHECK(gEMUInterface.StartIO(D, tracker, 1) == kAudioHardwareBadObjectError);

    if (gFailed) {
        printf("hal-surface-test: %u check(s) FAILED\n", gFailed);
        return 1;
    }
    say("== done\n");
    return 0;
}
