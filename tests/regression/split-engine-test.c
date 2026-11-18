/* The two-part driver, end to end, without hardware.
 *
 * This process is the HAL: the production plug-in (driver/plugin.c) linked to
 * the production engine link (driver/engine_remote.c), both included here so
 * the test can see their state, driven through AudioServerPlugInDriverInterface
 * by a fake host. The other process is the production engine service
 * (driver/engine_service.c) over a pretend device (fake-transport.c), started
 * by launchd under a throwaway name in the user's own domain -- see
 * split-engine-test.sh. Real XPC, real shared memory, two real processes; no
 * USB, no installed driver, no Core Audio server.
 *
 * The pretend device loops playback into capture at the same sample index, so
 * "what I read at N is what I wrote at N" checks placement across the whole
 * boundary. A frame that arrives too late comes back as silence, never as some
 * other frame, so the placement check does not depend on this machine being
 * idle; how many frames arrived in time is reported and bounded separately.
 *
 * Modes: `shared` (the playback store alone, in this process), `main`
 * (everything, against a healthy engine), `abi` (an engine that speaks
 * another ABI), `absent` (no engine at all).
 */
#include <stdarg.h>
#include <stdio.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>

#include "../../driver/plugin.c"
#include "../../driver/engine_remote.c"

/* --- reporting -------------------------------------------------------------- */

static unsigned gFailures;

static void check(bool ok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s ", ok ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) gFailures++;
    fflush(stdout);
}

static double ticks_per_second(void)
{
    static double t;
    if (!t) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        t = 1.0e9 * (double)tb.denom / (double)tb.numer;
    }
    return t;
}

static double seconds_since(uint64_t start)
{
    return (double)(mach_absolute_time() - start) / ticks_per_second();
}

/* Polls `cond` until it holds or `timeout` seconds pass. */
#define WAIT_FOR(cond, timeout) ({ \
        uint64_t start_ = mach_absolute_time(); \
        bool ok_ = false; \
        while (!(ok_ = (cond)) && seconds_since(start_) < (timeout)) usleep(10000); \
        ok_; })

/* --- the fake host ---------------------------------------------------------- */

static _Atomic unsigned gListChanges;
static _Atomic unsigned gAliveChanges;

static OSStatus host_changed(AudioServerPlugInHostRef host, AudioObjectID object,
                             UInt32 count, const AudioObjectPropertyAddress* addresses)
{
    (void)host; (void)object;
    for (UInt32 i = 0; i < count; i++) {
        if (addresses[i].mSelector == kAudioPlugInPropertyDeviceList) atomic_fetch_add(&gListChanges, 1);
        if (addresses[i].mSelector == kAudioDevicePropertyDeviceIsAlive) atomic_fetch_add(&gAliveChanges, 1);
    }
    return kAudioHardwareNoError;
}

static OSStatus host_request_change(AudioServerPlugInHostRef host, AudioObjectID object,
                                    UInt64 action, void* info)
{
    (void)host;
    /* The real HAL performs it later with IO stopped; nothing is running when
     * the tests change a rate, so performing it here is the same thing. */
    return gEMUInterface.PerformDeviceConfigurationChange(gEMUDriverRef, object, action, info);
}

static const AudioServerPlugInHostInterface gFakeHost = {
    .PropertiesChanged = host_changed,
    .RequestDeviceConfigurationChange = host_request_change,
};

/* --- client-side helpers ---------------------------------------------------- */

#define D gEMUDriverRef

static UInt32 u32(AudioObjectID object, UInt32 selector, UInt32 scope)
{
    AudioObjectPropertyAddress a = { selector, scope, kAudioObjectPropertyElementMain };
    UInt32 v = 0, size = 0;
    gEMUInterface.GetPropertyData(D, object, 0, &a, 0, NULL, sizeof v, &size, &v);
    return v;
}

static unsigned device_list(AudioObjectID* out)
{
    AudioObjectPropertyAddress a = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    AudioObjectID ids[8];
    UInt32 size = 0;
    gEMUInterface.GetPropertyData(D, kObjectID_PlugIn, 0, &a, 0, NULL, sizeof ids, &size, ids);
    if (out) memcpy(out, ids, size);
    return size / sizeof ids[0];
}

