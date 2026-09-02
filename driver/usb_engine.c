/*
 * USB engine for the HAL plug-in.
 *
 * The duplex transport proven in Milestone 4, with the tone generator replaced
 * by a ring that Core Audio fills. Everything load-bearing is unchanged and was
 * established the hard way:
 *
 *   - low-latency isochronous transfers, because the classic API delivers one
 *     frame-list entry per USB frame and a bInterval 3 endpoint is serviced
 *     twice per frame, so half the audio never moves
 *   - contiguous buffer layout, not strided by wMaxPacketSize
 *   - the clock rate set and verified by read-back before any alternate setting
 *     is selected, because a mismatch wedges the device until it is replugged
 *   - capture as the clock reference, sizing playback packets through the
 *     feedback queue in the Rust core
 */

#include "usb_engine.h"
#include "ring.h"
#include "../shared/usb_util.h"
#include "../shared/device.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include "../rust/emu-ca0189/include/emu_ca0189.h"

#define NUM_REQUESTS  8
#define REQUEST_MS    8
#define MAX_ENTRIES   (REQUEST_MS * 8)
#define SCHEDULE_MARGIN 16

/* --- which device the plug-in is speaking for -----------------------------
 *
 * Core Audio asks whether the plug-in has a device, and what it is called, on
 * its property thread, at any moment, and long before anything opens the
 * hardware. Neither obvious answer works: enumerating IOKit per call puts a
 * synchronous registry round trip on that thread, and resolving once and
 * caching forever keeps publishing a device that has since been unplugged --
 * or the name of one since replaced by a sibling.
 *
 * So presence is tracked instead of polled. IOKit reports arrivals and
 * departures on a private serial queue, that queue keeps the count of what is
 * attached, and the property thread only reads a pointer: NULL for nothing
 * attached, which is when the plug-in publishes no device at all. The rule
 * for choosing among several: whatever the engine is running on, for as long
 * as that stays attached; otherwise the preferred product if it is here;
 * otherwise the first that is. StartIO opens whichever this names, so the
 * name Core Audio shows and the hardware behind it agree by construction
 * rather than by two lookups applying the same rule.
 *
 * Nothing tears the watch down: it is armed once and lives as long as the
 * plug-in's host process does.
 */
static _Atomic(const EmuDeviceIdentity*) gIdentity;
static _Atomic(void (*)(void))           gIdentityObserver;

/* The product the engine has open, or NULL. While set, the identity stays on
 * it: a preferred sibling arriving mid-stream must not rename the device that
 * is playing, and the name must describe the hardware the audio is on. Stored
 * by the engine thread, applied by the hot-plug queue -- every store is
 * followed by a refresh there, in order with the notifications. */
static _Atomic(const EmuDeviceIdentity*) gRunningIdentity;

/* All of this belongs to gNotifyQueue, which is serial, so the counts need no
 * lock of their own. The iterators are held for the life of the process on
 * purpose: releasing one disarms its notification. */
static dispatch_queue_t      gNotifyQueue;
static IONotificationPortRef gNotifyPort;
static io_iterator_t         gMatchIter[EMU_DEVICE_COUNT];
static io_iterator_t         gTermIter[EMU_DEVICE_COUNT];
static uint32_t              gAttached[EMU_DEVICE_COUNT];

static uint32_t drain(io_iterator_t iter)
{
    uint32_t n = 0;
    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        IOObjectRelease(service);
        n++;
    }
    return n;
}

/* `notify` is false for the initial resolve, which happens while the plug-in
 * is still initializing: the host is about to ask for the name anyway, and
 * telling it a device it has not enumerated yet has changed is at best noise. */
static void refresh_identity(bool notify)
{
    const EmuDeviceIdentity* found =
        atomic_load_explicit(&gRunningIdentity, memory_order_relaxed);
    if (found && !gAttached[found - kEmuDevices]) found = NULL;
    for (unsigned i = 0; i < EMU_DEVICE_COUNT && !found; i++) {
        if (gAttached[i] && kEmuDevices[i].product_id == EMU_DEFAULT_PRODUCT_ID) {
            found = &kEmuDevices[i];
        }
    }
    for (unsigned i = 0; i < EMU_DEVICE_COUNT && !found; i++) {
        if (gAttached[i]) found = &kEmuDevices[i];
    }

    const EmuDeviceIdentity* was =
        atomic_exchange_explicit(&gIdentity, found, memory_order_relaxed);
    void (*observer)(void) =
        atomic_load_explicit(&gIdentityObserver, memory_order_relaxed);
    if (was != found && notify && observer) observer();
}

