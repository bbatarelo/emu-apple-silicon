/*
 * E-MU Tracker Pre -- Core Audio HAL plug-in.
 *
 * Publishes the device to Core Audio so every application can use it. Runs in
 * the audio driver-host helper, signed with an ordinary Apple Development
 * certificate, and needs no DriverKit entitlement. See docs/path-without-apple.md
 * for the measurements that established this route.
 *
 * The seam to the hardware is deliberately narrow: StartIO and StopIO own the
 * engine's lifetime, GetZeroTimeStamp republishes its clock, DoIOOperation
 * moves samples through timeline-indexed rings. Everything else is the
 * property surface.
 *
 * Timing is the device's own clock, end to end: the engine anchors the
 * timeline to the scheduled bus start before StartIO returns and to hardware
 * completion timestamps thereafter. The host clock appears only as the
 * switchable diagnostic (kEMUProperty_ClockSource) and never as a silent
 * fallback -- a timeline spliced between clocks is an audible dropout.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "usb_engine.h"
#include "../shared/usb_util.h"
#include "../shared/device.h"

#define LOG_SUBSYSTEM "net.quantum-bit.EMUTrackerPre"

static os_log_t emu_log(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create(LOG_SUBSYSTEM, "plugin"); });
    return log;
}

/* Named for whatever is actually attached, not for the model this driver was
 * first written against. */
/* Named for the device the message is about. Plug-in-wide messages have no
 * device in hand and say so, rather than borrowing whichever name a shared
 * global happened to hold -- with two published, that was always one of them
 * and wrong for the other. */
typedef struct Device Device;
static const char* device_log_name(const Device* dev);

