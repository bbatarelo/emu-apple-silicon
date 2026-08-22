/*
 * E-MU Tracker Pre -- Core Audio HAL plug-in.
 *
 * Publishes the device to Core Audio so every application can use it. Runs in
 * the audio driver-host helper, signed with an ordinary Apple Development
 * certificate, and needs no DriverKit entitlement. See docs/path-without-apple.md
 * for the measurements that established this route.
 *
 * This stage builds the Core Audio surface only: a device with an input and an
 * output stream, the property surface, and a timeline. IO is silence. The USB
 * engine goes underneath it next, and the seam is deliberately narrow -- only
 * StartIO, StopIO, GetZeroTimeStamp and DoIOOperation need to change.
 *
 * Timing here is driven by the host clock. That is a placeholder: the device's
 * own clock is the real reference, which is what ClockEstimator exists for.
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
#include "../shared/device.h"

#define LOG_SUBSYSTEM "net.quantum-bit.EMUTrackerPre"

static os_log_t emu_log(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create(LOG_SUBSYSTEM, "plugin"); });
    return log;
}

#define EMU_LOG(fmt, ...) os_log(emu_log(), "TrackerPre: " fmt, ##__VA_ARGS__)

/* --- object model --------------------------------------------------------
 *
 * Fixed IDs. Core Audio addresses every property by object, and a plug-in that
 * publishes a static topology has no reason to allocate them dynamically.
 */
enum {
    kObjectID_PlugIn        = kAudioObjectPlugInObject,  /* 1 */
    kObjectID_Device        = 2,
    kObjectID_Stream_Input  = 3,
    kObjectID_Stream_Output = 4,
    /* The Tracker Pre has no hardware master output level -- only a headphone
     * knob -- so these are software controls over the output stream. Without
     * them macOS greys out its volume slider entirely. */
    kObjectID_Volume_Output = 5,
    kObjectID_Mute_Output   = 6,
};

/* Range of the software fader. -96 dB is far enough below anything audible to
 * serve as the bottom of the slider. */
#define VOLUME_MIN_DB  (-96.0f)
#define VOLUME_MAX_DB  (0.0f)

#define DEVICE_UID          "net.quantum-bit.EMUTrackerPre"
#define DEVICE_NAME         "E-MU Tracker Pre"
#define DEVICE_MANUFACTURER "E-MU Systems (revival)"

#define CHANNELS            2
/* Core Audio speaks Float32 to clients. The device's 24-bit packed format is a
 * conversion in the USB layer, not something applications should see. */
#define BYTES_PER_CHANNEL   4
#define BYTES_PER_FRAME     (CHANNELS * BYTES_PER_CHANNEL)

/* The period at which the timeline is anchored, in frames. Core Audio uses it
 * to reason about wrap-around, so it must match how the ring actually behaves. */
#define RING_FRAMES         8192

/* Eight isochronous requests of 8 ms each, in frames at the current rate. */
#define ENGINE_LATENCY_MS   64
#define ENGINE_LATENCY_FRAMES ((UInt32)(gSampleRate * ENGINE_LATENCY_MS / 1000.0))

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
};

/* Every rate the hardware supports, from the descriptors. */
static const Float64 kSupportedRates[] = {
    44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0
};
#define NUM_RATES (sizeof(kSupportedRates) / sizeof(kSupportedRates[0]))

/* --- state ---------------------------------------------------------------- */

static pthread_mutex_t   gStateMutex     = PTHREAD_MUTEX_INITIALIZER;
static UInt32            gRefCount       = 0;
static AudioServerPlugInHostRef gHost    = NULL;

static Float64           gSampleRate     = 48000.0;
static UInt32            gIOClients      = 0;
static Boolean           gInputActive    = true;
static Boolean           gOutputActive   = true;
static Float32           gVolumeScalar   = 1.0f;
static Boolean           gMuted          = false;

/* Atomics, because DoIOOperation runs on the real-time thread and must not take
 * the state lock. */
static _Atomic uint64_t  gStartIOCount   = 0;
static _Atomic uint64_t  gIOCycles       = 0;
static _Atomic uint64_t  gFramesOut      = 0;

/* Timeline anchor. Advanced one ring period at a time so the sample clock and
 * the host clock stay tied together. */
static UInt64            gAnchorHostTime = 0;
static UInt64            gTimelineSeed   = 1;
static UInt64            gPeriodCount    = 0;

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
static void push_gain_to_engine(void)
{
    emu_engine_set_output_gain(gMuted ? 0.0f : volume_scalar_to_gain(gVolumeScalar));
}