/* Engine thread only. The store is what refresh_identity reads; the refresh
 * queued behind it is what makes the store take effect, ordered with every
 * arrival and departure. The queue exists whenever gIdentity has ever been
 * non-NULL, which is what the engine opens on. */
static void set_running_identity(const EmuDeviceIdentity* id)
{
    atomic_store_explicit(&gRunningIdentity, id, memory_order_relaxed);
    dispatch_async(gNotifyQueue, ^{ refresh_identity(true); });
}

/* Counted from the notifications themselves rather than re-enumerating: a
 * device is still in the registry for a moment as it terminates, so a fresh
 * lookup here can hand back the very device that just left. */
static void device_arrived(void* refcon, io_iterator_t iter)
{
    unsigned i = (unsigned)(uintptr_t)refcon;
    gAttached[i] += drain(iter);
    refresh_identity(true);
}

static void device_departed(void* refcon, io_iterator_t iter)
{
    unsigned i = (unsigned)(uintptr_t)refcon;
    uint32_t gone = drain(iter);
    gAttached[i] = gAttached[i] > gone ? gAttached[i] - gone : 0;
    refresh_identity(true);
}

/* Releasing an iterator disarms its notification. This is also the failure
 * path for arming: a watch armed for some products and not others, or for
 * arrival but not departure, would count wrong forever, and quietly. */
static void disarm_identity_notifications(void)
{
    for (unsigned i = 0; i < EMU_DEVICE_COUNT; i++) {
        if (gMatchIter[i]) IOObjectRelease(gMatchIter[i]);
        if (gTermIter[i]) IOObjectRelease(gTermIter[i]);
        gMatchIter[i] = gTermIter[i] = IO_OBJECT_NULL;
        gAttached[i] = 0;
    }
}

/* All or nothing: false means nothing is armed and nothing is counted. */
static bool install_identity_notifications(void)
{
    for (unsigned i = 0; i < EMU_DEVICE_COUNT; i++) {
        for (int kind = 0; kind < 2; kind++) {
            /* IOKit matches on idVendor and idProduct as a pair -- a vendor
             * alone matches nothing -- so it is one notification per product. */
            CFMutableDictionaryRef matching = IOServiceMatching(kIOUSBDeviceClassName);
            if (!matching) {
                disarm_identity_notifications();
                return false;
            }
            SInt32 vid = EMU_VENDOR_ID, pid = kEmuDevices[i].product_id;
            CFNumberRef vref = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
            CFNumberRef pref = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
            CFDictionarySetValue(matching, CFSTR(kUSBVendorID), vref);
            CFDictionarySetValue(matching, CFSTR(kUSBProductID), pref);
            CFRelease(vref);
            CFRelease(pref);

            io_iterator_t* iter = kind == 0 ? &gMatchIter[i] : &gTermIter[i];
            /* Consumes `matching`, on failure too. */
            if (IOServiceAddMatchingNotification(
                    gNotifyPort,
                    kind == 0 ? kIOFirstMatchNotification : kIOTerminatedNotification,
                    matching,
                    kind == 0 ? device_arrived : device_departed,
                    (void*)(uintptr_t)i, iter) != KERN_SUCCESS) {
                disarm_identity_notifications();
                return false;
            }
            /* Arming requires draining what already matches, which is also how
             * the first-match side learns what is attached right now. */
            uint32_t present = drain(*iter);
            if (kind == 0) gAttached[i] = present;
        }
    }
    return true;
}

/* Idempotent, and cheap after the first call: one pass over the registry to
 * arm the notifications, then nothing. Returns whether the watch is armed.
 *
 * Either it is, completely, or no device is ever published. There is
 * deliberately no look-the-device-up-once fallback: a device that appears but
 * cannot be followed is the stale-device bug the watch exists to fix, back
 * under conditions nobody could reproduce. Nor are those conditions worth
 * much: creating the port only fails when the process is out of Mach ports
 * or memory, and a lookup would fail the same way. The caller's job is to say
 * so where someone will read it. */