#define EMU_LOG(fmt, ...) \
    os_log(emu_log(), "%{public}s: " fmt, device_log_name(dev), ##__VA_ARGS__)
#define EMU_LOG_PLUGIN(fmt, ...) \
    os_log(emu_log(), "EMU driver: " fmt, ##__VA_ARGS__)

/* --- object model --------------------------------------------------------
 *
 * Core Audio addresses every property by object. The plug-in object is fixed;
 * a device's objects are computed from its slot, because the topology is no
 * longer static -- see DEVICE_OBJECT_BASE and DeviceRole below. The old fixed
 * constants are gone deliberately: keeping them alongside the arithmetic would
 * be two sources of truth for the same numbers, and the first device's block
 * happens to land on exactly the values they had.
 *
 * The device carries software volume and mute controls because this hardware
 * has no master output level -- only a headphone knob -- and without them
 * macOS greys out the volume slider entirely.
 */
enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,  /* 1 */
};

/* Range of the software fader. -96 dB is far enough below anything audible to
 * serve as the bottom of the slider. */
#define VOLUME_MIN_DB  (-96.0f)
#define VOLUME_MAX_DB  (0.0f)

/* Stable across models on purpose. The UID is how applications remember which
 * device they were told to use, and the plug-in publishes exactly one device
 * whichever member of the family is attached. The name below is what anyone
 * actually reads, and that does follow the hardware. */
#define DEVICE_UID          "net.quantum-bit.EMUTrackerPre"
#define DEVICE_MANUFACTURER "E-MU Systems (revival)"

#define CHANNELS            2
/* Core Audio speaks Float32 to clients. The device's 24-bit packed format is a
 * conversion in the USB layer, not something applications should see. */
#define BYTES_PER_CHANNEL   4
#define BYTES_PER_FRAME     (CHANNELS * BYTES_PER_CHANNEL)

/* The cadence at which the timeline anchor advances, in frames. Purely a
 * reporting period -- the rings are addressed by absolute sample time and do
 * not wrap in step with it. */
/* Also the HAL's cap on a client's IO buffer -- see EMU_ZERO_TIMESTAMP_PERIOD,
 * which the engine sizes its schedule against. Not a free constant any more. */
#define RING_FRAMES         EMU_ZERO_TIMESTAMP_PERIOD

/*
 * Safety offsets: how far ahead of (output) or behind (input) the presented
 * "now" the hardware actually touches the data, published so Core Audio stays
 * clear of it. Specified in time and converted at the current rate -- the
 * hardware's reach is a property of the transport's milliseconds, not of a
 * frame count, so a fixed frame value would be wrong at every other rate.
 *
 * On output, "the hardware" is the packet: Core Audio converts its mix
 * straight into the USB request that will carry those frames, so the offset
 * is the whole budget and it is spent by one thread. Core Audio has from its
 * IO cycle until that request transmits; anything it has not written by then
 * goes out as the zeros the request was submitted with (`unfilledPlayback`,
 * `outputUnderruns`). No engine thread is on the data path, so nothing else
 * needs covering -- which is why this is 4 ms rather than the 10 ms a staged
 * fill needed to absorb IOUSBLib's completion jitter on top (FINDINGS).
 * Raising 'emuS' buys IO-thread tolerance directly, at a latency cost of the
 * same size. On input, the offset covers one request of completion
 * granularity plus the same jitter.
 */
#define OUTPUT_SAFETY_DEFAULT_US 4000
#define OUTPUT_SAFETY_MIN_US     4000
/* The engine sizes its schedule against this, so it is not a free knob:
 * see EMU_OUTPUT_SAFETY_MAX_US. */
#define OUTPUT_SAFETY_MAX_US     EMU_OUTPUT_SAFETY_MAX_US
#define INPUT_SAFETY_US          5000

/*
 * Presentation latency past the safety offset: the stretch of the path USB
 * cannot see -- bus to DAC going out, ADC to bus coming back.
 *
 * Measured, with a cable from the outputs to the inputs (`hal-loopback
 * latency`). A chirp is written into the output buffer at sample time So and
 * cross-correlated back out of the input buffer at sample time Si. Both are
 * positions on the timeline this driver itself publishes, so Si - So is this
 * quantity and nothing else: the safety offsets and the client's buffer decide
 * *when* Core Audio hands frames over, not where on the timeline they sit.
 * That shows up in the measurement -- it is repeatable to 0.00 frames within a
 * run, identical on both channels, and unchanged across IO buffers from 64 to
 * 2048 frames while the output cycle's lead over the input cycle moves from
 * 560 to 4528.
 *
 * Over 44.1, 48, 88.2 and 96 kHz the round trip is, to within 0.05 ms,
 *
 *     total = 68 frames + 4.23 ms
 *
 * and it takes both terms because the path holds two different kinds of delay.
 * A converter's group delay is a filter, so it is a fixed number of *frames*
 * whatever the rate; the device's own buffering is a fixed *time*. Fit either
 * one alone and it misses by milliseconds at one end of the range.
 *
 * The frame term matches the DAC and ADC figures the original kext derived
 * from the converter specifications -- 15 + 53 -- to a tenth of a frame, so
 * that split between the two directions is kept. The time term is halved
 * between them for want of anything better: a loopback yields only the sum,
 * and separating the directions needs a measurement that isolates one of them.
 *
 * The safety offset is deliberately absent. The HAL publishes it as its own
 * property and adds the two together itself.
 */
#define DAC_LATENCY_FRAMES  15
#define ADC_LATENCY_FRAMES  53
/* The fixed-time term, as a round trip. Specified in time and converted at the
 * current rate, for the same reason the safety offsets above are. */
#define INTERNAL_ROUND_TRIP_US 4230
#define INTERNAL_LATENCY_FRAMES \
    ((UInt32)(dev->sampleRate * INTERNAL_ROUND_TRIP_US / 2.0e6))

/*
 * Diagnostic counters, readable as custom properties on the device object.
 *
 * The plug-in runs inside a system daemon, so the usual ways of watching a
 * process do not apply, and os_log is only as reliable as whatever is querying
 * it. Exposing counters through the property surface means any client can ask
 * the driver what it has actually done. The real driver wants this anyway --
 * guidelines section 15 calls for diagnostics through a narrow interface.
 */
/*
 * One custom property carrying all of them as a dictionary. The HAL will not
 * forward selectors a plug-in has not declared through
 * kAudioObjectPropertyCustomPropertyInfoList -- it answers 'who?'
 * (kAudioHardwareUnknownPropertyError) instead -- and custom properties may
 * only be CFString or CFPropertyList, never a raw integer.
 */
enum {
    kEMUProperty_Diagnostics = 'emuD',
    /* Settable. Which clock Core Audio's timeline follows. */
    kEMUProperty_ClockSource = 'emuK',
    /* Settable. Writing anything zeroes the read-only counters. */
    kEMUProperty_ResetCounters = 'emuR',
    /* Settable. Output safety offset in microseconds, for trading robustness
     * against latency -- the same knob the original kext exposed under this
     * name. The engine takes its copy at the next StartIO; coreaudiod's
     * published copy only follows a coreaudiod restart (see FINDINGS), so
     * `hal-check safety` reports both. */
    kEMUProperty_SafetyOffset = 'emuS',
    /* Settable: "on", "off" or "auto". "on" opens the capture interface;
     * "off" leaves it unclaimed at alternate setting 0, so no IN transaction
     * reaches the bus and playback is sized from the feedback endpoint alone;
     * "auto" is "off" at 176.4 and 192 kHz and "on" below.
     *
     * The workaround for setups on which duplex at those two rates drops
     * playback packets while IN transactions are on the bus (FINDINGS). The
     * cost is the whole input direction, so it is a choice rather than a fix.
     * Read once per StartIO; changing it under a running stream restarts the
     * stream. */
    kEMUProperty_InputMode = 'emuI',
    /* Settable, testing only. Injects a transport fault so the recovery path
     * can be exercised without unplugging anything: "transient" should be
     * rebuilt through, "persistent" should exhaust the retry budget and take
     * the device offline. */
    kEMUProperty_FaultInject = 'emuX',
};

/*
 * Where GetZeroTimeStamp gets its anchor.
 *
 * DEVICE is correct: it follows frames the hardware has actually consumed, so
 * the two clocks cannot drift apart. It is also coarser, because the anchor only
 * moves when an isochronous request completes -- every 8 ms -- and Core Audio
 * extrapolates in between.
 *
 * HOST is smooth but wrong: it advances on mach_absolute_time, so any difference
 * between the two clocks accumulates in the ring until it breaks. At a few ppm
 * that takes hours.
 *
 * Switchable at runtime because which one sounds better is a question about this
 * machine and this workload, not one to answer from first principles.
 */

/* Every rate the hardware supports, from the descriptors. */
static const Float64 kSupportedRates[] = {
    44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0
};
#define NUM_RATES (sizeof(kSupportedRates) / sizeof(kSupportedRates[0]))

/* --- state ---------------------------------------------------------------- */

static pthread_mutex_t   gStateMutex     = PTHREAD_MUTEX_INITIALIZER;
static UInt32            gRefCount       = 0;
static AudioServerPlugInHostRef gHost    = NULL;

typedef enum { kInputMode_Auto = 0, kInputMode_On, kInputMode_Off } InputMode;
typedef enum {
    kClockSource_Device = 0,
    kClockSource_Host   = 1,
} ClockSource;

/*
 * One published device.
 *
 * Everything here used to be file scope, which is why the driver could only
 * ever speak for one box: a second unit had nowhere to keep its rate, its
 * volume, its clock anchor or its client count. Object IDs are computed from
 * `base` rather than fixed, so an incoming AudioObjectID maps back to a device
 * and a role by arithmetic.
 */
#define EMU_MAX_DEVICES      4u
#define DEVICE_OBJECT_BASE   2u
#define DEVICE_OBJECT_STRIDE 5u

typedef enum {
    kRole_Device = 0,
    kRole_StreamInput,
    kRole_StreamOutput,
    kRole_VolumeOutput,
    kRole_MuteOutput,
    kRole_Count
} DeviceRole;

struct Device {
    bool                     present;
    unsigned                 index;
    AudioObjectID            base;      /* first object ID of this device's block */
    const EmuDeviceIdentity* identity;
    uint16_t                 unitProductID;
    uint64_t                 unitLocationID;
    char                     uid[160];
    EmuEngine* engine;
    _Atomic bool deviceAlive;
    Float64 sampleRate;
    UInt32 iOClients;
    Boolean inputActive;
    Boolean outputActive;
    InputMode inputMode;
    Float64 policyRate;
    ClockSource clockSource;
    Float32 volumeScalar;
    Boolean muted;
    UInt32 outputSafetyUS;
    _Atomic uint64_t startIOCount;
    _Atomic uint64_t iOCycles;
    _Atomic uint64_t framesOut;
    UInt64 anchorHostTime;
    UInt64 anchorJitterNs;
    UInt64 anchorJitterMaxNs;
    UInt64 anchorUpdates;
    UInt64 deficitBaseline;
    UInt64 resetCount;
    UInt64 timelineSeed;
    UInt64 periodCount;
};

static Device   gDevices[EMU_MAX_DEVICES];
static unsigned gDeviceCount;

static inline AudioObjectID device_object(const Device* dev, DeviceRole role)
{
    return dev->base + (AudioObjectID)role;
}

/*
 * What an object ID denotes: the plug-in itself, or one role of one device.
 *
 * The property handlers switch over "which object is this", and with computed
 * IDs that can no longer be a switch over the ID: device 1's stream is a
 * different number from device 0's but wants the same code. Classifying first
 * lets the switches stay switches, over the role rather than the number.
 * Negative values keep the plug-in and the unknown case clear of DeviceRole,
 * whose members start at zero.
 */
enum { kObjKind_Unknown = -2, kObjKind_PlugIn = -1 };

static Device* device_for_object(AudioObjectID id, DeviceRole* role);

static int object_kind(AudioObjectID object, Device** out_dev)
{
    if (object == kObjectID_PlugIn) { if (out_dev) *out_dev = NULL; return kObjKind_PlugIn; }
    DeviceRole role = kRole_Device;
    Device* dev = device_for_object(object, &role);
    if (out_dev) *out_dev = dev;
    return dev ? (int)role : kObjKind_Unknown;
}

/* Maps an object ID back to the device that owns it. NULL for the plug-in
 * object and for anything out of range, which callers answer with
 * kAudioHardwareBadObjectError exactly as they did when there was one device. */
static Device* device_for_object(AudioObjectID id, DeviceRole* role)
{
    if (id < DEVICE_OBJECT_BASE) return NULL;
    unsigned off = (unsigned)(id - DEVICE_OBJECT_BASE);
    unsigned idx = off / DEVICE_OBJECT_STRIDE;
    unsigned r   = off % DEVICE_OBJECT_STRIDE;
    if (idx >= EMU_MAX_DEVICES || r >= (unsigned)kRole_Count) return NULL;
    if (!gDevices[idx].present) return NULL;
    if (role) *role = (DeviceRole)r;
    return &gDevices[idx];
}


/* The transport for the one device this plug-in currently publishes. Stage 2
 * replaces it with a registry keyed by AudioObjectID, at which point nothing
 * else here has to change: every call already goes through the handle. */

/*
 * Whether the transport is still there.
 *
 * This answered a hard-coded 1, which is true right up until it is
 * catastrophically false: the engine can be gone while Core Audio goes on
 * calling DoIOOperation and every byte the client writes lands in requests
 * nobody is transmitting. Core Audio has no other way to be told, so this is
 * the one signal that ends that state.
 */

static void device_set_alive(Device* dev, bool alive)
{
    bool was = atomic_exchange(&dev->deviceAlive, alive);
    if (was == alive || !gHost) return;

    EMU_LOG("device marked %{public}s", alive ? "alive" : "NOT alive");
    AudioObjectPropertyAddress changed[] = {
        { kAudioDevicePropertyDeviceIsAlive,
          kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
        { kAudioDevicePropertyDeviceIsRunning,
          kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
    };
    gHost->PropertiesChanged(gHost, device_object(dev, kRole_Device), 2, changed);
}

/* Engine thread, once, when it has exhausted its rebuild attempts. */
static void engine_failed(void* context)
{
    Device* dev = (Device*)context;
    if (!dev) return;
    EMU_LOG("USB engine gave up; the device is unusable until it returns");
    device_set_alive(dev, false);
}

/* Whether a stream opens the capture direction at all. See
 * kEMUProperty_InputMode. Under gStateMutex; the effective answer is
 * input_wanted_locked(dev), read once per StartIO. */

/* The rate at and above which `auto` stops opening capture: 176.4 and
 * 192 kHz, the two rates that exist only at bInterval 3 and the two at which
 * duplex has been seen to drop playback packets (FINDINGS). Below it there is
 * nothing to trade away. */
#define EMU_INPUT_AUTO_OFF_HZ 176400.0

/* The rate `auto` decides against, updated when the rate is *requested* rather
 * than when it is performed.
 *
 * The HAL defers PerformDeviceConfigurationChange while no IO is running -- it
 * can be minutes, or until the next stream starts -- and until it runs,
 * dev->sampleRate still holds the old rate. Deriving the published input direction
 * from dev->sampleRate therefore left the device advertising an input it was about
 * to drop, for as long as the machine stayed quiet. Audio MIDI Setup reads the
 * layout exactly then. */

/* Atomics, because DoIOOperation runs on the real-time thread and must not take
 * the state lock. */

/* Timeline anchor. Advanced one ring period at a time so the sample clock and
 * the host clock stay tied together. */
/* Anchor jitter: how far each new timeline anchor lands from where the previous
 * one predicted. Diagnostics only; nothing depends on it. */
/* Deficit at the last reset, so the reported figure is what has accumulated
 * since rather than including everything from stream start. */

static Float64 host_ticks_per_second(void)
{
    static Float64 ticks;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        ticks = 1.0e9 * (Float64)tb.denom / (Float64)tb.numer;
    });
    return ticks;
}

/*
 * Scalar to linear amplitude, cubed.
 *
 * A slider that maps linearly to amplitude feels wrong: most of the audible
 * range crowds into the bottom of its travel. Cubing approximates perceived
 * loudness closely enough that the slider behaves the way people expect, and it
 * is the same shape Apple's own sample drivers use.
 */
static Float32 volume_scalar_to_gain(Float32 scalar)
{
    if (scalar <= 0.0f) return 0.0f;
    if (scalar >= 1.0f) return 1.0f;
    return scalar * scalar * scalar;
}

static Float32 volume_scalar_to_db(Float32 scalar)
{
    Float32 gain = volume_scalar_to_gain(scalar);
    if (gain <= 0.0f) return VOLUME_MIN_DB;
    Float32 db = 20.0f * log10f(gain);
    return db < VOLUME_MIN_DB ? VOLUME_MIN_DB : db;
}

static Float32 volume_db_to_scalar(Float32 db)
{
    if (db <= VOLUME_MIN_DB) return 0.0f;
    if (db >= VOLUME_MAX_DB) return 1.0f;
    return powf(powf(10.0f, db / 20.0f), 1.0f / 3.0f);
}

/* Gain the engine should apply, accounting for mute. Caller holds the lock. */
static void push_gain_to_engine(Device* dev)
{
    emu_engine_set_output_gain(dev->engine, dev->muted ? 0.0f : volume_scalar_to_gain(dev->volumeScalar));
}

static void fill_stream_format(Device* dev, AudioStreamBasicDescription* format)
{
    memset(format, 0, sizeof(*format));
    format->mSampleRate       = dev->sampleRate;
    format->mFormatID         = kAudioFormatLinearPCM;
    format->mFormatFlags      = kAudioFormatFlagIsFloat
                              | kAudioFormatFlagsNativeEndian
                              | kAudioFormatFlagIsPacked;
    format->mBytesPerPacket   = BYTES_PER_FRAME;
    format->mFramesPerPacket  = 1;
    format->mBytesPerFrame    = BYTES_PER_FRAME;
    format->mChannelsPerFrame = CHANNELS;
    format->mBitsPerChannel   = BYTES_PER_CHANNEL * 8;
}

/* --- forward declarations ------------------------------------------------- */

extern AudioServerPlugInDriverRef gEMUDriverRef;

/* --- COM plumbing --------------------------------------------------------- */

static HRESULT QueryInterface(void* driver, REFIID iid, LPVOID* out)
{
    (void)driver;
    if (!out) return E_POINTER;

    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT result = E_NOINTERFACE;
    if (requested) {
        if (CFEqual(requested, IUnknownUUID) ||
            CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
            pthread_mutex_lock(&gStateMutex);
            gRefCount++;
            pthread_mutex_unlock(&gStateMutex);
            *out = gEMUDriverRef;
            result = S_OK;
        }
        CFRelease(requested);
    }
    return result;
}

static ULONG AddRef(void* driver)
{
    (void)driver;
    pthread_mutex_lock(&gStateMutex);
    UInt32 n = ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return n;
}

static ULONG ReleaseRef(void* driver)
{
    (void)driver;
    pthread_mutex_lock(&gStateMutex);
    UInt32 n = gRefCount ? --gRefCount : 0;
    pthread_mutex_unlock(&gStateMutex);
    return n;
}

/* --- lifecycle ------------------------------------------------------------ */

/*
 * The device object is published only while hardware is attached.
 *
 * A HAL plug-in has no register/unregister call: it *is* its device list, and
 * Core Audio creates and destroys its device objects by re-reading that list
 * whenever the plug-in says it changed. So presence lives in the answers to
 * kAudioPlugInPropertyDeviceList and kAudioObjectPropertyOwnedObjects, and
 * this observer is what makes those answers reach Core Audio: it is told the
 * list changed on every arrival and departure. Without that, an unplugged
 * device stays in every device menu and can be picked as the default output,
 * where it cannot start.
 *
 * Every identity change is reported as a list change, not only the ones that
 * flip presence. A swap of one family member for another keeps the device
 * and only renames it; the extra list notification costs Core Audio one
 * re-read that finds the same object, and in exchange there is no
 * last-reported-presence state here to fall out of step with the engine's.
 *
 * Arrives on the engine's hot-plug queue; PropertiesChanged is callable from
 * any thread. What Core Audio does with a device it is withdrawing while IO
 * runs on it -- StopIO, client removal -- happens on its own threads
 * afterwards, the same as for any device that goes away.
 */

/* --- registry ------------------------------------------------------------
 *
 * The published set follows what is attached. A unit is matched to its slot by
 * location: the port is what distinguishes two boxes of the same model, and it
 * is stable for as long as the cable stays where it is, which is exactly the
 * lifetime of the slot.
 */

/*
 * A device's UID: the plug-in's prefix plus the unit's own serial.
 *
 * It has to identify the box, not the slot -- macOS remembers a user's chosen
 * output device by UID across reboots, and a per-slot name would move the
 * moment two devices were plugged in a different order. Both of these devices
 * report a serial (E-MU-69-3F04-... and E-MU-C7-3F0A-...), which survives
 * replugging and changing ports.
 *
 * Caller releases.
 */
static CFStringRef device_uid(const Device* dev)
{
    if (!dev || !dev->uid[0]) return NULL;
    return CFStringCreateWithCString(NULL, dev->uid, kCFStringEncodingUTF8);
}

static const char* device_log_name(const Device* dev)
{
    if (dev && dev->identity) return dev->identity->name;
    return "EMU driver";
}

static Device* slot_for_location(uint64_t location_id)
{
    for (unsigned i = 0; i < EMU_MAX_DEVICES; i++) {
        if (gDevices[i].present && gDevices[i].unitLocationID == location_id) {
            return &gDevices[i];
        }
    }
    return NULL;
}

/* Bring a slot up for a newly seen unit. Its object IDs come out of its index,
 * so slot 0 keeps the block the single-device build used. */
static void device_adopt(Device* dev, unsigned index, const EmuUnit* unit)
{
    memset(dev, 0, sizeof *dev);
    dev->index          = index;
    dev->base           = DEVICE_OBJECT_BASE + index * DEVICE_OBJECT_STRIDE;
    dev->identity       = unit->identity;
    dev->unitProductID  = unit->identity->product_id;
    dev->unitLocationID = unit->location_id;
    snprintf(dev->uid, sizeof dev->uid, "%s.%s", DEVICE_UID, unit->serial);

    dev->sampleRate     = 48000.0;
    dev->policyRate     = 48000.0;
    dev->inputActive    = true;
    dev->outputActive   = true;
    dev->inputMode      = kInputMode_On;
    dev->clockSource    = kClockSource_Device;
    dev->volumeScalar   = 1.0f;
    dev->muted          = false;
    dev->outputSafetyUS = OUTPUT_SAFETY_DEFAULT_US;
    dev->timelineSeed   = 1;
    atomic_init(&dev->deviceAlive, true);

    dev->engine = emu_engine_create(dev->unitProductID, dev->unitLocationID);
    if (dev->engine) {
        /* The handler carries this slot, so a failure names the device it
         * happened to rather than whichever was published first. */
        emu_engine_set_failure_handler(dev->engine, engine_failed, dev);
    }
    if (dev->engine) {
    
    }
    dev->present = true;
}

/* Take a slot down. The engine is stopped and freed outside the state lock,
 * because stopping joins its thread. */
static EmuEngine* device_retire_locked(Device* dev)
{
    EmuEngine* engine = dev->engine;
    dev->present = false;
    dev->engine  = NULL;
    return engine;
}

/*
 * Match the registry to the bus. Returns true if the published set changed, so
 * the caller can tell Core Audio to re-read the device list.
 */
static bool reconcile_devices(void)
{
    EmuUnit units[EMU_MAX_DEVICES];
    unsigned n = emu_enumerate_units(units, EMU_MAX_DEVICES);

    EmuEngine* retired[EMU_MAX_DEVICES];
    unsigned   retired_n = 0;
    bool changed = false;

    pthread_mutex_lock(&gStateMutex);

    for (unsigned i = 0; i < EMU_MAX_DEVICES; i++) {
        Device* dev = &gDevices[i];
        if (!dev->present) continue;
        bool still_there = false;
        for (unsigned u = 0; u < n; u++) {
            if (units[u].location_id == dev->unitLocationID) { still_there = true; break; }
        }
        if (!still_there) {
            retired[retired_n++] = device_retire_locked(dev);
            changed = true;
        }
    }

    for (unsigned u = 0; u < n; u++) {
        if (slot_for_location(units[u].location_id)) continue;
        for (unsigned i = 0; i < EMU_MAX_DEVICES; i++) {
            if (gDevices[i].present) continue;
            device_adopt(&gDevices[i], i, &units[u]);
            changed = true;
            break;
        }
    }

    unsigned count = 0;
    for (unsigned i = 0; i < EMU_MAX_DEVICES; i++) if (gDevices[i].present) count++;
    gDeviceCount = count;

    pthread_mutex_unlock(&gStateMutex);

    /* Outside the lock: stop joins the engine thread, and that thread may be
     * inside a callback that wants the same lock. */
    for (unsigned i = 0; i < retired_n; i++) emu_engine_destroy(retired[i]);

    return changed;
}

static void device_presence_changed(void)
{
    if (!gHost) return;
    reconcile_devices();

    AudioObjectPropertyAddress list[] = {
        { kAudioPlugInPropertyDeviceList,
          kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
        { kAudioObjectPropertyOwnedObjects,
          kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
    };
    gHost->PropertiesChanged(gHost, kObjectID_PlugIn, 2, list);

    /* Each published device is told separately: a device Core Audio keeps
     * across a change is not re-queried unasked, and with several of them the
     * first one's notification says nothing about the rest. */
    AudioObjectPropertyAddress name = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    for (unsigned i = 0; i < EMU_MAX_DEVICES; i++) {
        Device* dev = &gDevices[i];
        if (!dev->present) continue;
        gHost->PropertiesChanged(gHost, device_object(dev, kRole_Device), 1, &name);
    }
    EMU_LOG_PLUGIN("device set changed: %u published", gDeviceCount);
}

static OSStatus Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host)
{

    (void)driver;
    gHost = host;
    /* Arms the hot-plug watch, which resolves what is attached right now
     * before returning -- the host reads the device list next. */
    if (!emu_engine_set_identity_observer(device_presence_changed)) {
        /* Without the watch nothing is published, so there is nothing to
         * initialize; the engine has no look-up-once fallback on purpose.
         * Failing here puts the reason in the log next to the HAL's own
         * complaint, rather than leaving a plug-in that looks fine and lists
         * nothing. */
        EMU_LOG_PLUGIN("initialize failed: could not arm the hot-plug watch "
                "(IOKit notification port or matching notification), "
                "so no device will be published");
        gHost = NULL;
        return kAudioHardwareUnspecifiedError;
    }
    /* Publish whatever is attached right now; the observer keeps it matched
     * from here on. */
    reconcile_devices();
    if (gDeviceCount > 0) {
        EMU_LOG_PLUGIN("initialized, publishing %u device(s)", gDeviceCount);
    } else {
        EMU_LOG_PLUGIN("initialized, no device attached: publishing nothing until one is");
    }
    return kAudioHardwareNoError;
}

/* The topology is fixed, so Core Audio never asks the plug-in to create or
 * destroy a device; the one device comes and goes with the hardware through
 * the device list instead. */
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                             const AudioServerPlugInClientInfo* c, AudioObjectID* out)
{ (void)d; (void)desc; (void)c; (void)out; return kAudioHardwareUnsupportedOperationError; }

static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{ (void)d; (void)id; return kAudioHardwareUnsupportedOperationError; }

static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                const AudioServerPlugInClientInfo* c)
{
    (void)d; (void)c;
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    return (dev && role_ == kRole_Device) ? kAudioHardwareNoError
                                         : kAudioHardwareBadObjectError;
}