static void fill_stream_format(AudioStreamBasicDescription* format)
{
    memset(format, 0, sizeof(*format));
    format->mSampleRate       = gSampleRate;
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

static OSStatus Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host)
{
    (void)driver;
    gHost = host;
    EMU_LOG("initialized, publishing %{public}s at %{public}.0f Hz",
            DEVICE_NAME, gSampleRate);
    return kAudioHardwareNoError;
}

/* The device is static, so Core Audio never creates or destroys one. */
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                             const AudioServerPlugInClientInfo* c, AudioObjectID* out)
{ (void)d; (void)desc; (void)c; (void)out; return kAudioHardwareUnsupportedOperationError; }

static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{ (void)d; (void)id; return kAudioHardwareUnsupportedOperationError; }

static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                const AudioServerPlugInClientInfo* c)
{ (void)d; (void)c; return id == kObjectID_Device ? kAudioHardwareNoError
                                                  : kAudioHardwareBadObjectError; }

static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                   const AudioServerPlugInClientInfo* c)
{ (void)d; (void)c; return id == kObjectID_Device ? kAudioHardwareNoError
                                                  : kAudioHardwareBadObjectError; }

/* Rate changes arrive as a configuration change so Core Audio can quiesce IO
 * around them. The hardware SET_CUR and its read-back verification belong here
 * once USB is wired in -- guidelines section 18. */
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d,
                                                 AudioObjectID id, UInt64 action, void* info)
{
    (void)d; (void)info;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;

    Float64 rate = (Float64)action;
    Boolean known = false;
    for (size_t i = 0; i < NUM_RATES; i++) {
        if (kSupportedRates[i] == rate) { known = true; break; }
    }
    if (!known) return kAudioHardwareUnsupportedOperationError;

    pthread_mutex_lock(&gStateMutex);
    gSampleRate = rate;
    gAnchorHostTime = 0;   /* the timeline restarts with the rate */
    gPeriodCount = 0;
    gTimelineSeed++;
    pthread_mutex_unlock(&gStateMutex);

    EMU_LOG("sample rate changed to %{public}.0f Hz", rate);
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

static Boolean stream_matches_scope(AudioObjectID stream, AudioObjectPropertyScope scope)
{
    if (scope == kAudioObjectPropertyScopeGlobal) return true;
    if (scope == kAudioObjectPropertyScopeInput)  return stream == kObjectID_Stream_Input;
    if (scope == kAudioObjectPropertyScopeOutput) return stream == kObjectID_Stream_Output;
    return false;
}

/* Number of stream objects visible in a given scope. */
static UInt32 stream_count(AudioObjectPropertyScope scope)
{
    UInt32 n = 0;
    if (stream_matches_scope(kObjectID_Stream_Input, scope))  n++;
    if (stream_matches_scope(kObjectID_Stream_Output, scope)) n++;
    return n;
}

/* --- HasProperty ---------------------------------------------------------- */

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID object,
                           pid_t client, const AudioObjectPropertyAddress* address)
{
    (void)d; (void)client;
    if (!address) return false;

    switch (object) {
        case kObjectID_PlugIn:
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

        case kObjectID_Device:
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
                    return true;
                default: return false;
            }

        case kObjectID_Volume_Output:
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

        case kObjectID_Mute_Output:
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

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
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
    (void)d; (void)client;
    if (!address || !settable) return kAudioHardwareIllegalOperationError;

    *settable = false;
    if (object == kObjectID_Device &&
        address->mSelector == kAudioDevicePropertyNominalSampleRate) {
        *settable = true;
    }
    if ((object == kObjectID_Stream_Input || object == kObjectID_Stream_Output) &&
        address->mSelector == kAudioStreamPropertyIsActive) {
        *settable = true;
    }
    if (object == kObjectID_Volume_Output &&
        (address->mSelector == kAudioLevelControlPropertyScalarValue ||
         address->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        *settable = true;
    }
    if (object == kObjectID_Mute_Output &&
        address->mSelector == kAudioBooleanControlPropertyValue) {
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
            if (object == kObjectID_PlugIn) { *outSize = sizeof(AudioObjectID); }
            else if (object == kObjectID_Device) {
                UInt32 n = stream_count(address->mScope);
                if (stream_matches_scope(kObjectID_Stream_Output, address->mScope)) n += 2;
                *outSize = n * sizeof(AudioObjectID);
            } else { *outSize = 0; }
            return kAudioHardwareNoError;

        case kAudioPlugInPropertyDeviceList:
            *outSize = sizeof(AudioObjectID); return kAudioHardwareNoError;

        case kAudioDevicePropertyStreams:
            *outSize = stream_count(address->mScope) * sizeof(AudioObjectID);
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
            *outSize = sizeof(AudioServerPlugInCustomPropertyInfo);
            return kAudioHardwareNoError;

        case kEMUProperty_Diagnostics:
            *outSize = sizeof(CFPropertyListRef); return kAudioHardwareNoError;

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
    (void)d; (void)client;
    if (!address || !outSize || !outData) return kAudioHardwareIllegalOperationError;

    switch (object) {

    /* ---------------- plug-in ---------------- */
    case kObjectID_PlugIn:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioObjectClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioPlugInClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(kAudioObjectUnknown);
            case kAudioObjectPropertyManufacturer: RETURN_CFSTR(DEVICE_MANUFACTURER);
            case kAudioPlugInPropertyResourceBundle: RETURN_CFSTR("");

            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList:
                if (dataSize < sizeof(AudioObjectID)) { *outSize = 0; return kAudioHardwareNoError; }
                *(AudioObjectID*)outData = kObjectID_Device;
                *outSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;

            case kAudioPlugInPropertyTranslateUIDToDevice: {
                if (qualifierSize != sizeof(CFStringRef) || !qualifier) {
                    return kAudioHardwareIllegalOperationError;
                }
                CFStringRef uid = *(const CFStringRef*)qualifier;
                *(AudioObjectID*)outData =
                    (uid && CFStringCompare(uid, CFSTR(DEVICE_UID), 0) == kCFCompareEqualTo)
                        ? kObjectID_Device : kAudioObjectUnknown;
                *outSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }
            default: return kAudioHardwareUnknownPropertyError;
        }

    /* ---------------- device ---------------- */
    case kObjectID_Device:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioObjectClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioDeviceClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(kObjectID_PlugIn);
            case kAudioObjectPropertyName:         RETURN_CFSTR(DEVICE_NAME);
            case kAudioObjectPropertyManufacturer: RETURN_CFSTR(DEVICE_MANUFACTURER);
            case kAudioDevicePropertyDeviceUID:    RETURN_CFSTR(DEVICE_UID);
            case kAudioDevicePropertyModelUID:     RETURN_CFSTR(DEVICE_UID ".model");

            case kAudioDevicePropertyTransportType: RETURN_U32(kAudioDeviceTransportTypeUSB);
            case kAudioDevicePropertyClockDomain:   RETURN_U32(0);
            case kAudioDevicePropertyDeviceIsAlive: RETURN_U32(1);
            case kAudioDevicePropertyDeviceIsRunning: RETURN_U32(gIOClients > 0 ? 1 : 0);
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: RETURN_U32(1);
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RETURN_U32(1);
            case kAudioDevicePropertyIsHidden: RETURN_U32(0);
            case kAudioDevicePropertyZeroTimeStampPeriod: RETURN_U32(RING_FRAMES);

            /* The engine keeps eight requests of 8 ms in flight, so a frame
             * handed over now reaches the device roughly that far ahead. Still
             * an estimate rather than a measurement, but a defensible one, and
             * far better for latency compensation than claiming zero. */
            case kAudioDevicePropertyLatency:       RETURN_U32(ENGINE_LATENCY_FRAMES);
            case kAudioDevicePropertySafetyOffset:  RETURN_U32(RING_FRAMES / 8);

            case kAudioDevicePropertyNominalSampleRate: RETURN_F64(gSampleRate);

            case kAudioObjectPropertyCustomPropertyInfoList: {
                if (dataSize < sizeof(AudioServerPlugInCustomPropertyInfo)) {
                    *outSize = 0; return kAudioHardwareNoError;
                }
                AudioServerPlugInCustomPropertyInfo* info =
                    (AudioServerPlugInCustomPropertyInfo*)outData;
                info[0].mSelector = kEMUProperty_Diagnostics;
                info[0].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                info[0].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
                *outSize = sizeof(AudioServerPlugInCustomPropertyInfo);
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
                emu_engine_stats(&engine);

                struct { CFStringRef key; uint64_t value; } entries[] = {
                    { CFSTR("startIOCalls"),   atomic_load(&gStartIOCount) },
                    { CFSTR("ioCycles"),       atomic_load(&gIOCycles)     },
                    { CFSTR("framesToOutput"), atomic_load(&gFramesOut)    },
                    { CFSTR("ioClients"),      (uint64_t)gIOClients        },
                    { CFSTR("engineRunning"),  emu_engine_running() ? 1u : 0u },
                    { CFSTR("framesPlayed"),   engine.frames_played        },
                    { CFSTR("ringDepth"),      engine.ring_depth           },
                    { CFSTR("ringUnderruns"),  engine.underruns            },
                    { CFSTR("ringOverruns"),   engine.overruns             },
                    { CFSTR("usbErrors"),      engine.usb_errors           },
                    { CFSTR("feedbackStarved"), engine.feedback_starved   },
                    { CFSTR("framesCaptured"), engine.frames_captured      },
                    { CFSTR("inputDepth"),     engine.input_depth          },
                    { CFSTR("inputUnderruns"), engine.input_underruns      },
                    { CFSTR("inputOverruns"),  engine.input_overruns       },
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
                if (n < capacity) ids[n++] = kObjectID_Volume_Output;
                if (n < capacity) ids[n++] = kObjectID_Mute_Output;
                *outSize = n * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }

            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams: {
                AudioObjectID* ids = (AudioObjectID*)outData;
                UInt32 capacity = dataSize / sizeof(AudioObjectID);
                UInt32 n = 0;
                if (stream_matches_scope(kObjectID_Stream_Input, address->mScope) && n < capacity) {
                    ids[n++] = kObjectID_Stream_Input;
                }
                if (stream_matches_scope(kObjectID_Stream_Output, address->mScope) && n < capacity) {
                    ids[n++] = kObjectID_Stream_Output;
                }
                /* Controls are owned objects too, but they are not streams. */
                if (address->mSelector == kAudioObjectPropertyOwnedObjects &&
                    stream_matches_scope(kObjectID_Stream_Output, address->mScope)) {
                    if (n < capacity) ids[n++] = kObjectID_Volume_Output;
                    if (n < capacity) ids[n++] = kObjectID_Mute_Output;
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
    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioObjectClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioStreamClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(kObjectID_Device);
            case kAudioStreamPropertyDirection:
                RETURN_U32(object == kObjectID_Stream_Input ? 1 : 0);
            case kAudioStreamPropertyTerminalType:
                RETURN_U32(object == kObjectID_Stream_Input
                           ? kAudioStreamTerminalTypeMicrophone
                           : kAudioStreamTerminalTypeSpeaker);
            case kAudioStreamPropertyStartingChannel: RETURN_U32(1);
            case kAudioStreamPropertyLatency:         RETURN_U32(0);
            case kAudioStreamPropertyIsActive:
                RETURN_U32(object == kObjectID_Stream_Input ? gInputActive : gOutputActive);

            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                if (dataSize < sizeof(AudioStreamBasicDescription)) {
                    return kAudioHardwareBadPropertySizeError;
                }
                fill_stream_format((AudioStreamBasicDescription*)outData);
                *outSize = sizeof(AudioStreamBasicDescription);
                return kAudioHardwareNoError;
            }

            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription* formats = (AudioStreamRangedDescription*)outData;
                UInt32 capacity = dataSize / sizeof(AudioStreamRangedDescription);
                UInt32 n = 0;
                for (size_t i = 0; i < NUM_RATES && n < capacity; i++, n++) {
                    fill_stream_format(&formats[n].mFormat);
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
    case kObjectID_Volume_Output:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioLevelControlClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioVolumeControlClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(kObjectID_Device);
            case kAudioControlPropertyScope:    RETURN_U32(kAudioObjectPropertyScopeOutput);
            case kAudioControlPropertyElement:  RETURN_U32(kAudioObjectPropertyElementMain);

            case kAudioLevelControlPropertyScalarValue: {
                if (dataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                *(Float32*)outData = gVolumeScalar;
                pthread_mutex_unlock(&gStateMutex);
                *outSize = sizeof(Float32);
                return kAudioHardwareNoError;
            }
            case kAudioLevelControlPropertyDecibelValue: {
                if (dataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                *(Float32*)outData = volume_scalar_to_db(gVolumeScalar);
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

    case kObjectID_Mute_Output:
        switch (address->mSelector) {
            case kAudioObjectPropertyBaseClass: RETURN_U32(kAudioBooleanControlClassID);
            case kAudioObjectPropertyClass:     RETURN_U32(kAudioMuteControlClassID);
            case kAudioObjectPropertyOwner:     RETURN_U32(kObjectID_Device);
            case kAudioControlPropertyScope:    RETURN_U32(kAudioObjectPropertyScopeOutput);
            case kAudioControlPropertyElement:  RETURN_U32(kAudioObjectPropertyElementMain);
            case kAudioBooleanControlPropertyValue: {
                pthread_mutex_lock(&gStateMutex);
                UInt32 muted = gMuted ? 1 : 0;
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
    (void)d; (void)client; (void)qualifierSize; (void)qualifier;
    if (!address || !data) return kAudioHardwareIllegalOperationError;

    if (object == kObjectID_Device &&
        address->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (dataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 requested = *(const Float64*)data;

        for (size_t i = 0; i < NUM_RATES; i++) {
            if (kSupportedRates[i] != requested) continue;
            /* Ask the host to quiesce IO and call back into
             * PerformDeviceConfigurationChange, rather than switching underneath
             * a running stream. */
            if (gHost) {
                gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device,
                                                        (UInt64)requested, NULL);
            }
            return kAudioHardwareNoError;
        }
        return kAudioHardwareIllegalOperationError;
    }

    if ((object == kObjectID_Stream_Input || object == kObjectID_Stream_Output) &&
        address->mSelector == kAudioStreamPropertyIsActive) {
        if (dataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        Boolean active = *(const UInt32*)data != 0;
        pthread_mutex_lock(&gStateMutex);
        if (object == kObjectID_Stream_Input) gInputActive = active;
        else                                  gOutputActive = active;
        pthread_mutex_unlock(&gStateMutex);
        return kAudioHardwareNoError;
    }

    if (object == kObjectID_Volume_Output &&
        (address->mSelector == kAudioLevelControlPropertyScalarValue ||
         address->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        if (dataSize != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;

        Float32 scalar = address->mSelector == kAudioLevelControlPropertyScalarValue
                       ? *(const Float32*)data
                       : volume_db_to_scalar(*(const Float32*)data);
        if (scalar < 0.0f) scalar = 0.0f;
        if (scalar > 1.0f) scalar = 1.0f;

        pthread_mutex_lock(&gStateMutex);
        gVolumeScalar = scalar;
        push_gain_to_engine();
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
            gHost->PropertiesChanged(gHost, kObjectID_Volume_Output, 2, changed);
        }
        return kAudioHardwareNoError;
    }

    if (object == kObjectID_Mute_Output &&
        address->mSelector == kAudioBooleanControlPropertyValue) {
        if (dataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;

        pthread_mutex_lock(&gStateMutex);
        gMuted = *(const UInt32*)data != 0;
        push_gain_to_engine();
        pthread_mutex_unlock(&gStateMutex);

        if (gHost) {
            AudioObjectPropertyAddress changed = {
                kAudioBooleanControlPropertyValue,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
            };
            gHost->PropertiesChanged(gHost, kObjectID_Mute_Output, 1, &changed);
        }
        return kAudioHardwareNoError;
    }

    return kAudioHardwareUnsupportedOperationError;
}

/* --- IO ------------------------------------------------------------------- */

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    (void)d; (void)client;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;

    atomic_fetch_add(&gStartIOCount, 1);

    pthread_mutex_lock(&gStateMutex);
    if (gIOClients == 0) {
        gAnchorHostTime = mach_absolute_time();
        gPeriodCount = 0;
        gTimelineSeed++;
        if (!emu_engine_start((uint32_t)gSampleRate)) {
            pthread_mutex_unlock(&gStateMutex);
            EMU_LOG("IO start failed: USB engine did not come up");
            return kAudioHardwareNotRunningError;
        }
        push_gain_to_engine();
        EMU_LOG("IO started at %{public}.0f Hz", gSampleRate);
    }
    gIOClients++;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    (void)d; (void)client;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gStateMutex);
    bool last = (gIOClients > 0 && --gIOClients == 0);
    pthread_mutex_unlock(&gStateMutex);

    /* Outside the lock: stopping joins the engine thread, and holding the state
     * lock across that would deadlock against anything it needs. */
    if (last) {
        emu_engine_stop();
        EMU_LOG("IO stopped");
    }
    return kAudioHardwareNoError;
}

/*
 * Anchors Core Audio's sample timeline to the device's clock.
 *
 * The device decides how fast audio actually moves; the host clock only
 * approximates it. Anchoring to the host means Core Audio and the hardware
 * accumulate a difference that has to go somewhere, and where it goes is the
 * ring -- slowly filling or emptying until it breaks. Anchoring to frames the
 * device has genuinely consumed removes the disagreement rather than absorbing
 * it.
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
    (void)d; (void)client;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (!sampleTime || !hostTime || !seed) return kAudioHardwareIllegalOperationError;

    uint64_t deviceFrames = 0, deviceHost = 0;
    if (emu_engine_timeline(&deviceFrames, &deviceHost)) {
        pthread_mutex_lock(&gStateMutex);
        Float64 ticksPerFrame = host_ticks_per_second() / gSampleRate;

        UInt64 period = deviceFrames / RING_FRAMES;
        UInt64 boundaryFrames = period * RING_FRAMES;

        /* The last completion reported deviceFrames at deviceHost; the boundary
         * was crossed that many frames earlier. */
        UInt64 back = (UInt64)((Float64)(deviceFrames - boundaryFrames) * ticksPerFrame);
        UInt64 boundaryHost = deviceHost > back ? deviceHost - back : deviceHost;

        /* Never hand back a timeline that goes backwards, whatever the
         * arithmetic says. Core Audio treats that as a fault, and a rounding
         * wobble near a boundary is not worth a glitch. */
        if (boundaryFrames >= gPeriodCount * RING_FRAMES) {
            gPeriodCount = period;
            gAnchorHostTime = boundaryHost;
        }

        *sampleTime = (Float64)(gPeriodCount * RING_FRAMES);
        *hostTime   = gAnchorHostTime;
        *seed       = gTimelineSeed;
        pthread_mutex_unlock(&gStateMutex);
        return kAudioHardwareNoError;
    }

    /* Before the first transfer completes there is no device clock to follow,
     * so fall back to the host's. This lasts a few milliseconds at stream
     * start. */
    pthread_mutex_lock(&gStateMutex);
    Float64 ticksPerFrame = host_ticks_per_second() / gSampleRate;
    UInt64  ticksPerPeriod = (UInt64)(ticksPerFrame * (Float64)RING_FRAMES);
    UInt64  now = mach_absolute_time();

    if (ticksPerPeriod > 0) {
        while (now >= gAnchorHostTime + ticksPerPeriod) {
            gAnchorHostTime += ticksPerPeriod;
            gPeriodCount++;
        }
    }

    *sampleTime = (Float64)(gPeriodCount * RING_FRAMES);
    *hostTime   = gAnchorHostTime;
    *seed       = gTimelineSeed;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
                                  UInt32 client, UInt32 operation, Boolean* willDo,
                                  Boolean* willDoInPlace)
{
    (void)d; (void)client;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;
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
{ (void)d; (void)client; (void)operation; (void)frameCount; (void)cycle;
  return id == kObjectID_Device ? kAudioHardwareNoError : kAudioHardwareBadObjectError; }

/*
 * The audio path.
 *
 * Silence for now, in both directions. ReadInput will come from the USB capture
 * ring and WriteMix will go to the playback ring, converting between Float32
 * and the device's 24-bit packed format.
 *
 * Real-time rules apply here already: no allocation, no locks, no logging
 * (guidelines section 16).
 */
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
                              AudioObjectID stream, UInt32 client, UInt32 operation,
                              UInt32 frameCount, const AudioServerPlugInIOCycleInfo* cycle,
                              void* mainBuffer, void* secondaryBuffer)
{
    (void)d; (void)stream; (void)client; (void)cycle; (void)secondaryBuffer;
    if (id != kObjectID_Device) return kAudioHardwareBadObjectError;

    atomic_fetch_add(&gIOCycles, 1);

    if (operation == kAudioServerPlugInIOOperationReadInput && mainBuffer) {
        emu_engine_read_input((float*)mainBuffer, frameCount);
    } else if (operation == kAudioServerPlugInIOOperationWriteMix && mainBuffer) {
        atomic_fetch_add(&gFramesOut, frameCount);
        emu_engine_write_output((const float*)mainBuffer, frameCount);
    }
    return kAudioHardwareNoError;
}

static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id,
                               UInt32 client, UInt32 operation, UInt32 frameCount,
                               const AudioServerPlugInIOCycleInfo* cycle)
{ (void)d; (void)client; (void)operation; (void)frameCount; (void)cycle;
  return id == kObjectID_Device ? kAudioHardwareNoError : kAudioHardwareBadObjectError; }

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