static bool watch_identity(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        /* dispatch_queue_create retries allocation rather than returning NULL. */
        gNotifyQueue = dispatch_queue_create("net.quantum-bit.EMUTrackerPre.hotplug",
                                             DISPATCH_QUEUE_SERIAL);
        gNotifyPort = IONotificationPortCreate(kIOMainPortDefault);
        if (!gNotifyPort) return;

        IONotificationPortSetDispatchQueue(gNotifyPort, gNotifyQueue);
        /* On the queue, so the initial count cannot race a notification that
         * arrives while it is being taken. */
        __block bool armed = false;
        dispatch_sync(gNotifyQueue, ^{
            armed = install_identity_notifications();
            if (armed) refresh_identity(false);
        });
        if (!armed) {
            /* Nothing is armed at this point, so nothing can fire; the port
             * and queue are just unused. Leave the failed state looking like
             * the never-started one. */
            IONotificationPortDestroy(gNotifyPort);
            gNotifyPort = NULL;
            dispatch_release(gNotifyQueue);
            gNotifyQueue = NULL;
        }
    });
    /* Written once, inside the dispatch_once, so any thread may read it. */
    return gNotifyPort != NULL;
}

bool emu_engine_set_identity_observer(void (*observer)(void))
{
    atomic_store_explicit(&gIdentityObserver, observer, memory_order_relaxed);
    return watch_identity();
}

bool emu_engine_device_attached(void)
{
    watch_identity();
    return atomic_load_explicit(&gIdentity, memory_order_relaxed) != NULL;
}

const char* emu_engine_device_name(void)
{
    watch_identity();
    const EmuDeviceIdentity* id = atomic_load_explicit(&gIdentity, memory_order_relaxed);
    /* Nothing attached means nothing published, so nothing should be asking;
     * the placeholder is for logs. */
    return id ? id->name : "E-MU USB Audio (not attached)";
}

typedef struct Engine Engine;

typedef struct {
    Engine*                    engine;
    void*                      buffer;
    IOUSBLowLatencyIsocFrame*  frames;
    uint64_t                   frame_start;
} Request;

struct Engine {
    const EmuDeviceIdentity*     identity;   /* what this opened; pinned while set */
    IOUSBDeviceInterface500**    device;
    io_service_t                 service;
    IOUSBInterfaceInterface500** in_intf;
    IOUSBInterfaceInterface500** out_intf;

    uint8_t  in_pipe, out_pipe;
    uint16_t in_max, out_max;
    uint32_t bytes_per_frame;
    uint32_t entries_per_ms;
    uint32_t entries_per_request;
    uint32_t nominal_frames;

    uint64_t next_in_frame, next_out_frame;
    Request  in_requests[NUM_REQUESTS];
    Request  out_requests[NUM_REQUESTS];

    _Alignas(16) uint8_t feedback_storage[2048];
    EmuFeedback* feedback;

    /* Frames the device has actually consumed. Core Audio's timeline is
     * anchored to this rather than to the host clock, so the two cannot drift. */
    _Atomic uint64_t frames_played;
    _Atomic uint64_t frames_captured;
    _Atomic uint64_t usb_errors;

    volatile bool stopping;
    CFRunLoopRef  run_loop;
};

static Engine       gEngine;
static EmuRing      gOutputRing;
static EmuRing      gInputRing;
static pthread_t    gThread;
static volatile bool gRunning = false;
static uint32_t     gSampleRate = 48000;

/* Written by whichever thread handles a volume change, read on the USB
 * completion path. Plain atomic load/store; no ordering is needed beyond not
 * tearing, since a gain that lands one packet late is inaudible. */
static _Atomic float gOutputGain = 1.0f;

/*
 * The device's own clock, published as a matched pair: frames it has consumed,
 * and the host time at which that count was true. Core Audio anchors its
 * timeline to this instead of to mach_absolute_time, so the two clocks cannot
 * drift apart.
 *
 * A seqlock rather than two atomics, because the pair must be consistent -- a
 * frame count from one completion with a timestamp from the next would be worse
 * than useless. The writer is the single USB completion thread; readers retry
 * while a write is in progress, which on this path is a handful of nanoseconds.
 */
static _Atomic uint32_t gTimelineSeq = 0;
static uint64_t         gTimelineFrames = 0;
static uint64_t         gTimelineHost = 0;

static void timeline_publish(uint64_t frames, uint64_t host_time)
{
    uint32_t seq = atomic_load_explicit(&gTimelineSeq, memory_order_relaxed);
    atomic_store_explicit(&gTimelineSeq, seq + 1, memory_order_release);  /* odd */
    gTimelineFrames = frames;
    gTimelineHost = host_time;
    atomic_store_explicit(&gTimelineSeq, seq + 2, memory_order_release);  /* even */
}