static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                const AudioServerPlugInClientInfo* c)
{
    (void)d; (void)c;
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    return (dev && role_ == kRole_Device) ? kAudioHardwareNoError
                                         : kAudioHardwareBadObjectError;
}

/* Whether this stream should open capture. Call with gStateMutex held.
 *
 * `on` is the default, so out of the box the driver is full duplex at every
 * rate, and the high-rate duplex instability -- which not every setup shows
 * -- is left as it is. Losing the input direction is not something to do to
 * someone who did not ask: a device that records is what the hardware is, and
 * a driver that quietly stopped being one at two of its six rates would be the
 * more surprising failure of the two. `auto` and `off` are for whoever needs
 * those rates clean and knows what it costs.
 *
 * Whichever mode is in force, it must not be silent: the input stream is
 * withdrawn (input_published) and reports kAudioStreamPropertyIsActive false
 * whenever this is false. */
static Boolean input_wanted_locked(Device* dev)
{
    switch (dev->inputMode) {
        case kInputMode_On:  return true;
        case kInputMode_Off: return false;
        case kInputMode_Auto:
        default:             return dev->policyRate < EMU_INPUT_AUTO_OFF_HZ;
    }
}

/* Tells Core Audio the input direction has appeared or disappeared.
 *
 * The stream list and the owned objects, not just the stream's active flag:
 * the device gains or loses its input channels with it, and nothing
 * re-queries a device on its own. Never call with gStateMutex held -- the host
 * may call back in. */
static void notify_input_visibility_changed(void)
{
    /* Stage 3: these fire from the hot-plug observer and the input-mode
     * setter, neither of which names a device yet. With several published
     * they have to notify each one, not just the first. */
    Device* dev = &gDevices[0];
    if (!gHost) return;
    AudioObjectPropertyAddress device[] = {
        { kAudioDevicePropertyStreams,
          kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
        { kAudioDevicePropertyStreams,
          kAudioObjectPropertyScopeInput,  kAudioObjectPropertyElementMain },
        { kAudioObjectPropertyOwnedObjects,
          kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
    };
    gHost->PropertiesChanged(gHost, device_object(dev, kRole_Device), 3, device);

    AudioObjectPropertyAddress active = {
        kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    gHost->PropertiesChanged(gHost, device_object(dev, kRole_StreamInput), 1, &active);
}

/* Rate changes arrive as a configuration change so Core Audio can quiesce IO
 * around them. The hardware SET_CUR and its read-back verification happen in
 * the engine when the stream next starts -- guidelines section 18. */
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d,
                                                 AudioObjectID id, UInt64 action, void* info)
{
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    (void)d; (void)info;
    if ((!dev || role_ != kRole_Device)) return kAudioHardwareBadObjectError;

    Float64 rate = (Float64)action;
    Boolean known = false;
    for (size_t i = 0; i < NUM_RATES; i++) {
        if (kSupportedRates[i] == rate) { known = true; break; }
    }
    if (!known) return kAudioHardwareUnsupportedOperationError;

    pthread_mutex_lock(&gStateMutex);
    Boolean was = input_wanted_locked(dev);
    dev->sampleRate = rate;
    dev->policyRate = rate;   /* for a change the HAL originated, not our setter */
    dev->anchorHostTime = 0;   /* the timeline restarts with the rate */
    dev->periodCount = 0;
    dev->timelineSeed++;
    Boolean now = input_wanted_locked(dev);
    pthread_mutex_unlock(&gStateMutex);

    /* Under `auto` the rate decides the input direction, so crossing 176.4 kHz
     * changes whether the input stream is running. Nothing re-queries a stream
     * on its own. */
    if (was != now) notify_input_visibility_changed();

    EMU_LOG("sample rate changed to %{public}.0f Hz, capture %{public}s",
            rate, now ? "on" : "off");
    return kAudioHardwareNoError;
}

static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d,
                                               AudioObjectID id, UInt64 action, void* info)
{ (void)d; (void)id; (void)action; (void)info; return kAudioHardwareNoError; }

/* --- property helpers ----------------------------------------------------- */

#define RETURN_U32(v) do {                                        \
        if (dataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError; \
        *(UInt32*)outData = (v); *outSize = sizeof(UInt32);        \
        return kAudioHardwareNoError;                              \
    } while (0)

#define RETURN_F64(v) do {                                        \
        if (dataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError; \
        *(Float64*)outData = (v); *outSize = sizeof(Float64);      \
        return kAudioHardwareNoError;                              \
    } while (0)

#define RETURN_CFSTR(v) do {                                      \
        if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError; \
        *(CFStringRef*)outData = CFSTR(v); *outSize = sizeof(CFStringRef); \
        return kAudioHardwareNoError;                              \
    } while (0)