static void get_string(AudioObjectID object, UInt32 selector, char* out, size_t cap)
{
    AudioObjectPropertyAddress a = { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef v = NULL;
    UInt32 size = 0;
    out[0] = 0;
    if (gEMUInterface.GetPropertyData(D, object, 0, &a, 0, NULL, sizeof v, &size, &v) == 0 && v) {
        CFStringGetCString(v, out, (CFIndex)cap, kCFStringEncodingUTF8);
        CFRelease(v);
    }
}

static OSStatus set_string(AudioObjectID object, UInt32 selector, const char* value)
{
    AudioObjectPropertyAddress a = { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef v = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
    OSStatus s = gEMUInterface.SetPropertyData(D, object, 0, &a, 0, NULL, sizeof v, &v);
    CFRelease(v);
    return s;
}

static void set_rate(AudioObjectID device, Float64 rate)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    gEMUInterface.SetPropertyData(D, device, 0, &a, 0, NULL, sizeof rate, &rate);
}

static void set_volume(AudioObjectID device, Float32 scalar)
{
    AudioObjectPropertyAddress a = { kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    gEMUInterface.SetPropertyData(D, device + kRole_VolumeOutput, 0, &a, 0, NULL, sizeof scalar, &scalar);
}

static Float32 get_volume(AudioObjectID device)
{
    AudioObjectPropertyAddress a = { kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    Float32 v = -1;
    UInt32 size = 0;
    gEMUInterface.GetPropertyData(D, device + kRole_VolumeOutput, 0, &a, 0, NULL, sizeof v, &size, &v);
    return v;
}

/* One figure out of the 'emuD' dictionary, as hal-check would read it. */
static long long diag(AudioObjectID device, const char* key)
{
    AudioObjectPropertyAddress a = { kEMUProperty_Diagnostics, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    CFDictionaryRef d = NULL;
    UInt32 size = 0;
    long long value = -1;
    if (gEMUInterface.GetPropertyData(D, device, 0, &a, 0, NULL, sizeof d, &size, &d) != 0 || !d) return -1;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFNumberRef n = CFDictionaryGetValue(d, k);
    if (n) CFNumberGetValue(n, kCFNumberLongLongType, &value);
    CFRelease(k);
    CFRelease(d);
    return value;
}

static long long engine_pid(void)
{
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "op", "status");
    xpc_object_t reply = NULL;
    long long pid = 0;
    if (request(msg, 2000, &reply) == kReplyOK) pid = xpc_dictionary_get_int64(reply, "pid");
    if (reply) xpc_release(reply);
    return pid;
}

/* --- a Core Audio IO thread, more or less ----------------------------------- */

typedef struct {
    AudioObjectID dev;
    double   rate;
    uint32_t buffer;
    double   seconds;
    uint32_t salt;
    float    gain;

    uint64_t cycles, checked, exact, silent, wrong;
    double   zts_max_ms;
    bool     zts_failed;
    uint64_t seed;
} IoRun;

/* What frame `f` of channel `ch` holds going out. Never zero once packed, so
 * silence coming back is always distinguishable from audio. */
static float pattern(uint64_t f, unsigned ch, uint32_t salt)
{
    uint32_t k = (uint32_t)((f * 2654435761ull + ch * 40503u + salt) % 16000000u) + 100000u;
    float v = (float)k / 8388608.0f - 1.0f;
    return (v > -0.001f && v < 0.001f) ? 0.5f : v;
}

/* ...and what the capture side must then hand back: through the production
 * 24-bit packing, the gain, and the ring's unpacking. */
static float expected(uint64_t f, unsigned ch, uint32_t salt, float gain)
{
    uint8_t p[3];
    emu_pack_sample_s24(p, pattern(f, ch, salt) * gain);
    int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
    if (v & 0x800000) v |= (int32_t)0xff000000;
    return (float)v / 8388608.0f;
}

static bool anchor(IoRun* r, double* sample, uint64_t* host)
{
    Float64 st = 0;
    UInt64 ht = 0, seed = 0;
    uint64_t t0 = mach_absolute_time();
    OSStatus s = gEMUInterface.GetZeroTimeStamp(D, r->dev, 0, &st, &ht, &seed);
    double ms = seconds_since(t0) * 1000.0;
    if (ms > r->zts_max_ms) r->zts_max_ms = ms;
    if (s != 0) { r->zts_failed = true; return false; }
    *sample = st;
    *host = ht;
    r->seed = seed;
    return true;
}

/* Core Audio's IO threads are real-time. An ordinary thread's timers are
 * coalesced by milliseconds, which here would read as late audio. */
static void io_thread_policy(double period_seconds)
{
    thread_time_constraint_policy_data_t policy;
    policy.period      = (uint32_t)(period_seconds * ticks_per_second());
    policy.computation = policy.period / 8;
    policy.constraint  = policy.period / 2;
    policy.preemptible = TRUE;
    thread_port_t thread = mach_thread_self();
    thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), thread);
}

static void* io_thread(void* arg)
{
    IoRun* r = (IoRun*)arg;
    io_thread_policy((double)r->buffer / r->rate);
    double ticks_per_frame = ticks_per_second() / r->rate;
    uint32_t out_safety = u32(r->dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
    uint32_t in_safety  = u32(r->dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput);
    float out[4096 * 2], in[4096 * 2];
    AudioServerPlugInIOCycleInfo cycle;
    memset(&cycle, 0, sizeof cycle);

    double sample = 0;
    uint64_t host = 0;
    if (!anchor(r, &sample, &host)) return NULL;
    double now = sample + ((double)mach_absolute_time() - (double)host) / ticks_per_frame;
    int64_t first = ((int64_t)(now / r->buffer) + 2) * (int64_t)r->buffer;
    if (first < 0) first = 0;
    uint64_t first_written = (uint64_t)first + out_safety + r->buffer;
    uint64_t total = (uint64_t)(r->seconds * r->rate / r->buffer);

    for (uint64_t c = 0; c < total; c++) {
        uint64_t f = (uint64_t)first + c * r->buffer;
        if (!anchor(r, &sample, &host)) break;
        mach_wait_until(host + (uint64_t)(((double)f - sample) * ticks_per_frame));

        uint64_t out_pos = f + out_safety + r->buffer;
        for (uint32_t i = 0; i < r->buffer; i++) {
            out[i * 2]     = pattern(out_pos + i, 0, r->salt);
            out[i * 2 + 1] = pattern(out_pos + i, 1, r->salt);
        }
        cycle.mOutputTime.mSampleTime = (Float64)out_pos;
        gEMUInterface.DoIOOperation(D, r->dev, r->dev + kRole_StreamOutput, 0,
                                    kAudioServerPlugInIOOperationWriteMix, r->buffer, &cycle, out, NULL);

        if (f >= (uint64_t)in_safety + r->buffer) {
            uint64_t in_pos = f - in_safety - r->buffer;
            cycle.mInputTime.mSampleTime = (Float64)in_pos;
            gEMUInterface.DoIOOperation(D, r->dev, r->dev + kRole_StreamInput, 0,
                                        kAudioServerPlugInIOOperationReadInput, r->buffer, &cycle, in, NULL);
            for (uint32_t i = 0; i < r->buffer; i++) {
                uint64_t frame = in_pos + i;
                float l = in[i * 2], rr = in[i * 2 + 1];
                if (frame < first_written) {
                    if (l != 0.0f || rr != 0.0f) r->wrong++;    /* audio where none was sent */
                    continue;
                }
                r->checked++;
                if (l == expected(frame, 0, r->salt, r->gain) &&
                    rr == expected(frame, 1, r->salt, r->gain)) r->exact++;
                else if (l == 0.0f && rr == 0.0f) r->silent++;
                else r->wrong++;
            }
        }
        r->cycles++;
    }
    return NULL;
}

static void report(const char* what, const IoRun* r)
{
    double on_time = r->checked ? 100.0 * (double)r->exact / (double)r->checked : 0.0;
    check(r->wrong == 0 && r->checked > 0 && !r->zts_failed,
          "%s: %llu cycles, %llu frames checked, %llu misplaced", what,
          (unsigned long long)r->cycles, (unsigned long long)r->checked, (unsigned long long)r->wrong);
    check(on_time >= 90.0, "%s: %.2f%% of frames arrived in time (%llu late, as silence)", what,
          on_time, (unsigned long long)r->silent);
    check(r->zts_max_ms < 5.0, "%s: slowest GetZeroTimeStamp %.3f ms", what, r->zts_max_ms);

    /* What the split adds: how long after the HAL wrote a cycle the engine
     * began binding it. Reported, and bounded only loosely -- this is a
     * pretend device on a machine doing other things, not a measurement. */
    Device* dev = device_for_object(r->dev, NULL);
    EmuShared* shm = dev && dev->engine ? atomic_load(&dev->engine->shm) : NULL;
    if (shm) {
        uint64_t worst = atomic_load(&shm->pump.copy_delay_max_ns);
        uint64_t records = atomic_load(&shm->pump.records);
        uint64_t within = atomic_load(&shm->pump.copy_delay[0]) + atomic_load(&shm->pump.copy_delay[1])
                        + atomic_load(&shm->pump.copy_delay[2]) + atomic_load(&shm->pump.copy_delay[3]);
        check(records > 0 && worst < 20000000 && atomic_load(&shm->hal_dropped) == 0
              && atomic_load(&shm->pump.invalid) == 0 && atomic_load(&shm->pump.resyncs) == 0,
              "%s: %llu writes forwarded, %.1f%% within 1 ms, worst %.3f ms, queue depth <= %u", what,
              (unsigned long long)records, records ? 100.0 * (double)within / (double)records : 0.0,
              (double)worst / 1.0e6, atomic_load(&shm->pump.queue_depth_max));
    }
}

static IoRun run_io(AudioObjectID dev, double rate, uint32_t buffer, double seconds, uint32_t salt, float gain)
{
    IoRun r = { .dev = dev, .rate = rate, .buffer = buffer, .seconds = seconds, .salt = salt, .gain = gain };
    io_thread(&r);
    return r;
}

/* --- the modes -------------------------------------------------------------- */

static void write_bus(const char* text)
{
    const char* path = getenv("EMU_FAKE_BUS");
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    fputs(text, f);
    fclose(f);
    rename(tmp, path);
}

#define BUS_TRACKER "3f0a 14100000 E-MU-C7-3F0A-FAKE0001\n"
#define BUS_0404    "3f04 14200000 E-MU-69-3F04-FAKE0002\n"

static int mode_main(void)
{
    const AudioObjectID tracker = DEVICE_OBJECT_BASE, emu0404 = DEVICE_OBJECT_BASE + DEVICE_OBJECT_STRIDE;
    char text[200];

    printf("Connection and inventory\n");
    check(gEMUInterface.Initialize(D, &gFakeHost) == 0, "plug-in initializes");
    check(WAIT_FOR(device_list(NULL) == 2, 5.0), "both units are published");
    if (device_list(NULL) != 2) {
        /* No engine, or not the one this test started: nothing below would
         * be testing what it says. */
        printf("  the test engine is not serving; the remaining checks were not run\n");
        gFailures++;
        return 1;
    }
    get_string(tracker, kAudioDevicePropertyDeviceUID, text, sizeof text);
    check(strcmp(text, "net.batarelo.EMUTrackerPre.E-MU-C7-3F0A-FAKE0001") == 0,
          "slot 0 is the Tracker Pre with a serial-derived UID");
    get_string(emu0404, kAudioObjectPropertyName, text, sizeof text);
    check(strcmp(text, "E-MU 0404 USB") == 0, "slot 1 is the 0404");
    long long pid0 = engine_pid();
    check(pid0 > 0 && pid0 != getpid(), "the engine is another process (pid %lld, HAL %d)", pid0, (int)getpid());

    printf("Before any session\n");
    {
        float buffer[64 * 2];
        AudioServerPlugInIOCycleInfo cycle;
        memset(&cycle, 0, sizeof cycle);
        memset(buffer, 0x55, sizeof buffer);
        cycle.mInputTime.mSampleTime = 1000;
        gEMUInterface.DoIOOperation(D, tracker, tracker + kRole_StreamInput, 0,
                                    kAudioServerPlugInIOOperationReadInput, 64, &cycle, buffer, NULL);
        check(buffer[0] == 0.0f && buffer[127] == 0.0f, "reading with no session returns silence");
    }

    printf("One device, 48 kHz / 128 frames\n");
    check(gEMUInterface.StartIO(D, tracker, 1) == 0, "StartIO");
    Device* dev = device_for_object(tracker, NULL);
    uint64_t f0 = 0, h0 = 0;
    check(emu_engine_timeline(dev->engine, &f0, &h0), "the clock anchor exists when StartIO returns");
    IoRun r = run_io(tracker, 48000, 128, 1.0, 11, 1.0f);
    report("48 kHz / 128", &r);
    uint64_t seed1 = r.seed;
    {
        float buffer[64 * 2];
        AudioServerPlugInIOCycleInfo cycle;
        memset(&cycle, 0, sizeof cycle);
        long long before = diag(tracker, "framesToOutput");
        cycle.mOutputTime.mSampleTime = -64;
        cycle.mInputTime.mSampleTime = -64;
        memset(buffer, 0x55, sizeof buffer);
        gEMUInterface.DoIOOperation(D, tracker, tracker + kRole_StreamOutput, 0,
                                    kAudioServerPlugInIOOperationWriteMix, 64, &cycle, buffer, NULL);
        gEMUInterface.DoIOOperation(D, tracker, tracker + kRole_StreamInput, 0,
                                    kAudioServerPlugInIOOperationReadInput, 64, &cycle, buffer, NULL);
        check(diag(tracker, "framesToOutput") == before && buffer[0] == 0.0f,
              "a negative sample time writes nothing and reads silence");

        EmuShared* shm = atomic_load(&dev->engine->shm);
        float big[8];
        emu_engine_write_output(dev->engine, big, EMU_SHARED_MAX_IO + 1, 1u << 20);
        check(atomic_load(&shm->hal_oversize) == 1, "an oversized write is refused, not copied");
    }

    printf("A stale stop, and a second HAL\n");
    {
        xpc_object_t msg = unit_message(dev->engine, "stop");
        xpc_dictionary_set_uint64(msg, "generation", atomic_load(&dev->engine->generation) - 1);
        xpc_object_t reply = NULL;
        ReplyStatus s = request(msg, 2000, &reply);
        check(s == kReplyOK && !reply_ok(reply) && xpc_dictionary_get_bool(reply, "stale"),
              "a stop naming an earlier session is refused");
        if (reply) xpc_release(reply);
        uint64_t f1 = 0, f2 = 0, h = 0;
        emu_engine_timeline(dev->engine, &f1, &h);
        usleep(50000);
        check(emu_engine_timeline(dev->engine, &f2, &h) && f2 > f1, "...and the current session keeps running");

        dispatch_queue_t q = dispatch_queue_create("second", DISPATCH_QUEUE_SERIAL);
        xpc_connection_t second = xpc_connection_create_mach_service(getenv("EMU_ENGINE_SERVICE"), q, 0);
        xpc_connection_set_event_handler(second, ^(xpc_object_t e) { (void)e; });
        xpc_connection_resume(second);
        xpc_object_t hello = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(hello, "op", "hello");
        xpc_dictionary_set_uint64(hello, "abi", EMU_ENGINE_ABI);
        xpc_object_t answer = xpc_connection_send_message_with_reply_sync(second, hello);
        check(xpc_get_type(answer) == XPC_TYPE_DICTIONARY && !xpc_dictionary_get_bool(answer, "ok"),
              "a second HAL is refused while the first is connected");
        xpc_release(answer);
        xpc_release(hello);
        xpc_object_t stop = unit_message(dev->engine, "stop");
        xpc_dictionary_set_uint64(stop, "generation", atomic_load(&dev->engine->generation));
        answer = xpc_connection_send_message_with_reply_sync(second, stop);
        check(xpc_get_type(answer) == XPC_TYPE_DICTIONARY && !xpc_dictionary_get_bool(answer, "ok"),
              "...and cannot stop the first one's session");
        xpc_release(answer);
        xpc_release(stop);
        xpc_connection_cancel(second);
        xpc_release(second);
        emu_engine_timeline(dev->engine, &f1, &h);
        usleep(50000);
        check(emu_engine_timeline(dev->engine, &f2, &h) && f2 > f1, "the session is still running");
    }

    printf("Stop, final figures, restart\n");
    long long sent = diag(tracker, "framesToOutput");
    check(gEMUInterface.StopIO(D, tracker, 1) == 0, "StopIO");
    check(diag(tracker, "engineRunning") == 0 && diag(tracker, "engineStreaming") == 0, "engine reports stopped");
    check(diag(tracker, "framesBound") + diag(tracker, "unmappedFrames") == sent && sent > 0,
          "every frame handed over is accounted for after the stop (%lld)", sent);
    check(diag(tracker, "framesPlayed") > 40000, "the last session's figures survive it");
    check(gEMUInterface.StartIO(D, tracker, 1) == 0, "StartIO again");
    r = run_io(tracker, 48000, 256, 0.5, 12, 1.0f);
    report("second session", &r);
    check(r.seed > seed1, "a new session is a new timeline (seed %llu -> %llu)",
          (unsigned long long)seed1, (unsigned long long)r.seed);
    gEMUInterface.StopIO(D, tracker, 1);

    printf("Two devices at once: 44.1 kHz / 64 and 96 kHz / 512\n");
    set_rate(tracker, 44100);
    set_rate(emu0404, 96000);
    check(gEMUInterface.StartIO(D, tracker, 1) == 0 && gEMUInterface.StartIO(D, emu0404, 1) == 0, "both start");
    {
        IoRun a = { .dev = tracker, .rate = 44100, .buffer = 64,  .seconds = 1.5, .salt = 21, .gain = 1.0f };
        IoRun b = { .dev = emu0404, .rate = 96000, .buffer = 512, .seconds = 1.5, .salt = 22, .gain = 1.0f };
        pthread_t ta, tb;
        pthread_create(&ta, NULL, io_thread, &a);
        pthread_create(&tb, NULL, io_thread, &b);
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        report("Tracker Pre 44.1 kHz", &a);
        report("0404 96 kHz", &b);
        check(a.checked > EMU_RING_FRAMES && b.checked > EMU_RING_FRAMES,
              "both ran past a full lap of the shared rings");
        long long fa = diag(tracker, "framesPlayed"), fb = diag(emu0404, "framesPlayed");
        check(fa > 0 && fb > fa * 2 - fa / 4, "each keeps its own clock (%lld vs %lld frames)", fa, fb);
    }
    gEMUInterface.StopIO(D, tracker, 1);
    gEMUInterface.StopIO(D, emu0404, 1);
    set_rate(tracker, 48000);
    set_rate(emu0404, 48000);

    printf("Volume\n");
    set_volume(tracker, 0.5f);
    check(gEMUInterface.StartIO(D, tracker, 1) == 0, "StartIO");
    r = run_io(tracker, 48000, 128, 0.4, 31, 0.125f);
    report("at half volume", &r);
    gEMUInterface.StopIO(D, tracker, 1);

    printf("Faults\n");
    check(set_string(tracker, kEMUProperty_FaultInject, "startup-stale") == 0, "arm the one-shot startup fault");
    get_string(tracker, kEMUProperty_FaultInject, text, sizeof text);
    check(strcmp(text, "startup-stale") == 0, "it reads back armed while idle");
    gEMUInterface.StartIO(D, tracker, 1);
    usleep(150000);
    gEMUInterface.StopIO(D, tracker, 1);
    get_string(tracker, kEMUProperty_FaultInject, text, sizeof text);
    check(strcmp(text, "none") == 0, "the start consumed it (%s)", text);

    gEMUInterface.StartIO(D, tracker, 1);
    unsigned alive_before = atomic_load(&gAliveChanges);
    set_string(tracker, kEMUProperty_FaultInject, "persistent");
    check(WAIT_FOR(u32(tracker, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 0, 3.0),
          "an engine that gives up takes the device offline");
    check(atomic_load(&gAliveChanges) > alive_before, "...and Core Audio is told");
    check(!emu_engine_running(dev->engine), "the session is over on the HAL side");
    set_string(tracker, kEMUProperty_FaultInject, "none");
    check(u32(tracker, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 1,
          "clearing the fault revives it");
    gEMUInterface.StopIO(D, tracker, 1);
    check(gEMUInterface.StartIO(D, tracker, 1) == 0, "and it starts again");
    r = run_io(tracker, 48000, 128, 0.3, 41, 0.125f);
    report("after the fault", &r);
    gEMUInterface.StopIO(D, tracker, 1);

    printf("Hot-plug\n");
    write_bus(BUS_TRACKER);
    check(WAIT_FOR(device_list(NULL) == 1, 3.0), "unplugging the 0404 withdraws it");
    write_bus(BUS_TRACKER BUS_0404);
    check(WAIT_FOR(device_list(NULL) == 2, 3.0), "plugging it back publishes it");
    get_string(emu0404, kAudioDevicePropertyDeviceUID, text, sizeof text);
    check(strcmp(text, "net.batarelo.EMUTrackerPre.E-MU-69-3F04-FAKE0002") == 0, "with the same UID");

    printf("The engine dies while streaming, half way through publishing its clock\n");
    check(gEMUInterface.StartIO(D, tracker, 1) == 0, "StartIO");
    r = run_io(tracker, 48000, 128, 1.2, 51, 0.125f);
    unsigned lists_before = atomic_load(&gListChanges);
    {
        xpc_object_t msg = unit_message(dev->engine, "test-crash");
        xpc_dictionary_set_bool(msg, "mid_publication", true);
        xpc_object_t reply = NULL;
        request(msg, 500, &reply);
        if (reply) xpc_release(reply);
    }
    check(WAIT_FOR(u32(tracker, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 0, 3.0),
          "the device goes offline");
    {
        EmuShared* shm = atomic_load(&dev->engine->shm);
        check((atomic_load(&shm->tl_seq) & 1u) == 1, "the clock was left mid-publication");
        uint64_t f = 0, h = 0;
        check(!emu_shared_read_timeline(shm, &f, &h), "a reader of it gives up instead of spinning");
        IoRun dead = { .dev = tracker, .rate = 48000 };
        double sample = 0;
        uint64_t host = 0;
        bool ok = true;
        for (int i = 0; i < 200; i++) ok = ok && anchor(&dead, &sample, &host);
        check(ok && dead.zts_max_ms < 5.0 && dead.seed == r.seed,
              "GetZeroTimeStamp keeps answering, promptly, on the same timeline (%.3f ms worst)", dead.zts_max_ms);
        float buffer[64 * 2];
        AudioServerPlugInIOCycleInfo cycle;
        memset(&cycle, 0, sizeof cycle);
        memset(buffer, 0x55, sizeof buffer);
        cycle.mInputTime.mSampleTime = 4096;
        cycle.mOutputTime.mSampleTime = 8192;
        gEMUInterface.DoIOOperation(D, tracker, tracker + kRole_StreamOutput, 0,
                                    kAudioServerPlugInIOOperationWriteMix, 64, &cycle, buffer, NULL);
        gEMUInterface.DoIOOperation(D, tracker, tracker + kRole_StreamInput, 0,
                                    kAudioServerPlugInIOOperationReadInput, 64, &cycle, buffer, NULL);
        check(buffer[0] == 0.0f, "IO goes on harmlessly: silence in, writes dropped");
    }
    /* Its shared snapshot is unreadable now; the HAL's own copy, refreshed
     * twice a second while it ran, is what is left. */
    check(diag(tracker, "framesPlayed") > 24000,
          "its figures from the last half second are still readable (%lld frames played)",
          diag(tracker, "framesPlayed"));
    check(WAIT_FOR(atomic_load(&gEpoch) != 0, 15.0), "launchd restarts the engine and the HAL reconnects");
    long long pid1 = engine_pid();
    check(pid1 > 0 && pid1 != pid0, "it is a new engine process (pid %lld -> %lld), same HAL (%d)",
          pid0, pid1, (int)getpid());
    check(device_list(NULL) == 2 && atomic_load(&gListChanges) == lists_before,
          "the devices were never withdrawn");
    check(get_volume(tracker) == 0.5f, "and kept their settings");
    /* Client 1 never stopped: it was simply left with a dead session. A
     * second client starting now must get a real one, not a success that
     * means "someone is already running". */
    check(gEMUInterface.StartIO(D, tracker, 2) == 0, "another client starting gets a stream from the new engine");
    check(emu_engine_running(dev->engine) && emu_engine_streaming(dev->engine),
          "that start is a live engine session");
    check(u32(tracker, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 1, "alive again");
    r = run_io(tracker, 48000, 128, 0.5, 52, 0.125f);
    report("on the replacement engine", &r);
    gEMUInterface.StopIO(D, tracker, 1);
    {
        uint64_t f1 = 0, f2 = 0, h = 0;
        emu_engine_timeline(dev->engine, &f1, &h);
        usleep(50000);
        check(emu_engine_timeline(dev->engine, &f2, &h) && f2 > f1,
              "the first client's late stop does not end the second client's session");
    }
    gEMUInterface.StopIO(D, tracker, 2);
    check(!emu_engine_running(dev->engine), "the second client's stop does");

    printf("A clean engine reload: emu-engine ctl shutdown\n");
    {
        char cmd[1200];
        snprintf(cmd, sizeof cmd, "'%s' ctl shutdown '%s' --user-domain > /dev/null",
                 getenv("EMU_ENGINE_BIN"), getenv("EMU_ENGINE_SERVICE"));
        check(system(cmd) == 0, "the engine acknowledges the shutdown before it exits");
        check(WAIT_FOR(atomic_load(&gEpoch) == 0, 3.0), "the HAL sees it go");
        check(WAIT_FOR(atomic_load(&gEpoch) != 0, 15.0), "and sees its replacement");
        long long pid2 = engine_pid();
        check(pid2 > 0 && pid2 != pid1, "new engine process (pid %lld -> %lld)", pid1, pid2);
        check(device_list(NULL) == 2 && atomic_load(&gListChanges) == lists_before
              && u32(tracker, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 1,
              "idle devices were not disturbed");
        check(gEMUInterface.StartIO(D, tracker, 1) == 0, "StartIO");
        /* A new engine starts at unity gain; the HAL's level has to be in
         * force before its first frame, not one pump interval later. The
         * device's own figure for it is the first thing to look at. */
        r = run_io(tracker, 48000, 128, 0.5, 61, 0.125f);
        report("after the reload, at the volume set before it", &r);
        gEMUInterface.StopIO(D, tracker, 1);
        check(atomic_load(&gRetiredRegions) == 0, "no shared region had to be abandoned so far");
    }

    printf("An engine that hangs\n");
    check(gEMUInterface.StartIO(D, tracker, 1) == 0, "StartIO");
    {
        xpc_object_t reply = NULL;
        request(unit_message(dev->engine, "test-hang"), 1000, &reply);
        if (reply) xpc_release(reply);
        check(WAIT_FOR(u32(tracker, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 0, 5.0),
              "a stopped pump is noticed without any XPC error");
        gEMUInterface.StopIO(D, tracker, 1);
        char cmd[400];
        snprintf(cmd, sizeof cmd, "launchctl kill SIGKILL gui/%d/%s", (int)getuid(), getenv("EMU_ENGINE_SERVICE"));
        check(system(cmd) == 0, "the hung engine is killed");
        check(WAIT_FOR(atomic_load(&gEpoch) == 0, 3.0) && WAIT_FOR(atomic_load(&gEpoch) != 0, 15.0),
              "the HAL reconnects to its replacement");
        check(gEMUInterface.StartIO(D, tracker, 1) == 0, "StartIO");
        check(atomic_load(&gRetiredRegions) == 1, "the region the hung engine held was not reused");
        r = run_io(tracker, 48000, 128, 0.5, 71, 0.125f);
        report("on a fresh region", &r);
        gEMUInterface.StopIO(D, tracker, 1);
    }

    printf("Everything unplugged\n");
    write_bus("");
    check(WAIT_FOR(device_list(NULL) == 0, 3.0), "nothing is published");
    return 0;
}

/* The playback store on its own: no engine, no connection -- the two ends of
 * the shared region driven directly, the consumer exactly as the pump drives
 * it. What it must never do is hand the engine, for one sample time, the
 * frames published for another. */
static float tag(uint64_t frame, unsigned ch) { return (float)(frame * 2 + ch + 1); }

static unsigned drain(EmuShared* s, uint64_t* frames_out)
{
    unsigned wrong = 0;
    EmuWriteRecord r;
    while (emu_shared_peek(s, &r)) {
        for (uint32_t i = 0; i < r.count; i++) {
            const float* f = &s->play[(size_t)((r.at + i) & EMU_RING_MASK) * EMU_RING_CHANNELS];
            if (f[0] != tag(r.pos + i, 0) || f[1] != tag(r.pos + i, 1)) wrong++;
        }
        *frames_out += r.count;
        emu_shared_done(s, &r);
    }
    return wrong;
}

static void put(EmuShared* s, uint64_t pos, uint32_t count)
{
    static float buffer[(EMU_SHARED_MAX_IO + 1) * 2];   /* one past the limit, for the refusal */
    for (uint32_t i = 0; i < count; i++) {
        buffer[i * 2] = tag(pos + i, 0);
        buffer[i * 2 + 1] = tag(pos + i, 1);
    }
    emu_shared_write(s, buffer, count, pos);
}

static int mode_shared(void)
{
    printf("The shared playback store\n");
    EmuShared* s = NULL;
    if (posix_memalign((void**)&s, 16384, sizeof *s) != 0) return 1;
    emu_shared_init(s, sizeof *s);
    check(emu_shared_valid(s, sizeof *s), "a fresh region validates");

    /* The pump has not run yet and Core Audio keeps writing: 65 cycles of
     * 512 frames is 512 more than the store holds. */
    uint64_t pos = 0, got = 0;
    for (int i = 0; i < 65; i++, pos += 512) put(s, pos, 512);
    check(atomic_load(&s->hal_dropped) == 1 && atomic_load(&s->hal_dropped_frames) == 512,
          "the write that does not fit is refused whole, before anything is bound");
    unsigned wrong = drain(s, &got);
    check(wrong == 0 && got == 64 * 512,
          "every frame handed on is the one published for its sample time (%llu frames, %u wrong)",
          (unsigned long long)got, wrong);

    /* The pump resumes; the store has wrapped and keeps wrapping. */
    got = 0;
    wrong = 0;
    for (int i = 0; i < 400; i++, pos += 448) {
        put(s, pos, 448);
        if (i % 3 == 2) wrong += drain(s, &got);
    }
    wrong += drain(s, &got);
    check(wrong == 0 && got == 400 * 448 && atomic_load(&s->hal_dropped) == 1,
          "it carries on across further laps with nothing lost (%llu frames, %u wrong)",
          (unsigned long long)got, wrong);

    /* Sample times need not be contiguous, or even increasing: a rewrite of
     * an earlier range, then a jump a whole lap ahead that would alias it in
     * a store indexed by sample time. */
    got = 0;
    put(s, 1000, 256);
    put(s, 1000, 256);
    put(s, 1000 + EMU_RING_FRAMES, 256);
    put(s, 0, 64);
    wrong = drain(s, &got);
    check(wrong == 0 && got == 832, "rewrites and whole-lap jumps keep their own frames");

    /* Record slots run out before frames do. */
    uint64_t dropped = atomic_load(&s->hal_dropped);
    for (unsigned i = 0; i < EMU_WRITE_QUEUE + 20; i++) put(s, 5000 + i, 1);
    got = 0;
    wrong = drain(s, &got);
    check(atomic_load(&s->hal_dropped) - dropped == 20 && got == EMU_WRITE_QUEUE && wrong == 0,
          "with every record slot taken, further writes are refused and the queued ones survive");

    /* What the other side might leave behind. */
    put(s, 9000, 128);
    s->queue[atomic_load(&s->q_tail) & EMU_WRITE_QUEUE_MASK].count = 1u << 30;
    put(s, 9128, 128);
    EmuWriteRecord r;
    got = 0;
    bool survived = emu_shared_peek(s, &r) && r.pos == 9128 && r.count == 128;
    if (survived) emu_shared_done(s, &r);
    check(survived && atomic_load(&s->pump.invalid) == 1 && !emu_shared_peek(s, &r),
          "a record with an impossible length is skipped, not followed");

    atomic_store(&s->q_tail, atomic_load(&s->q_head) + 5);
    put(s, 20000, 128);
    check(atomic_load(&s->hal_dropped) - dropped == 21,
          "indices the engine left inconsistent make the HAL refuse, not overwrite");
    check(!emu_shared_peek(s, &r) && atomic_load(&s->pump.resyncs) == 1,
          "and the pump starts again from where the HAL is rather than reading what it finds");
    emu_shared_discard(s);
    put(s, 30000, 128);
    got = 0;
    check(drain(s, &got) == 0 && got == 128, "a new session's discard puts both sides back in step");

    put(s, 0, EMU_SHARED_MAX_IO + 1);
    check(atomic_load(&s->hal_oversize) == 1, "an oversized write is refused");
    free(s);
    return 0;
}

/* An engine that is there and speaks another ABI: nothing is published, and
 * nothing waits. */
static int mode_refused(const char* what)
{
    printf("%s\n", what);
    uint64_t t0 = mach_absolute_time();
    check(gEMUInterface.Initialize(D, &gFakeHost) == 0, "plug-in initializes");
    check(seconds_since(t0) < 1.5, "without waiting on it (%.0f ms)", seconds_since(t0) * 1000.0);
    usleep(1500000);
    check(device_list(NULL) == 0, "no device is published");
    check(atomic_load(&gEpoch) == 0, "the HAL does not consider itself connected");

    EmuEngine* e = emu_engine_create(0x3f0a, 0x14100000);
    check(e != NULL, "a proxy can still be made");
    t0 = mach_absolute_time();
    check(!emu_engine_start(e, 48000, 4000, true), "a start fails");
    check(seconds_since(t0) < 0.2, "at once (%.1f ms)", seconds_since(t0) * 1000.0);
    float buffer[64 * 2];
    uint64_t f = 0, h = 0;
    memset(buffer, 0x55, sizeof buffer);
    t0 = mach_absolute_time();
    emu_engine_write_output(e, buffer, 64, 0);
    emu_engine_read_input(e, buffer, 64, 0);
    check(!emu_engine_timeline(e, &f, &h) && buffer[0] == 0.0f && seconds_since(t0) < 0.005,
          "the IO calls return immediately: no clock, silence in");
    EmuEngineStats stats;
    emu_engine_stats(e, &stats);
    check(stats.frames_played == 0 && stats.engine_streaming == 0, "statistics read as an idle engine");
    emu_engine_destroy(e);
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char* mode = argc > 1 ? argv[1] : "main";
    if (strcmp(mode, "shared") == 0) {
        mode_shared();
        printf("\n%u failure%s.\n", gFailures, gFailures == 1 ? "" : "s");
        return gFailures ? 1 : 0;
    }
    if (!getenv("EMU_ENGINE_SERVICE")) {
        fprintf(stderr, "run through tests/regression/split-engine-test.sh\n");
        return 2;
    }
    if (strcmp(mode, "main") == 0) mode_main();
    else if (strcmp(mode, "abi") == 0) mode_refused("An engine with another ABI");
    else if (strcmp(mode, "absent") == 0) mode_refused("No engine at all");
    else return 2;

    printf("\n%u failure%s.\n", gFailures, gFailures == 1 ? "" : "s");
    return gFailures ? 1 : 0;
}