bool emu_engine_timeline(uint64_t* frames, uint64_t* host_time)
{
    if (!gRunning) return false;

    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t before = atomic_load_explicit(&gTimelineSeq, memory_order_acquire);
        if (before & 1u) continue;                 /* write in progress */
        uint64_t f = gTimelineFrames;
        uint64_t h = gTimelineHost;
        uint32_t after = atomic_load_explicit(&gTimelineSeq, memory_order_acquire);
        if (before == after) {
            if (h == 0) return false;              /* nothing published yet */
            *frames = f;
            *host_time = h;
            return true;
        }
    }
    return false;
}

/* --- public surface -------------------------------------------------------- */

void emu_engine_write_output(const float* frames, uint32_t count)
{
    if (gRunning) emu_ring_write(&gOutputRing, frames, count);
}

void emu_engine_set_output_gain(float gain)
{
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    atomic_store_explicit(&gOutputGain, gain, memory_order_relaxed);
}

void emu_engine_read_input(float* frames, uint32_t count)
{
    if (gRunning) {
        emu_ring_read_f32(&gInputRing, frames, count);
    } else {
        memset(frames, 0, (size_t)count * EMU_RING_CHANNELS * sizeof(float));
    }
}

uint64_t emu_engine_frames_played(void)
{
    return atomic_load_explicit(&gEngine.frames_played, memory_order_relaxed);
}

bool emu_engine_running(void) { return gRunning; }

/*
 * Zeroes the counters that only exist to be read.
 *
 * Deliberately leaves frames_played and frames_captured alone: the timeline
 * derives its period from frames_played, so resetting it would send Core Audio's
 * sample time backwards, which it treats as a fault.
 */
void emu_engine_reset_counters(void)
{
    atomic_store_explicit(&gOutputRing.underruns, 0, memory_order_relaxed);
    atomic_store_explicit(&gOutputRing.overruns, 0, memory_order_relaxed);
    atomic_store_explicit(&gInputRing.underruns, 0, memory_order_relaxed);
    atomic_store_explicit(&gInputRing.overruns, 0, memory_order_relaxed);
    atomic_store_explicit(&gEngine.usb_errors, 0, memory_order_relaxed);
}

void emu_engine_stats(EmuEngineStats* stats)
{
    if (!stats) return;
    stats->frames_played = emu_engine_frames_played();
    stats->underruns  = atomic_load_explicit(&gOutputRing.underruns, memory_order_relaxed);
    stats->overruns   = atomic_load_explicit(&gOutputRing.overruns, memory_order_relaxed);
    stats->usb_errors = atomic_load_explicit(&gEngine.usb_errors, memory_order_relaxed);
    stats->ring_depth = emu_ring_filled(&gOutputRing);
    stats->feedback_starved = gEngine.feedback ? emu_feedback_starved(gEngine.feedback) : 0;

    stats->frames_captured = atomic_load_explicit(&gEngine.frames_captured, memory_order_relaxed);
    stats->input_depth     = emu_ring_filled(&gInputRing);
    stats->input_underruns = atomic_load_explicit(&gInputRing.underruns, memory_order_relaxed);
    stats->input_overruns  = atomic_load_explicit(&gInputRing.overruns, memory_order_relaxed);
}

/* --- transfers ------------------------------------------------------------- */

static void capture_complete(void* refcon, IOReturn result, void* arg0);
static void playback_complete(void* refcon, IOReturn result, void* arg0);

static IOReturn submit_capture(Request* req)
{
    Engine* e = req->engine;
    for (uint32_t i = 0; i < e->entries_per_request; i++) {
        req->frames[i].frStatus   = kUSBLowLatencyIsochTransferKey;
        req->frames[i].frReqCount = e->in_max;
        req->frames[i].frActCount = 0;
    }
    req->frame_start = e->next_in_frame;
    e->next_in_frame += REQUEST_MS;

    return (*e->in_intf)->LowLatencyReadIsochPipeAsync(
        e->in_intf, e->in_pipe, req->buffer, req->frame_start,
        e->entries_per_request, 1, req->frames, capture_complete, req);
}

static IOReturn submit_playback(Request* req)
{
    Engine* e = req->engine;
    size_t offset = 0;

    for (uint32_t i = 0; i < e->entries_per_request; i++) {
        /* Capture's measurement of the device clock sizes this packet. */
        uint32_t frames = emu_feedback_next(e->feedback, e->nominal_frames);
        uint32_t bytes = emu_output_packet_bytes(frames, e->bytes_per_frame);
        while (bytes > e->out_max && frames > 0) {
            frames--;
            bytes = emu_output_packet_bytes(frames, e->bytes_per_frame);
        }

        /* Contiguous: entry i begins where entry i-1's frReqCount ended. */
        emu_ring_read_s24(&gOutputRing, (uint8_t*)req->buffer + offset, frames,
                          atomic_load_explicit(&gOutputGain, memory_order_relaxed));
        offset += bytes;

        req->frames[i].frStatus   = kUSBLowLatencyIsochTransferKey;
        req->frames[i].frReqCount = (UInt16)bytes;
        req->frames[i].frActCount = 0;
    }

    req->frame_start = e->next_out_frame;
    e->next_out_frame += REQUEST_MS;

    return (*e->out_intf)->LowLatencyWriteIsochPipeAsync(
        e->out_intf, e->out_pipe, req->buffer, req->frame_start,
        e->entries_per_request, 1, req->frames, playback_complete, req);
}