/*
 * The device name is not a literal: it names whichever member of the family is
 * plugged in. Core Audio takes ownership of the string it is handed, so this
 * creates one per query rather than sharing a cached reference that the HAL
 * would eventually release out from under us.
 */
#define RETURN_CFSTR_UTF8(v) do {                                 \
        if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError; \
        CFStringRef s = CFStringCreateWithCString(NULL, (v), kCFStringEncodingUTF8); \
        if (!s) return kAudioHardwareUnspecifiedError;             \
        *(CFStringRef*)outData = s; *outSize = sizeof(CFStringRef); \
        return kAudioHardwareNoError;                              \
    } while (0)

/* Whether the input stream is published at all.
 *
 * kAudioStreamPropertyIsActive is what the API offers for "this stream is not
 * running", and it is not enough: nothing in Audio MIDI Setup surfaces it, and
 * a recording application simply reads zeroes. A device that cannot record has
 * to stop having an input direction -- no stream in the input scope, and so no
 * input channels -- which is a thing users and applications both understand.
 *
 * Takes gStateMutex itself: the property calls that need it do not hold it. */
static Boolean input_published(Device* dev)
{
    pthread_mutex_lock(&gStateMutex);
    Boolean published = input_wanted_locked(dev);
    pthread_mutex_unlock(&gStateMutex);
    return published;
}

static Boolean stream_matches_scope(Device* dev, AudioObjectID stream, AudioObjectPropertyScope scope)
{
    if (stream == device_object(dev, kRole_StreamInput) && !input_published(dev)) return false;
    if (scope == kAudioObjectPropertyScopeGlobal) return true;
    if (scope == kAudioObjectPropertyScopeInput)  return stream == device_object(dev, kRole_StreamInput);
    if (scope == kAudioObjectPropertyScopeOutput) return stream == device_object(dev, kRole_StreamOutput);
    return false;
}

/* Number of stream objects visible in a given scope. */
static UInt32 stream_count(Device* dev, AudioObjectPropertyScope scope)
{
    UInt32 n = 0;
    if (stream_matches_scope(dev, device_object(dev, kRole_StreamInput), scope))  n++;
    if (stream_matches_scope(dev, device_object(dev, kRole_StreamOutput), scope)) n++;
    return n;
}

/* --- HasProperty ---------------------------------------------------------- */

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID object,
                           pid_t client, const AudioObjectPropertyAddress* address)
{
    Device* dev = NULL;
    int kind_ = object_kind(object, &dev);
    DeviceRole role_ = kind_ >= 0 ? (DeviceRole)kind_ : kRole_Device;
    (void)role_;
    (void)d; (void)client;
    if (!address) return false;

    switch (kind_) {
        case kObjKind_PlugIn:
            switch (address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                case kAudioPlugInPropertyResourceBundle:
                    return true;
                default: return false;
            }

        case kRole_Device:
            switch (address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyRelatedDevices:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertyStreams:
                case kAudioObjectPropertyControlList:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates:
                case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyPreferredChannelsForStereo:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                case kAudioObjectPropertyCustomPropertyInfoList:
                case kEMUProperty_Diagnostics:
                case kEMUProperty_ClockSource:
                case kEMUProperty_ResetCounters:
                case kEMUProperty_SafetyOffset:
                case kEMUProperty_InputMode:
                case kEMUProperty_FaultInject:
                    return true;
                default: return false;
            }

        case kRole_VolumeOutput:
            switch (address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                case kAudioLevelControlPropertyScalarValue:
                case kAudioLevelControlPropertyDecibelValue:
                case kAudioLevelControlPropertyDecibelRange:
                case kAudioLevelControlPropertyConvertScalarToDecibels:
                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    return true;
                default: return false;
            }

        case kRole_MuteOutput:
            switch (address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                case kAudioBooleanControlPropertyValue:
                    return true;
                default: return false;
            }

        case kRole_StreamInput:
        case kRole_StreamOutput:
            switch (address->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    return true;
                default: return false;
            }

        default: return false;
    }
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID object,
                                   pid_t client, const AudioObjectPropertyAddress* address,
                                   Boolean* settable)
{
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(object, &role_);
    (void)d; (void)client;
    if (!address || !settable) return kAudioHardwareIllegalOperationError;

    *settable = false;
    if ((dev && role_ == kRole_Device) &&
        address->mSelector == kAudioDevicePropertyNominalSampleRate) {
        *settable = true;
    }
    if (((dev && role_ == kRole_StreamInput) || (dev && role_ == kRole_StreamOutput)) &&
        address->mSelector == kAudioStreamPropertyIsActive) {
        *settable = true;
    }
    if ((dev && role_ == kRole_VolumeOutput) &&
        (address->mSelector == kAudioLevelControlPropertyScalarValue ||
         address->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        *settable = true;
    }
    if ((dev && role_ == kRole_MuteOutput) &&
        address->mSelector == kAudioBooleanControlPropertyValue) {
        *settable = true;
    }
    if ((dev && role_ == kRole_Device) &&
        (address->mSelector == kEMUProperty_ClockSource ||
         address->mSelector == kEMUProperty_InputMode ||
         address->mSelector == kEMUProperty_ResetCounters ||
         address->mSelector == kEMUProperty_FaultInject)) {
        *settable = true;
    }
    if ((dev && role_ == kRole_Device) && address->mSelector == kEMUProperty_SafetyOffset) {
        *settable = true;
    }
    return kAudioHardwareNoError;
}

/* --- GetPropertyDataSize -------------------------------------------------- */

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID object,
                                    pid_t client, const AudioObjectPropertyAddress* address,
                                    UInt32 qualifierSize, const void* qualifier,
                                    UInt32* outSize)
{
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(object, &role_);
    (void)d; (void)client; (void)qualifierSize; (void)qualifier;
    if (!address || !outSize) return kAudioHardwareIllegalOperationError;

    switch (address->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
            *outSize = sizeof(AudioClassID); return kAudioHardwareNoError;

        case kAudioObjectPropertyOwner:
        case kAudioPlugInPropertyTranslateUIDToDevice:
            *outSize = sizeof(AudioObjectID); return kAudioHardwareNoError;

        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioPlugInPropertyResourceBundle:
            *outSize = sizeof(CFStringRef); return kAudioHardwareNoError;

        case kAudioObjectPropertyOwnedObjects:
            if (object == kObjectID_PlugIn) {
                *outSize = gDeviceCount * sizeof(AudioObjectID);
            } else if ((dev && role_ == kRole_Device)) {
                UInt32 n = stream_count(dev, address->mScope);
                if (stream_matches_scope(dev, device_object(dev, kRole_StreamOutput), address->mScope)) n += 2;
                *outSize = n * sizeof(AudioObjectID);
            } else { *outSize = 0; }
            return kAudioHardwareNoError;

        case kAudioPlugInPropertyDeviceList:
            *outSize = gDeviceCount * sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyStreams:
            *outSize = stream_count(dev, address->mScope) * sizeof(AudioObjectID);
            return kAudioHardwareNoError;

        case kAudioDevicePropertyRelatedDevices:
            *outSize = sizeof(AudioObjectID); return kAudioHardwareNoError;

        case kAudioObjectPropertyControlList:
            *outSize = 2 * sizeof(AudioObjectID); return kAudioHardwareNoError;

        case kAudioControlPropertyScope:
            *outSize = sizeof(AudioObjectPropertyScope); return kAudioHardwareNoError;
        case kAudioControlPropertyElement:
            *outSize = sizeof(AudioObjectPropertyElement); return kAudioHardwareNoError;

        case kAudioLevelControlPropertyScalarValue:
        case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            *outSize = sizeof(Float32); return kAudioHardwareNoError;
        case kAudioLevelControlPropertyDecibelRange:
            *outSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
        case kAudioBooleanControlPropertyValue:
            *outSize = sizeof(UInt32); return kAudioHardwareNoError;

        case kAudioDevicePropertyAvailableNominalSampleRates:
            *outSize = (UInt32)(NUM_RATES * sizeof(AudioValueRange));
            return kAudioHardwareNoError;

        case kAudioDevicePropertyNominalSampleRate:
            *outSize = sizeof(Float64); return kAudioHardwareNoError;

        case kAudioObjectPropertyCustomPropertyInfoList:
            *outSize = 6 * sizeof(AudioServerPlugInCustomPropertyInfo);
            return kAudioHardwareNoError;

        case kEMUProperty_Diagnostics:
        case kEMUProperty_SafetyOffset:
            *outSize = sizeof(CFPropertyListRef); return kAudioHardwareNoError;
        case kEMUProperty_ClockSource:
        case kEMUProperty_InputMode:
        case kEMUProperty_FaultInject:
        case kEMUProperty_ResetCounters:
            *outSize = sizeof(CFStringRef); return kAudioHardwareNoError;

        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outSize = 2 * sizeof(UInt32); return kAudioHardwareNoError;

        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outSize = sizeof(AudioStreamBasicDescription);
            return kAudioHardwareNoError;

        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outSize = (UInt32)(NUM_RATES * sizeof(AudioStreamRangedDescription));
            return kAudioHardwareNoError;

        default:
            *outSize = sizeof(UInt32); return kAudioHardwareNoError;
    }
}