static void resync(Engine* e)
{
    UInt64 now = 0;
    AbsoluteTime at;
    if ((*e->in_intf)->GetBusFrameNumber(e->in_intf, &now, &at) == kIOReturnSuccess) {
        e->next_in_frame = now + SCHEDULE_MARGIN;
        e->next_out_frame = now + SCHEDULE_MARGIN;
    }
}

static void capture_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    Request* req = (Request*)refcon;
    Engine* e = req->engine;

    if (result == kIOReturnIsoTooOld) resync(e);

    /* The buffer is laid out by frReqCount, not by what arrived. Reads set
     * frReqCount to wMaxPacketSize for every entry, so the stride is fixed and
     * short packets leave a gap -- advancing by frActCount instead shifts every
     * frame boundary after the first, which is audible as noise rather than as
     * an error. E-MU's driver says the same thing:
     *
     *     source += mInput.maxFrameSize; // each frame's frReqCount is set to maxFrameSize
     *
     * Writes are contiguous only because there frReqCount *is* the real size. */
    size_t offset = 0;

    for (uint32_t i = 0; i < e->entries_per_request; i++) {
        const IOUSBLowLatencyIsocFrame* f = &req->frames[i];
        if (!emu_frame_ok(f->frStatus)) {
            if (!e->stopping) atomic_fetch_add(&e->usb_errors, 1);
            continue;
        }
        if (f->frActCount == 0) continue;

        uint32_t frames = emu_frames_in_packet(f->frActCount, e->bytes_per_frame);

        /* Capture is the clock reference, so this happens whether or not anyone
         * is recording. */
        emu_feedback_push(e->feedback, frames);

        /* At bInterval 3 every packet leads with 4 bytes that are not sample
         * frames: the packet's own byte length, which E-MU's Windows driver
         * reads and steps over (FINDINGS). Taking the frames from the first
         * byte instead puts every sample two thirds of a frame early and
         * scrambles all of them.
         *
         * The offset is taken from the packet rather than the rate, so it is
         * zero wherever the byte count already divides, which is every rate
         * up to 96 kHz. The read stays inside the packet either way: lead +
         * frames x bytes_per_frame is frActCount exactly. */
        uint32_t lead = f->frActCount % e->bytes_per_frame;
        emu_ring_write_s24(&gInputRing, (const uint8_t*)req->buffer + offset + lead, frames);
        atomic_fetch_add(&e->frames_captured, frames);

        offset += f->frReqCount;
    }

    if (e->stopping) { CFRunLoopStop(CFRunLoopGetCurrent()); return; }
    if (submit_capture(req) != kIOReturnSuccess) {
        resync(e);
        if (submit_capture(req) != kIOReturnSuccess) {
            e->stopping = true;
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
    }
}

static void playback_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    Request* req = (Request*)refcon;
    Engine* e = req->engine;

    if (result == kIOReturnIsoTooOld) resync(e);

    for (uint32_t i = 0; i < e->entries_per_request; i++) {
        const IOUSBLowLatencyIsocFrame* f = &req->frames[i];
        if (!emu_frame_ok(f->frStatus)) {
            if (!e->stopping) atomic_fetch_add(&e->usb_errors, 1);
            continue;
        }
        atomic_fetch_add(&e->frames_played,
                         emu_frames_in_packet(f->frActCount, e->bytes_per_frame));
    }

    /*
     * Timestamp from the kernel, not from this thread.
     *
     * mach_absolute_time() here reads the moment the completion *callback* ran,
     * which can only ever be later than the transfer -- never earlier. That
     * one-sided error does not average out: it biases the anchor late, which
     * makes the device look slower than it is, which makes Core Audio deliver
     * slower than the device consumes, which drains the ring. Measured at about
     * 1880 ppm, with callback delays up to 29 ms.
     *
     * frTimeStamp is recorded by the USB stack when the frame actually
     * completed. Using it is the whole reason the low-latency API reports it.
     */
    uint64_t stamp = 0;
    for (int32_t i = (int32_t)e->entries_per_request - 1; i >= 0; i--) {
        const IOUSBLowLatencyIsocFrame* f = &req->frames[i];
        if (!emu_frame_ok(f->frStatus) || f->frActCount == 0) continue;
        stamp = ((uint64_t)f->frTimeStamp.hi << 32) | (uint64_t)f->frTimeStamp.lo;
        if (stamp) break;
    }

    /* Fall back only if the stack left no usable stamp; a late anchor beats
     * none, and the next request will correct it. */
    timeline_publish(atomic_load_explicit(&e->frames_played, memory_order_relaxed),
                     stamp ? stamp : mach_absolute_time());

    if (e->stopping) return;
    if (submit_playback(req) != kIOReturnSuccess) {
        resync(e);
        if (submit_playback(req) != kIOReturnSuccess) e->stopping = true;
    }
}

/* --- setup ----------------------------------------------------------------- */

static bool open_device(Engine* e)
{
    /* The watcher decides which product is published; this opens that one.
     * NULL means nothing is published, so nothing should be starting. */
    watch_identity();
    e->identity = atomic_load_explicit(&gIdentity, memory_order_relaxed);
    if (!e->identity) return false;
    /* Pinned before the open, not after. A sibling arriving in between may
     * move the identity; the refresh this queues moves it back, ordered
     * behind that arrival, and every later arrival sees the pin. If instead
     * this device leaves, the lookup or the open fails and teardown unpins. */
    set_running_identity(e->identity);

    e->service = emu_find_product(e->identity->product_id);
    if (!e->service) return false;

    IOCFPlugInInterface** plugin = NULL;
    SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(e->service, kIOUSBDeviceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugin, &score)
        != KERN_SUCCESS || !plugin) {
        return false;
    }
    HRESULT hr = (*plugin)->QueryInterface(plugin,
                    CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&e->device);
    (*plugin)->Release(plugin);
    return !hr && e->device;
}

/* SET_CUR then GET_CUR compare. A rate that silently fails to apply, then an
 * alternate setting chosen for the rate we asked for, is what wedges this
 * device -- see docs/milestone-3-results.md. */
static bool set_clock_rate(Engine* e, const EmuDeviceModel* model, uint32_t hz)
{
    const EmuExtensionUnit* xu = NULL;
    for (uint8_t i = 0; i < model->num_extension_units; i++) {
        if (model->extension_units[i].extension_code == EMU_XU_CLOCK_RATE) {
            xu = &model->extension_units[i];
            break;
        }
    }
    if (!xu) return false;

    uint8_t want = emu_hz_to_rate_code(hz);
    if (want == 0xff) return false;

    if ((*e->device)->USBDeviceOpen(e->device) != kIOReturnSuccess) {
        if ((*e->device)->USBDeviceOpenSeize(e->device) != kIOReturnSuccess) return false;
    }

    EmuControlSetup setup;
    IOUSBDevRequestTO req;
    uint8_t value = want;
    bool ok = false;

    emu_setup_set_clock_rate(xu->unit_id, model->control_interface, &setup);
    memset(&req, 0, sizeof req);
    req.bmRequestType = setup.bm_request_type; req.bRequest = setup.b_request;
    req.wValue = setup.w_value; req.wIndex = setup.w_index; req.wLength = setup.w_length;
    req.pData = &value; req.noDataTimeout = 1000; req.completionTimeout = 1000;

    if ((*e->device)->DeviceRequestTO(e->device, &req) == kIOReturnSuccess) {
        uint8_t got = 0xff;
        emu_setup_get_clock_rate(xu->unit_id, model->control_interface, &setup);
        memset(&req, 0, sizeof req);
        req.bmRequestType = setup.bm_request_type; req.bRequest = setup.b_request;
        req.wValue = setup.w_value; req.wIndex = setup.w_index; req.wLength = setup.w_length;
        req.pData = &got; req.noDataTimeout = 1000; req.completionTimeout = 1000;

        ok = (*e->device)->DeviceRequestTO(e->device, &req) == kIOReturnSuccess
             && got == want;
    }

    (*e->device)->USBDeviceClose(e->device);
    return ok;
}