/* --- GetPropertyData ------------------------------------------------------ */

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID object,
                                pid_t client, const AudioObjectPropertyAddress* address,
                                UInt32 qualifierSize, const void* qualifier,
                                UInt32 dataSize, UInt32* outSize, void* outData)
{
    Device* dev = NULL;
    int kind_ = object_kind(object, &dev);
    DeviceRole role_ = kind_ >= 0 ? (DeviceRole)kind_ : kRole_Device;
    (void)role_;
    (void)d; (void)client;
    if (!address || !outSize || !outData) return kAudioHardwareIllegalOperationError;

    switch (kind_) {

    /* ---------------- plug-in ---------------- */
    case kObjKind_PlugIn:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioObjectClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioPlugInClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(kAudioObjectUnknown);
            case kAudioObjectPropertyManufacturer: RETURN_CFSTR(DEVICE_MANUFACTURER);
            case kAudioPlugInPropertyResourceBundle: RETURN_CFSTR("");

            /* The device list is where presence is expressed: one device
             * while hardware is attached, none otherwise. See
             * device_presence_changed for how Core Audio learns it moved. */
            /* The device list is where presence is expressed: one entry per
             * attached unit, none when nothing is plugged in. See
             * device_presence_changed for how Core Audio learns it moved. */
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList: {
                AudioObjectID* ids = (AudioObjectID*)outData;
                UInt32 capacity = dataSize / (UInt32)sizeof(AudioObjectID);
                UInt32 n = 0;
                for (unsigned i = 0; i < EMU_MAX_DEVICES && n < capacity; i++) {
                    if (!gDevices[i].present) continue;
                    ids[n++] = device_object(&gDevices[i], kRole_Device);
                }
                *outSize = n * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }

            case kAudioPlugInPropertyTranslateUIDToDevice: {
                if (qualifierSize != sizeof(CFStringRef) || !qualifier) {
                    return kAudioHardwareIllegalOperationError;
                }
                CFStringRef uid = *(const CFStringRef*)qualifier;
                AudioObjectID match = kAudioObjectUnknown;
                for (unsigned i = 0; uid && i < EMU_MAX_DEVICES; i++) {
                    Device* cand = &gDevices[i];
                    if (!cand->present) continue;
                    CFStringRef own = device_uid(cand);
                    if (own && CFStringCompare(uid, own, 0) == kCFCompareEqualTo) {
                        match = device_object(cand, kRole_Device);
                    }
                    if (own) CFRelease(own);
                    if (match != kAudioObjectUnknown) break;
                }
                *(AudioObjectID*)outData = match;
                *outSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }

    /* ---------------- device ---------------- */
    case kRole_Device:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioObjectClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioDeviceClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(kObjectID_PlugIn);
            /* This device's own model, not "whatever is attached": with two
             * published, the global answer would name them both the same. */
            case kAudioObjectPropertyName:
                RETURN_CFSTR_UTF8(dev->identity ? dev->identity->name
                                                : emu_engine_device_name());
            case kAudioObjectPropertyManufacturer: RETURN_CFSTR(DEVICE_MANUFACTURER);
            case kAudioDevicePropertyDeviceUID: {
                CFStringRef uid = device_uid(dev);
                if (!uid) return kAudioHardwareUnspecifiedError;
                *(CFStringRef*)outData = uid;      /* caller owns it */
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            }
            /* The model is the same for every unit of a family, which is what
             * kAudioDevicePropertyModelUID means: it groups devices, it does
             * not name one. */
            case kAudioDevicePropertyModelUID:
                RETURN_CFSTR_UTF8(dev->identity ? dev->identity->name : DEVICE_UID ".model");

            case kAudioDevicePropertyTransportType: RETURN_U32(kAudioDeviceTransportTypeUSB);
            case kAudioDevicePropertyClockDomain:   RETURN_U32(0);
            /* Not a constant. When the engine exhausts its rebuild attempts
             * it declares the device dead, which is the only way to make Core
             * Audio stop handing audio to a transport that is gone: clients
             * are forced to re-open, and a re-open starts a fresh engine. */
            case kAudioDevicePropertyDeviceIsAlive:
                RETURN_U32(atomic_load(&dev->deviceAlive) ? 1 : 0);
            case kAudioDevicePropertyDeviceIsRunning: RETURN_U32(dev->iOClients > 0 ? 1 : 0);
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: RETURN_U32(1);
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RETURN_U32(1);
            case kAudioDevicePropertyIsHidden: RETURN_U32(0);
            case kAudioDevicePropertyZeroTimeStampPeriod: RETURN_U32(RING_FRAMES);

            case kAudioDevicePropertyLatency: {
                pthread_mutex_lock(&gStateMutex);
                UInt32 latency = INTERNAL_LATENCY_FRAMES
                               + (address->mScope == kAudioObjectPropertyScopeInput
                                      ? ADC_LATENCY_FRAMES : DAC_LATENCY_FRAMES);
                pthread_mutex_unlock(&gStateMutex);
                RETURN_U32(latency);
            }
            case kAudioDevicePropertySafetyOffset: {
                pthread_mutex_lock(&gStateMutex);
                UInt32 us = address->mScope == kAudioObjectPropertyScopeInput
                          ? INPUT_SAFETY_US : dev->outputSafetyUS;
                UInt32 frames = (UInt32)(dev->sampleRate * (Float64)us / 1.0e6);
                pthread_mutex_unlock(&gStateMutex);
                RETURN_U32(frames);
            }

            case kAudioDevicePropertyNominalSampleRate: RETURN_F64(dev->sampleRate);

            case kAudioObjectPropertyCustomPropertyInfoList: {
                AudioServerPlugInCustomPropertyInfo* info =
                    (AudioServerPlugInCustomPropertyInfo*)outData;
                UInt32 capacity = dataSize / sizeof(AudioServerPlugInCustomPropertyInfo);
                UInt32 n = 0;
                if (n < capacity) {
                    info[n].mSelector = kEMUProperty_Diagnostics;
                    info[n].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                    info[n].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
                    n++;
                }
                if (n < capacity) {
                    info[n].mSelector = kEMUProperty_ClockSource;
                    info[n].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFString;
                    info[n].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
                    n++;
                }
                if (n < capacity) {
                    info[n].mSelector = kEMUProperty_ResetCounters;
                    info[n].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFString;
                    info[n].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
                    n++;
                }
                if (n < capacity) {
                    info[n].mSelector = kEMUProperty_SafetyOffset;
                    info[n].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                    info[n].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
                    n++;
                }
                if (n < capacity) {
                    info[n].mSelector = kEMUProperty_InputMode;
                    info[n].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFString;
                    info[n].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
                    n++;
                }
                if (n < capacity) {
                    info[n].mSelector = kEMUProperty_FaultInject;
                    info[n].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFString;
                    info[n].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
                    n++;
                }
                *outSize = n * sizeof(AudioServerPlugInCustomPropertyInfo);
                return kAudioHardwareNoError;
            }

            case kEMUProperty_ResetCounters: {
                if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                UInt64 count = dev->resetCount;
                pthread_mutex_unlock(&gStateMutex);
                CFStringRef value = CFStringCreateWithFormat(
                    NULL, NULL, CFSTR("%llu"), (unsigned long long)count);
                *(CFStringRef*)outData = value;   /* caller owns it */
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            }

            case kEMUProperty_SafetyOffset: {
                if (dataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                long long us = dev->outputSafetyUS;
                pthread_mutex_unlock(&gStateMutex);
                /* The caller owns the returned reference. */
                CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongLongType, &us);
                if (!n) return kAudioHardwareUnspecifiedError;
                *(CFPropertyListRef*)outData = n;
                *outSize = sizeof(CFPropertyListRef);
                return kAudioHardwareNoError;
            }

            case kEMUProperty_ClockSource: {
                if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                ClockSource source = dev->clockSource;
                pthread_mutex_unlock(&gStateMutex);
                *(CFStringRef*)outData = (source == kClockSource_Host)
                    ? CFSTR("host") : CFSTR("device");
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            }

            case kEMUProperty_InputMode: {
                if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                InputMode mode = dev->inputMode;
                pthread_mutex_unlock(&gStateMutex);
                /* The mode, not the effect: what `auto` currently resolves to
                 * is the input stream's kAudioStreamPropertyIsActive. */
                *(CFStringRef*)outData = mode == kInputMode_On   ? CFSTR("on")
                                       : mode == kInputMode_Off  ? CFSTR("off")
                                                                 : CFSTR("auto");
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            }

            case kEMUProperty_FaultInject: {
                if (dataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                EmuEngineStats st;
                emu_engine_stats(dev->engine, &st);
                *(CFStringRef*)outData =
                    st.fault_mode == EMU_FAULT_TRANSIENT  ? CFSTR("transient")  :
                    st.fault_mode == EMU_FAULT_PERSISTENT ? CFSTR("persistent") : CFSTR("none");
                *outSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            }

            case kEMUProperty_Diagnostics: {
                if (dataSize < sizeof(CFPropertyListRef)) {
                    return kAudioHardwareBadPropertySizeError;
                }
                /* The caller owns the returned reference. */
                CFMutableDictionaryRef d = CFDictionaryCreateMutable(
                    NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                if (!d) return kAudioHardwareUnspecifiedError;

                EmuEngineStats engine;
                memset(&engine, 0, sizeof engine);
                emu_engine_stats(dev->engine, &engine);

                struct { CFStringRef key; uint64_t value; } entries[] = {
                    { CFSTR("startIOCalls"),   atomic_load_explicit(&dev->startIOCount, memory_order_relaxed) },
                    { CFSTR("ioCycles"),       atomic_load_explicit(&dev->iOCycles, memory_order_relaxed)     },
                    { CFSTR("framesToOutput"), atomic_load_explicit(&dev->framesOut, memory_order_relaxed)    },
                    { CFSTR("ioClients"),      (uint64_t)dev->iOClients        },
                    { CFSTR("engineRunning"),  emu_engine_running(dev->engine) ? 1u : 0u },
                    { CFSTR("framesPlayed"),   engine.frames_played        },
                    { CFSTR("outputLead"),     engine.output_lead          },
                    { CFSTR("outputUnderruns"), engine.underruns           },
                    { CFSTR("usbErrors"),      engine.usb_errors           },
                    { CFSTR("feedbackStarved"), engine.feedback_starved   },
                    /* Should stay 0: capture measurements the queue could not
                     * hold, which means the two directions have decoupled. */
                    { CFSTR("feedbackOverflows"), engine.feedback_overflows },
                    /* The explicit feedback endpoint, 0x81 -- the device's own
                     * statement of how many frames it wants per playback
                     * service interval, in Q16.16 as sent. `feedbackQ16`
                     * against `feedbackNominalQ16` is the disagreement between
                     * what the device asks for and what the rate says. It
                     * sizes the packets only while `inputEnabled` is 0. */
                    { CFSTR("feedbackPackets"),  engine.feedback_packets    },
                    { CFSTR("feedbackSilent"),   engine.feedback_silent     },
                    { CFSTR("feedbackErrors"),   engine.feedback_errors     },
                    { CFSTR("feedbackRejected"), engine.feedback_rejected   },
                    { CFSTR("feedbackChanges"),  engine.feedback_changes    },
                    { CFSTR("feedbackQ16"),      engine.feedback_q16        },
                    { CFSTR("feedbackMinQ16"),   engine.feedback_min_q16    },
                    { CFSTR("feedbackMaxQ16"),   engine.feedback_max_q16    },
                    { CFSTR("feedbackNominalQ16"), engine.feedback_nominal_q16 },
                    { CFSTR("tsFallbacks"),    engine.timestamp_fallbacks  },
                    { CFSTR("tsResets"),       engine.timestamp_resets     },
                    { CFSTR("resyncs"),        engine.resyncs              },
                    { CFSTR("deadFrames"),     engine.dead_frames          },
                    { CFSTR("unfilledPlayback"), engine.unfilled_playback  },
                    { CFSTR("shortPlayback"),  engine.short_playback       },
                    { CFSTR("safetyOffsetUS"), (uint64_t)dev->outputSafetyUS   },
                    /* The output path's own accounting. `framesBound` should
                     * track framesToOutput exactly; `unmappedFrames` is audio
                     * no queued request covered (a rebuild's dead interval,
                     * or -- if `unmappedAhead` is what moved -- a schedule
                     * too short for `writeLeadMax`); `bindRaces` is a write
                     * into a request recycled underneath it, and should be 0. */
                    { CFSTR("framesBound"),    engine.frames_bound         },
                    { CFSTR("unmappedFrames"), engine.unmapped_frames      },
                    { CFSTR("unmappedAhead"),  engine.unmapped_ahead       },
                    { CFSTR("writeLeadMax"),   engine.write_lead_max       },
                    { CFSTR("bindRaces"),      engine.bind_races           },
                    { CFSTR("scheduleRequests"), engine.schedule_requests  },
                    /* Should stay 0: the schedule may otherwise be short of
                     * the write lead. */
                    { CFSTR("scheduleClamped"), engine.schedule_clamped ? 1u : 0u },
                    /* Without it the capture counters below all read 0,
                     * which is indistinguishable from a clean capture run. */
                    { CFSTR("inputEnabled"),   engine.input_enabled ? 1u : 0u },
                    { CFSTR("framesCaptured"), engine.frames_captured      },
                    { CFSTR("inputDepth"),     engine.input_depth          },
                    { CFSTR("inputUnderruns"), engine.input_underruns      },
                    { CFSTR("inputOverruns"),  engine.input_overruns       },
                    { CFSTR("emptyCapture"),   engine.empty_capture        },
                    { CFSTR("clockSourceIsHost"), (uint64_t)(dev->clockSource == kClockSource_Host) },
                    { CFSTR("anchorUpdates"),     dev->anchorUpdates      },
                    { CFSTR("anchorJitterNs"),    dev->anchorJitterNs     },
                    { CFSTR("anchorJitterMaxNs"), dev->anchorJitterMaxNs  },
                    /* Frames the device consumed beyond what Core Audio handed
                     * over. The clearest health check there is: if it grows, the
                     * reported timeline is slower than the hardware and the ring
                     * will keep starving. */
                    /* Since the last reset, so a startup transient does not sit
                     * in the figure for the rest of the session. */
                    { CFSTR("frameDeficit"), (uint64_t)({
                          uint64_t out = atomic_load_explicit(&dev->framesOut, memory_order_relaxed);
                          uint64_t raw = engine.frames_played > out
                                       ? engine.frames_played - out : 0;
                          raw > dev->deficitBaseline ? raw - dev->deficitBaseline : 0; }) },
                    { CFSTR("counterResets"), dev->resetCount },
                    /* The counter whose absence cost a day and a half: with the
                     * engine dead every other figure here keeps advancing. */
                    { CFSTR("engineStreaming"), (uint64_t)engine.engine_streaming },
                    { CFSTR("engineAlive"),     (uint64_t)(atomic_load(&dev->deviceAlive) ? 1 : 0) },
                    { CFSTR("recoveries"),        engine.recoveries        },
                    { CFSTR("recoveryFailures"),  engine.recovery_failures },
                    { CFSTR("faultInjected"),     (uint64_t)engine.fault_mode },
                };
                for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
                    long long v = (long long)entries[i].value;
                    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongLongType, &v);
                    if (n) { CFDictionarySetValue(d, entries[i].key, n); CFRelease(n); }
                }

                *(CFPropertyListRef*)outData = d;
                *outSize = sizeof(CFPropertyListRef);
                return kAudioHardwareNoError;
            }

            case kAudioDevicePropertyRelatedDevices:
                *outSize = 0; return kAudioHardwareNoError;

            case kAudioObjectPropertyControlList: {
                AudioObjectID* ids = (AudioObjectID*)outData;
                UInt32 capacity = dataSize / sizeof(AudioObjectID);
                UInt32 n = 0;
                if (n < capacity) ids[n++] = device_object(dev, kRole_VolumeOutput);
                if (n < capacity) ids[n++] = device_object(dev, kRole_MuteOutput);
                *outSize = n * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }

            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams: {
                AudioObjectID* ids = (AudioObjectID*)outData;
                UInt32 capacity = dataSize / sizeof(AudioObjectID);
                UInt32 n = 0;
                if (stream_matches_scope(dev, device_object(dev, kRole_StreamInput), address->mScope) && n < capacity) {
                    ids[n++] = device_object(dev, kRole_StreamInput);
                }
                if (stream_matches_scope(dev, device_object(dev, kRole_StreamOutput), address->mScope) && n < capacity) {
                    ids[n++] = device_object(dev, kRole_StreamOutput);
                }
                /* Controls are owned objects too, but they are not streams. */
                if (address->mSelector == kAudioObjectPropertyOwnedObjects &&
                    stream_matches_scope(dev, device_object(dev, kRole_StreamOutput), address->mScope)) {
                    if (n < capacity) ids[n++] = device_object(dev, kRole_VolumeOutput);
                    if (n < capacity) ids[n++] = device_object(dev, kRole_MuteOutput);
                }
                *outSize = n * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }

            case kAudioDevicePropertyAvailableNominalSampleRates: {
                AudioValueRange* ranges = (AudioValueRange*)outData;
                UInt32 capacity = dataSize / sizeof(AudioValueRange);
                UInt32 n = 0;
                for (size_t i = 0; i < NUM_RATES && n < capacity; i++, n++) {
                    ranges[n].mMinimum = kSupportedRates[i];
                    ranges[n].mMaximum = kSupportedRates[i];
                }
                *outSize = n * sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }

            case kAudioDevicePropertyPreferredChannelsForStereo: {
                if (dataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                ((UInt32*)outData)[0] = 1;
                ((UInt32*)outData)[1] = 2;
                *outSize = 2 * sizeof(UInt32);
                return kAudioHardwareNoError;
            }

            default: return kAudioHardwareUnknownPropertyError;
        }

    /* ---------------- streams ---------------- */
    case kRole_StreamInput:
    case kRole_StreamOutput:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioObjectClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioStreamClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(device_object(dev, kRole_Device));
            case kAudioStreamPropertyDirection:
                RETURN_U32((dev && role_ == kRole_StreamInput) ? 1 : 0);
            case kAudioStreamPropertyTerminalType:
                RETURN_U32((dev && role_ == kRole_StreamInput)
                           ? kAudioStreamTerminalTypeMicrophone
                           : kAudioStreamTerminalTypeSpeaker);
            case kAudioStreamPropertyStartingChannel: RETURN_U32(1);
            case kAudioStreamPropertyLatency:         RETURN_U32(0);
            case kAudioStreamPropertyIsActive: {
                /* The input stream is active only if something also opened it
                 * on the USB side. A client that deactivated it stays
                 * deactivated; the mode can only take it away, never grant it.
                 */
                pthread_mutex_lock(&gStateMutex);
                UInt32 active = ((dev && role_ == kRole_StreamInput))
                              ? (dev->inputActive && input_wanted_locked(dev))
                              : dev->outputActive;
                pthread_mutex_unlock(&gStateMutex);
                RETURN_U32(active);
            }

            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                if (dataSize < sizeof(AudioStreamBasicDescription)) {
                    return kAudioHardwareBadPropertySizeError;
                }
                fill_stream_format(dev, (AudioStreamBasicDescription*)outData);
                *outSize = sizeof(AudioStreamBasicDescription);
                return kAudioHardwareNoError;
            }

            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription* formats = (AudioStreamRangedDescription*)outData;
                UInt32 capacity = dataSize / sizeof(AudioStreamRangedDescription);
                UInt32 n = 0;
                for (size_t i = 0; i < NUM_RATES && n < capacity; i++, n++) {
                    fill_stream_format(dev, &formats[n].mFormat);
                    formats[n].mFormat.mSampleRate = kSupportedRates[i];
                    formats[n].mSampleRateRange.mMinimum = kSupportedRates[i];
                    formats[n].mSampleRateRange.mMaximum = kSupportedRates[i];
                }
                *outSize = n * sizeof(AudioStreamRangedDescription);
                return kAudioHardwareNoError;
            }

            default: return kAudioHardwareUnknownPropertyError;
        }

    /* ---------------- controls ---------------- */
    case kRole_VolumeOutput:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioLevelControlClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioVolumeControlClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(device_object(dev, kRole_Device));
            case kAudioControlPropertyScope:    RETURN_U32(kAudioObjectPropertyScopeOutput);
            case kAudioControlPropertyElement:  RETURN_U32(kAudioObjectPropertyElementMain);

            case kAudioLevelControlPropertyScalarValue: {
                if (dataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                *(Float32*)outData = dev->volumeScalar;
                pthread_mutex_unlock(&gStateMutex);
                *outSize = sizeof(Float32);
                return kAudioHardwareNoError;
            }
            case kAudioLevelControlPropertyDecibelValue: {
                if (dataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                *(Float32*)outData = volume_scalar_to_db(dev->volumeScalar);
                pthread_mutex_unlock(&gStateMutex);
                *outSize = sizeof(Float32);
                return kAudioHardwareNoError;
            }
            case kAudioLevelControlPropertyDecibelRange: {
                if (dataSize < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                AudioValueRange* range = (AudioValueRange*)outData;
                range->mMinimum = VOLUME_MIN_DB;
                range->mMaximum = VOLUME_MAX_DB;
                *outSize = sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            /* These convert in place: the incoming value is the one to convert. */
            case kAudioLevelControlPropertyConvertScalarToDecibels: {
                if (dataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                *(Float32*)outData = volume_scalar_to_db(*(Float32*)outData);
                *outSize = sizeof(Float32);
                return kAudioHardwareNoError;
            }
            case kAudioLevelControlPropertyConvertDecibelsToScalar: {
                if (dataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                *(Float32*)outData = volume_db_to_scalar(*(Float32*)outData);
                *outSize = sizeof(Float32);
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }

    case kRole_MuteOutput:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioBooleanControlClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioMuteControlClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(device_object(dev, kRole_Device));
            case kAudioControlPropertyScope:    RETURN_U32(kAudioObjectPropertyScopeOutput);
            case kAudioControlPropertyElement:  RETURN_U32(kAudioObjectPropertyElementMain);
            case kAudioBooleanControlPropertyValue: {
                pthread_mutex_lock(&gStateMutex);
                UInt32 muted = dev->muted ? 1 : 0;
                pthread_mutex_unlock(&gStateMutex);
                RETURN_U32(muted);
            }
            default: return kAudioHardwareUnknownPropertyError;
        }

    default:
        return kAudioHardwareBadObjectError;
    }
}

/* --- SetPropertyData ------------------------------------------------------ */

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID object,
                                pid_t client, const AudioObjectPropertyAddress* address,
                                UInt32 qualifierSize, const void* qualifier,
                                UInt32 dataSize, const void* data)
{
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(object, &role_);
    (void)d; (void)client; (void)qualifierSize; (void)qualifier;
    if (!address || !data) return kAudioHardwareIllegalOperationError;

    if ((dev && role_ == kRole_Device) &&
        address->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (dataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 requested = *(const Float64*)data;

        for (size_t i = 0; i < NUM_RATES; i++) {
            if (kSupportedRates[i] != requested) continue;

            /* The input direction follows the rate under `auto`, and it has to
             * follow it now rather than when the change is performed: the HAL
             * defers that indefinitely while nothing is playing, which is
             * exactly when a user is in Audio MIDI Setup looking at the
             * device. */
            pthread_mutex_lock(&gStateMutex);
            Boolean was = input_wanted_locked(dev);
            dev->policyRate = requested;
            Boolean now = input_wanted_locked(dev);
            pthread_mutex_unlock(&gStateMutex);
            if (was != now) notify_input_visibility_changed();

            /* Ask the host to quiesce IO and call back into
             * PerformDeviceConfigurationChange, rather than switching underneath
             * a running stream. */
            if (gHost) {
                gHost->RequestDeviceConfigurationChange(gHost, device_object(dev, kRole_Device),
                                                        (UInt64)requested, NULL);
            }
            return kAudioHardwareNoError;
        }
        return kAudioHardwareIllegalOperationError;
    }

    if (((dev && role_ == kRole_StreamInput) || (dev && role_ == kRole_StreamOutput)) &&
        address->mSelector == kAudioStreamPropertyIsActive) {
        if (dataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        Boolean active = *(const UInt32*)data != 0;
        pthread_mutex_lock(&gStateMutex);
        if ((dev && role_ == kRole_StreamInput)) dev->inputActive = active;
        else                                  dev->outputActive = active;
        pthread_mutex_unlock(&gStateMutex);
        return kAudioHardwareNoError;
    }

    if ((dev && role_ == kRole_VolumeOutput) &&
        (address->mSelector == kAudioLevelControlPropertyScalarValue ||
         address->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        if (dataSize != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;

        Float32 scalar = address->mSelector == kAudioLevelControlPropertyScalarValue
                       ? *(const Float32*)data
                       : volume_db_to_scalar(*(const Float32*)data);
        if (scalar < 0.0f) scalar = 0.0f;
        if (scalar > 1.0f) scalar = 1.0f;

        pthread_mutex_lock(&gStateMutex);
        dev->volumeScalar = scalar;
        push_gain_to_engine(dev);
        pthread_mutex_unlock(&gStateMutex);

        /* Both representations changed, whichever one was written. Without this
         * the slider does not follow the keyboard keys, and vice versa. */
        if (gHost) {
            AudioObjectPropertyAddress changed[] = {
                { kAudioLevelControlPropertyScalarValue,
                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                { kAudioLevelControlPropertyDecibelValue,
                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            };
            gHost->PropertiesChanged(gHost, device_object(dev, kRole_VolumeOutput), 2, changed);
        }
        return kAudioHardwareNoError;
    }

    if ((dev && role_ == kRole_Device) && address->mSelector == kEMUProperty_ResetCounters) {
        emu_engine_reset_counters(dev->engine);

        EmuEngineStats engine;
        memset(&engine, 0, sizeof engine);
        emu_engine_stats(dev->engine, &engine);

        pthread_mutex_lock(&gStateMutex);
        dev->anchorJitterNs = 0;
        dev->anchorJitterMaxNs = 0;
        dev->anchorUpdates = 0;
        atomic_store_explicit(&dev->iOCycles, 0, memory_order_relaxed);
        /* Rebase rather than zero: frames_played cannot be reset without
         * breaking the timeline, so record where the deficit stands now. */
        /* framesToOutput is zeroed with framesBound, not left running: the
         * health check the diagnostics prescribe is framesBound + unmapped ==
         * framesToOutput, and windowing one side of it and not the other made
         * that comparison false for the rest of the session. The deficit
         * rebases onto the same instant, so it still reads as a delta. */
        atomic_store_explicit(&dev->framesOut, 0, memory_order_relaxed);
        dev->deficitBaseline = engine.frames_played;
        dev->resetCount++;
        pthread_mutex_unlock(&gStateMutex);

        EMU_LOG("counters reset");
        return kAudioHardwareNoError;
    }

    if ((dev && role_ == kRole_Device) && address->mSelector == kEMUProperty_ClockSource) {
        if (dataSize != sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
        CFStringRef requested = *(const CFStringRef*)data;
        if (!requested) return kAudioHardwareIllegalOperationError;

        ClockSource source;
        if (CFStringCompare(requested, CFSTR("host"), kCFCompareCaseInsensitive)
            == kCFCompareEqualTo) {
            source = kClockSource_Host;
        } else if (CFStringCompare(requested, CFSTR("device"), kCFCompareCaseInsensitive)
                   == kCFCompareEqualTo) {
            source = kClockSource_Device;
        } else {
            return kAudioHardwareIllegalOperationError;
        }

        pthread_mutex_lock(&gStateMutex);
        if (dev->clockSource != source) {
            dev->clockSource = source;
            /* Bump the seed so Core Audio discards whatever it had extrapolated
             * from the previous anchor rather than splicing two timelines, and
             * restart the anchor so the incoming path does not inherit one the
             * other path derived. */
            dev->timelineSeed++;
            dev->anchorHostTime = 0;
            dev->anchorJitterNs = 0;
            dev->anchorJitterMaxNs = 0;
            dev->anchorUpdates = 0;
        }
        pthread_mutex_unlock(&gStateMutex);

        EMU_LOG("clock source set to %{public}s",
                source == kClockSource_Host ? "host" : "device");
        return kAudioHardwareNoError;
    }

    if ((dev && role_ == kRole_Device) && address->mSelector == kEMUProperty_FaultInject) {
        if (dataSize != sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
        CFStringRef requested = *(const CFStringRef*)data;
        if (!requested) return kAudioHardwareIllegalOperationError;

        EmuFaultMode mode;
        if (CFStringCompare(requested, CFSTR("transient"), kCFCompareCaseInsensitive)
            == kCFCompareEqualTo) {
            mode = EMU_FAULT_TRANSIENT;
        } else if (CFStringCompare(requested, CFSTR("persistent"), kCFCompareCaseInsensitive)
                   == kCFCompareEqualTo) {
            mode = EMU_FAULT_PERSISTENT;
        } else if (CFStringCompare(requested, CFSTR("none"), kCFCompareCaseInsensitive)
                   == kCFCompareEqualTo) {
            mode = EMU_FAULT_NONE;
        } else {
            return kAudioHardwareIllegalOperationError;
        }

        /* Clearing a fault also revives a device the engine gave up on, so a
         * test can put the driver back without restarting coreaudiod. */
        if (mode == EMU_FAULT_NONE && !atomic_load(&dev->deviceAlive)) {
            device_set_alive(dev, true);
        }
        emu_engine_inject_fault(dev->engine, mode);
        EMU_LOG("fault injection set to %{public}s",
                mode == EMU_FAULT_TRANSIENT  ? "transient" :
                mode == EMU_FAULT_PERSISTENT ? "persistent" : "none");
        return kAudioHardwareNoError;
    }

    if ((dev && role_ == kRole_Device) && address->mSelector == kEMUProperty_InputMode) {
        if (dataSize != sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
        CFStringRef requested = *(const CFStringRef*)data;
        if (!requested) return kAudioHardwareIllegalOperationError;

        InputMode mode;
        if (CFStringCompare(requested, CFSTR("on"), kCFCompareCaseInsensitive)
            == kCFCompareEqualTo) {
            mode = kInputMode_On;
        } else if (CFStringCompare(requested, CFSTR("off"), kCFCompareCaseInsensitive)
                   == kCFCompareEqualTo) {
            mode = kInputMode_Off;
        } else if (CFStringCompare(requested, CFSTR("auto"), kCFCompareCaseInsensitive)
                   == kCFCompareEqualTo) {
            mode = kInputMode_Auto;
        } else {
            return kAudioHardwareIllegalOperationError;
        }

        pthread_mutex_lock(&gStateMutex);
        Boolean was = input_wanted_locked(dev);
        dev->inputMode = mode;
        Boolean now = input_wanted_locked(dev);
        Float64 rate = dev->sampleRate;
        pthread_mutex_unlock(&gStateMutex);

        if (was != now) {
            notify_input_visibility_changed();
            /* Claiming or releasing an interface cannot happen underneath a
             * running stream: it rebuilds the schedule, which is a dropout on
             * the output side -- the side this mode exists to protect. Ask the
             * HAL to stop IO, re-apply the rate it already has, and start
             * again, which is the same path a rate change takes and gives two
             * clean streams either side of the switch. */
            if (gHost) {
                gHost->RequestDeviceConfigurationChange(gHost, device_object(dev, kRole_Device),
                                                        (UInt64)rate, NULL);
            }
        }
        EMU_LOG("input mode set to %{public}s, capture %{public}s",
                mode == kInputMode_On ? "on" : mode == kInputMode_Off ? "off" : "auto",
                now ? "on" : "off");
        return kAudioHardwareNoError;
    }

    if ((dev && role_ == kRole_Device) && address->mSelector == kEMUProperty_SafetyOffset) {
        if (dataSize != sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
        CFPropertyListRef value = *(const CFPropertyListRef*)data;
        if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) {
            return kAudioHardwareIllegalOperationError;
        }
        long long us = 0;
        CFNumberGetValue((CFNumberRef)value, kCFNumberLongLongType, &us);
        if (us < OUTPUT_SAFETY_MIN_US) us = OUTPUT_SAFETY_MIN_US;
        if (us > OUTPUT_SAFETY_MAX_US) us = OUTPUT_SAFETY_MAX_US;

        pthread_mutex_lock(&gStateMutex);
        dev->outputSafetyUS = (UInt32)us;
        pthread_mutex_unlock(&gStateMutex);

        /* The value Core Audio actually consumes changed with it. Note that
         * coreaudiod keeps its own copy of a device's safety offset and,
         * measured, does not refresh it on this notification -- nor on
         * DeviceHasChanged, nor across a stream restart; only a coreaudiod
         * restart does. The engine's copy follows at the next StartIO.
         * `hal-check safety` shows both views. See FINDINGS. */
        if (gHost) {
            AudioObjectPropertyAddress changed[] = {
                { kAudioDevicePropertySafetyOffset,
                  kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain },
                { kEMUProperty_SafetyOffset,
                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            };
            gHost->PropertiesChanged(gHost, device_object(dev, kRole_Device), 2, changed);
        }
        EMU_LOG("output safety offset set to %{public}lld us", us);
        return kAudioHardwareNoError;
    }

    if ((dev && role_ == kRole_MuteOutput) &&
        address->mSelector == kAudioBooleanControlPropertyValue) {
        if (dataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;

        pthread_mutex_lock(&gStateMutex);
        dev->muted = *(const UInt32*)data != 0;
        push_gain_to_engine(dev);
        pthread_mutex_unlock(&gStateMutex);

        if (gHost) {
            AudioObjectPropertyAddress changed = {
                kAudioBooleanControlPropertyValue,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
            };
            gHost->PropertiesChanged(gHost, device_object(dev, kRole_MuteOutput), 1, &changed);
        }
        return kAudioHardwareNoError;
    }

    return kAudioHardwareUnsupportedOperationError;
}

/* --- IO ------------------------------------------------------------------- */

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    (void)d; (void)client;
    if ((!dev || role_ != kRole_Device)) return kAudioHardwareBadObjectError;

    atomic_fetch_add_explicit(&dev->startIOCount, 1, memory_order_relaxed);

    bool revived = false;
    pthread_mutex_lock(&gStateMutex);
    if (dev->iOClients == 0) {
        /* Blocks -- tens of milliseconds -- until the streams are scheduled on
         * the bus and the engine has published the timeline anchor for sample
         * 0. StartIO is off the IO path, so a slow start here is the sanctioned
         * kind, and it buys the property that matters: the very first
         * GetZeroTimeStamp is already on the device's clock. A host-clock
         * guess spliced to the device clock later stalls coreaudiod's IO
         * thread for the length of the discrepancy (FINDINGS). */
        /* gStateMutex is held here, so the effective answer is read against
         * the same rate the engine is about to be started at. */
        if (!emu_engine_start(dev->engine, (uint32_t)dev->sampleRate, dev->outputSafetyUS,
                              input_wanted_locked(dev))) {
            pthread_mutex_unlock(&gStateMutex);
            EMU_LOG("IO start failed: USB engine did not come up");
            return kAudioHardwareNotRunningError;
        }

        /* Seed the host-clock fallback from the same anchor, so even the
         * diagnostic "host" clock source starts on the device's timeline
         * rather than at whatever mach_absolute_time StartIO ran at. */
        uint64_t frames = 0, host = 0;
        dev->anchorHostTime = emu_engine_timeline(dev->engine, &frames, &host) ? host : mach_absolute_time();
        dev->periodCount = 0;
        /* The one legitimate new timeline: a new session. Mid-stream the
         * engine keeps its line continuous through anything the bus does,
         * so nothing else ever changes the seed. */
        dev->timelineSeed++;
        push_gain_to_engine(dev);
        EMU_LOG("IO started at %{public}.0f Hz", dev->sampleRate);
        revived = true;
    }
    dev->iOClients++;
    pthread_mutex_unlock(&gStateMutex);

    /* Outside the lock: PropertiesChanged re-enters the plug-in. A device the
     * engine gave up on is alive again the moment a fresh engine is on the
     * bus, which is what makes a client re-open the way back. */
    if (revived) device_set_alive(dev, true);
    return kAudioHardwareNoError;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    (void)d; (void)client;
    if ((!dev || role_ != kRole_Device)) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gStateMutex);
    bool last = (dev->iOClients > 0 && --dev->iOClients == 0);
    if (last) {
        /* Keep the client-count transition and the join indivisible from a
         * new StartIO. The engine does not acquire plug-in state, and StopIO
         * is already the control-thread operation that joins it. */
        emu_engine_stop(dev->engine);
    }
    pthread_mutex_unlock(&gStateMutex);
    if (last) EMU_LOG("IO stopped");
    return kAudioHardwareNoError;
}

/*
 * Anchors Core Audio's sample timeline to the device's clock.
 *
 * The device decides how fast audio actually moves; the host clock only
 * approximates it. Anchoring to the host means Core Audio and the hardware
 * accumulate a difference that has to go somewhere, and where it goes is the
 * rings -- a drift the timeline-indexed slots turn into one glitch per lap.
 * Anchoring to frames the device has genuinely consumed removes the
 * disagreement rather than absorbing it.
 *
 * The engine publishes its anchor before StartIO returns -- initially the
 * scheduled bus time of sample 0, then per-request hardware timestamps through
 * the Rust core's filter -- so every call lands in the device branch with a
 * valid pair. This function must never splice timelines: a discontinuous
 * handoff under one seed (a host-clock guess replaced by the device clock)
 * stalls coreaudiod's IO thread for the length of the discrepancy, which is
 * an audible dropout. Nor does it ever need a new seed
 * mid-stream: a stall that forces the engine to rebuild its bus schedule is
 * accounted as frames the device consumed while nothing reached it, so the
 * anchor stays on the same line. A seed change is not a cheaper way out --
 * it makes the HAL resynchronise its IO thread, a glitch of its own.
 *
 * Core Audio expects the anchor to advance in whole ZeroTimeStampPeriod steps,
 * so this reports the most recent period boundary the device has crossed, and
 * interpolates back from the last completion to estimate when that crossing
 * happened.
 */
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id,
                                 UInt32 client, Float64* sampleTime, UInt64* hostTime,
                                 UInt64* seed)
{
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    (void)d; (void)client;
    if ((!dev || role_ != kRole_Device)) return kAudioHardwareBadObjectError;
    if (!sampleTime || !hostTime || !seed) return kAudioHardwareIllegalOperationError;

    pthread_mutex_lock(&gStateMutex);
    bool followDevice = (dev->clockSource == kClockSource_Device);
    pthread_mutex_unlock(&gStateMutex);

    uint64_t deviceFrames = 0, deviceHost = 0;
    if (followDevice && emu_engine_timeline(dev->engine, &deviceFrames, &deviceHost)) {
        pthread_mutex_lock(&gStateMutex);

        Float64 ticksPerFrame = host_ticks_per_second() / dev->sampleRate;

        UInt64 period = deviceFrames / RING_FRAMES;
        UInt64 boundaryFrames = period * RING_FRAMES;

        /* The last completion reported deviceFrames at deviceHost; the boundary
         * was crossed that many frames earlier. */
        UInt64 back = (UInt64)((Float64)(deviceFrames - boundaryFrames) * ticksPerFrame);
        UInt64 boundaryHost = deviceHost > back ? deviceHost - back : deviceHost;

        /*
         * Only when the period actually advances -- strictly greater, not
         * greater-or-equal.
         *
         * Core Audio calls this many times within one period, and the pair it
         * gets back must be stable across those calls: it derives the device's
         * rate from consecutive anchors, so re-deriving hostTime each time for
         * an unchanged sampleTime feeds it the scheduling jitter of whichever
         * completion happened to be most recent. That was audible as an
         * occasional glitch every few minutes, when a wobble grew large enough
         * to survive Core Audio's own smoothing.
         */
        if (period > dev->periodCount) {
            /* How far the new anchor sits from where the previous one predicted
             * it would. This is the anchor jitter, and it is the thing that
             * shows up as a glitch, so it is worth being able to see. */
            if (dev->anchorHostTime != 0) {
                UInt64 ticksPerPeriod = (UInt64)(ticksPerFrame * (Float64)RING_FRAMES);
                UInt64 predicted = dev->anchorHostTime
                                 + ticksPerPeriod * (period - dev->periodCount);
                UInt64 delta = boundaryHost > predicted
                             ? boundaryHost - predicted : predicted - boundaryHost;
                Float64 ns = (Float64)delta * 1.0e9 / host_ticks_per_second();
                dev->anchorJitterNs = (UInt64)ns;
                if (dev->anchorJitterNs > dev->anchorJitterMaxNs) {
                    dev->anchorJitterMaxNs = dev->anchorJitterNs;
                }
                dev->anchorUpdates++;
            }

            /* Host time only ever ratchets forward: sample time has just
             * advanced, so an anchor that moved backwards would report a
             * negative rate, which Core Audio treats as a fault. Nothing
             * legitimate produces one, so anything earlier is noise. */
            dev->periodCount = period;
            if (boundaryHost > dev->anchorHostTime) dev->anchorHostTime = boundaryHost;
        }

        *sampleTime = (Float64)(dev->periodCount * RING_FRAMES);
        *hostTime   = dev->anchorHostTime;
        *seed       = dev->timelineSeed;
        pthread_mutex_unlock(&gStateMutex);
        return kAudioHardwareNoError;
    }

    /* The diagnostic "host" clock source, or -- defensively -- an engine that
     * died mid-stream. Extrapolates from the anchor StartIO copied out of the
     * engine, so even this path never contradicts the device timeline at the
     * moment it takes over; from there it drifts at the crystals' difference,
     * which is the documented cost of choosing it. */
    pthread_mutex_lock(&gStateMutex);
    Float64 ticksPerFrame = host_ticks_per_second() / dev->sampleRate;
    UInt64  ticksPerPeriod = (UInt64)(ticksPerFrame * (Float64)RING_FRAMES);
    UInt64  now = mach_absolute_time();

    if (dev->anchorHostTime == 0) {
        dev->anchorHostTime = now;   /* first call, or just switched source */
    } else if (ticksPerPeriod > 0) {
        while (now >= dev->anchorHostTime + ticksPerPeriod) {
            dev->anchorHostTime += ticksPerPeriod;
            dev->periodCount++;
        }
    }

    *sampleTime = (Float64)(dev->periodCount * RING_FRAMES);
    *hostTime   = dev->anchorHostTime;
    *seed       = dev->timelineSeed;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
                                  UInt32 client, UInt32 operation, Boolean* willDo,
                                  Boolean* willDoInPlace)
{
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    (void)d; (void)client;
    if ((!dev || role_ != kRole_Device)) return kAudioHardwareBadObjectError;
    if (!willDo || !willDoInPlace) return kAudioHardwareIllegalOperationError;

    switch (operation) {
        case kAudioServerPlugInIOOperationReadInput:
        case kAudioServerPlugInIOOperationWriteMix:
            *willDo = true; *willDoInPlace = true; break;
        default:
            *willDo = false; *willDoInPlace = true; break;
    }
    return kAudioHardwareNoError;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
                                 UInt32 client, UInt32 operation, UInt32 frameCount,
                                 const AudioServerPlugInIOCycleInfo* cycle)
{
    (void)d; (void)client; (void)operation; (void)frameCount; (void)cycle;
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    return (dev && role_ == kRole_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

/*
 * The audio path.
 *
 * The rings are addressed by the cycle's sample time -- the same timeline
 * GetZeroTimeStamp publishes -- not by arrival order. Where a frame lands is
 * decided by when Core Audio says it plays or was captured, so the phase
 * between these calls and the USB engine's cursors is pinned by construction:
 * a late cycle is one glitch, never a shifted stream. The sample times Core
 * Audio hands an IO cycle are non-negative once IO is running; anything else
 * is answered with silence rather than arithmetic on a wrapped index.
 *
 * Real-time rules apply here: no allocation, no locks, no logging
 * (guidelines section 16).
 */
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
                              AudioObjectID stream, UInt32 client, UInt32 operation,
                              UInt32 frameCount, const AudioServerPlugInIOCycleInfo* cycle,
                              void* mainBuffer, void* secondaryBuffer)
{
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    (void)d; (void)stream; (void)client; (void)secondaryBuffer;
    if ((!dev || role_ != kRole_Device)) return kAudioHardwareBadObjectError;

    atomic_fetch_add_explicit(&dev->iOCycles, 1, memory_order_relaxed);

    if (operation == kAudioServerPlugInIOOperationReadInput && mainBuffer) {
        Float64 pos = cycle ? cycle->mInputTime.mSampleTime : -1.0;
        if (pos >= 0.0) {
            emu_engine_read_input(dev->engine, (float*)mainBuffer, frameCount, (uint64_t)(pos + 0.5));
        } else {
            memset(mainBuffer, 0, (size_t)frameCount * CHANNELS * sizeof(float));
        }
    } else if (operation == kAudioServerPlugInIOOperationWriteMix && mainBuffer) {
        Float64 pos = cycle ? cycle->mOutputTime.mSampleTime : -1.0;
        if (pos >= 0.0) {
            atomic_fetch_add_explicit(&dev->framesOut, frameCount, memory_order_relaxed);
            emu_engine_write_output(dev->engine, (const float*)mainBuffer, frameCount,
                                    (uint64_t)(pos + 0.5));
        }
    }
    return kAudioHardwareNoError;
}

static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
                               UInt32 client, UInt32 operation, UInt32 frameCount,
                               const AudioServerPlugInIOCycleInfo* cycle)
{
    (void)d; (void)client; (void)operation; (void)frameCount; (void)cycle;
    DeviceRole role_ = kRole_Device;
    Device* dev = device_for_object(id, &role_);
    return (dev && role_ == kRole_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

/* --- interface and factory ------------------------------------------------ */

static AudioServerPlugInDriverInterface gEMUInterface = {
    .QueryInterface                   = QueryInterface,
    .AddRef                           = AddRef,
    .Release                          = ReleaseRef,
    .Initialize                       = Initialize,
    .CreateDevice                     = CreateDevice,
    .DestroyDevice                    = DestroyDevice,
    .AddDeviceClient                  = AddDeviceClient,
    .RemoveDeviceClient               = RemoveDeviceClient,
    .PerformDeviceConfigurationChange = PerformDeviceConfigurationChange,
    .AbortDeviceConfigurationChange   = AbortDeviceConfigurationChange,
    .HasProperty                      = HasProperty,
    .IsPropertySettable               = IsPropertySettable,
    .GetPropertyDataSize              = GetPropertyDataSize,
    .GetPropertyData                  = GetPropertyData,
    .SetPropertyData                  = SetPropertyData,
    .StartIO                          = StartIO,
    .StopIO                           = StopIO,
    .GetZeroTimeStamp                 = GetZeroTimeStamp,
    .WillDoIOOperation                = WillDoIOOperation,
    .BeginIOOperation                 = BeginIOOperation,
    .DoIOOperation                    = DoIOOperation,
    .EndIOOperation                   = EndIOOperation,
};

static AudioServerPlugInDriverInterface* gEMUInterfacePtr = &gEMUInterface;
AudioServerPlugInDriverRef gEMUDriverRef = &gEMUInterfacePtr;

void* EMUTrackerPreFactory(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);

void* EMUTrackerPreFactory(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID)
{
    (void)allocator;
    if (!requestedTypeUUID) return NULL;
    if (!CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID)) return NULL;
    return gEMUDriverRef;
}