static bool select_alt(IOUSBInterfaceInterface500** intf, const EmuDeviceModel* model,
                       uint8_t interface_number, uint32_t rate,
                       const EmuAltSetting** chosen_out)
{
    const EmuAltSetting* chosen = NULL;
    for (uint16_t i = 0; i < model->num_alt_settings; i++) {
        const EmuAltSetting* a = &model->alt_settings[i];
        if (a->interface_number != interface_number || a->data_endpoint == 0) continue;
        if (a->sample_rate != rate) continue;
        /* The ring and the published stream format are stereo, so an alt
         * setting with any other channel count would be decoded at the wrong
         * stride. The 0404 USB offers four-channel alts at every rate it
         * offers stereo ones, which makes the rate alone ambiguous. */
        if (a->channels != EMU_RING_CHANNELS) continue;
        if (!chosen || a->interval > chosen->interval) chosen = a;
    }
    if (!chosen) return false;

    (*intf)->SetAlternateInterface(intf, 0);
    if ((*intf)->SetAlternateInterface(intf, chosen->alternate_setting) != kIOReturnSuccess) {
        return false;
    }
    *chosen_out = chosen;
    return true;
}

static void teardown(Engine* e)
{
    for (int i = 0; i < NUM_REQUESTS; i++) {
        if (e->in_requests[i].buffer && e->in_intf)
            (*e->in_intf)->LowLatencyDestroyBuffer(e->in_intf, e->in_requests[i].buffer);
        if (e->in_requests[i].frames && e->in_intf)
            (*e->in_intf)->LowLatencyDestroyBuffer(e->in_intf, e->in_requests[i].frames);
        if (e->out_requests[i].buffer && e->out_intf)
            (*e->out_intf)->LowLatencyDestroyBuffer(e->out_intf, e->out_requests[i].buffer);
        if (e->out_requests[i].frames && e->out_intf)
            (*e->out_intf)->LowLatencyDestroyBuffer(e->out_intf, e->out_requests[i].frames);
    }
    if (e->out_intf) {
        (*e->out_intf)->SetAlternateInterface(e->out_intf, 0);
        (*e->out_intf)->USBInterfaceClose(e->out_intf);
        (*e->out_intf)->Release(e->out_intf);
    }
    if (e->in_intf) {
        (*e->in_intf)->SetAlternateInterface(e->in_intf, 0);
        (*e->in_intf)->USBInterfaceClose(e->in_intf);
        (*e->in_intf)->Release(e->in_intf);
    }
    if (e->device) (*e->device)->Release(e->device);
    if (e->service) IOObjectRelease(e->service);
    if (e->identity) set_running_identity(NULL);
    memset(e, 0, sizeof *e);
}

static void* engine_thread(void* arg)
{
    (void)arg;
    Engine* e = &gEngine;
    memset(e, 0, sizeof *e);
    emu_ring_reset(&gOutputRing);
    emu_ring_reset(&gInputRing);

    e->feedback = emu_feedback_init(e->feedback_storage);
    if (!e->feedback) { gRunning = false; return NULL; }

    if (!open_device(e)) { teardown(e); gRunning = false; return NULL; }

    IOUSBConfigurationDescriptorPtr cfg = NULL;
    EmuDeviceModel model;
    if ((*e->device)->GetConfigurationDescriptorPtr(e->device, 0, &cfg) != kIOReturnSuccess ||
        emu_parse_config_descriptor((const uint8_t*)cfg,
                                    OSSwapLittleToHostInt16(cfg->wTotalLength), &model) != 0) {
        teardown(e); gRunning = false; return NULL;
    }

    if (!set_clock_rate(e, &model, gSampleRate)) { teardown(e); gRunning = false; return NULL; }

    if (!emu_find_interface(e->device, 1, &e->out_intf) ||
        !emu_find_interface(e->device, 2, &e->in_intf)) {
        teardown(e); gRunning = false; return NULL;
    }
    if ((*e->out_intf)->USBInterfaceOpen(e->out_intf) != kIOReturnSuccess ||
        (*e->in_intf)->USBInterfaceOpen(e->in_intf) != kIOReturnSuccess) {
        teardown(e); gRunning = false; return NULL;
    }

    const EmuAltSetting *out_alt = NULL, *in_alt = NULL;
    if (!select_alt(e->out_intf, &model, 1, gSampleRate, &out_alt) ||
        !select_alt(e->in_intf, &model, 2, gSampleRate, &in_alt)) {
        teardown(e); gRunning = false; return NULL;
    }

    if (!emu_find_isoc_pipe(e->out_intf, kUSBOut, &e->out_pipe, &e->out_max) ||
        !emu_find_isoc_pipe(e->in_intf, kUSBIn, &e->in_pipe, &e->in_max)) {
        teardown(e); gRunning = false; return NULL;
    }

    e->bytes_per_frame = out_alt->channels * out_alt->subframe_size;
    if (e->bytes_per_frame == 0) e->bytes_per_frame = 6;

    uint32_t period = 1u << (out_alt->interval - 1);
    e->entries_per_ms = period >= 8 ? 1 : (8 / period);
    e->entries_per_request = REQUEST_MS * e->entries_per_ms;
    if (e->entries_per_request > MAX_ENTRIES) e->entries_per_request = MAX_ENTRIES;

    double interval_ms = (double)period * 0.125;
    e->nominal_frames = (uint32_t)(gSampleRate * interval_ms / 1000.0);
    emu_feedback_set_nominal(e->feedback, gSampleRate, (uint64_t)(interval_ms * 1e6));

    CFRunLoopSourceRef in_source = NULL, out_source = NULL;
    if ((*e->in_intf)->CreateInterfaceAsyncEventSource(e->in_intf, &in_source) != kIOReturnSuccess ||
        (*e->out_intf)->CreateInterfaceAsyncEventSource(e->out_intf, &out_source) != kIOReturnSuccess) {
        teardown(e); gRunning = false; return NULL;
    }
    e->run_loop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(e->run_loop, in_source, kCFRunLoopDefaultMode);
    CFRunLoopAddSource(e->run_loop, out_source, kCFRunLoopDefaultMode);

    UInt32 list_bytes = e->entries_per_request * sizeof(IOUSBLowLatencyIsocFrame);
    for (int i = 0; i < NUM_REQUESTS; i++) {
        e->in_requests[i].engine = e;
        e->out_requests[i].engine = e;
        if ((*e->in_intf)->LowLatencyCreateBuffer(e->in_intf, &e->in_requests[i].buffer,
                (UInt32)e->entries_per_request * e->in_max, kUSBLowLatencyReadBuffer) != kIOReturnSuccess ||
            (*e->in_intf)->LowLatencyCreateBuffer(e->in_intf, (void**)&e->in_requests[i].frames,
                list_bytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess ||
            (*e->out_intf)->LowLatencyCreateBuffer(e->out_intf, &e->out_requests[i].buffer,
                (UInt32)e->entries_per_request * e->out_max, kUSBLowLatencyWriteBuffer) != kIOReturnSuccess ||
            (*e->out_intf)->LowLatencyCreateBuffer(e->out_intf, (void**)&e->out_requests[i].frames,
                list_bytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess) {
            teardown(e); gRunning = false; return NULL;
        }
    }

    UInt64 now = 0;
    AbsoluteTime at;
    if ((*e->in_intf)->GetBusFrameNumber(e->in_intf, &now, &at) != kIOReturnSuccess) {
        teardown(e); gRunning = false; return NULL;
    }
    e->next_in_frame = now + SCHEDULE_MARGIN;
    e->next_out_frame = now + SCHEDULE_MARGIN;

    /* Capture first, so playback has measurements waiting instead of starving
     * through its whole first request. */
    for (int i = 0; i < NUM_REQUESTS; i++) {
        if (submit_capture(&e->in_requests[i]) != kIOReturnSuccess) {
            teardown(e); gRunning = false; return NULL;
        }
    }
    for (int i = 0; i < NUM_REQUESTS; i++) {
        if (submit_playback(&e->out_requests[i]) != kIOReturnSuccess) {
            teardown(e); gRunning = false; return NULL;
        }
    }

    while (!e->stopping) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    }

    (*e->out_intf)->AbortPipe(e->out_intf, e->out_pipe);
    (*e->in_intf)->AbortPipe(e->in_intf, e->in_pipe);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);

    CFRunLoopRemoveSource(e->run_loop, in_source, kCFRunLoopDefaultMode);
    CFRunLoopRemoveSource(e->run_loop, out_source, kCFRunLoopDefaultMode);
    CFRelease(in_source);
    CFRelease(out_source);

    teardown(e);
    gRunning = false;
    return NULL;
}

bool emu_engine_start(uint32_t sample_rate)
{
    if (gRunning) return true;
    gSampleRate = sample_rate;
    emu_ring_reset(&gOutputRing);
    emu_ring_reset(&gInputRing);
    gTimelineFrames = 0;
    gTimelineHost = 0;
    gRunning = true;

    if (pthread_create(&gThread, NULL, engine_thread, NULL) != 0) {
        gRunning = false;
        return false;
    }
    return true;
}

void emu_engine_stop(void)
{
    if (!gRunning) return;
    gEngine.stopping = true;
    if (gEngine.run_loop) CFRunLoopStop(gEngine.run_loop);
    pthread_join(gThread, NULL);
    gRunning = false;
}
