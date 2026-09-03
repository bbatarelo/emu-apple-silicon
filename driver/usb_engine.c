/*
 * USB engine for the HAL plug-in.
 *
 * The duplex transport proven in Milestone 4, joined to Core Audio through
 * timeline-indexed rings. Everything load-bearing about the transport was
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
 *
 * The shape of the transport follows from the two budgets Core Audio's model
 * leaves to a driver, kept separate on purpose:
 *
 *   - Playback data is late-bound, and bound by Core Audio itself. A
 *     request's frame list -- packet count and sizes -- is fixed when it is
 *     submitted, but its buffer goes out zeroed; the engine publishes the
 *     slice of the timeline that request carries, and WriteMix converts the
 *     audio straight into it on Core Audio's own IO thread. Low-latency
 *     buffers are what make that legal: shared, wired memory the controller
 *     reads at transmission time (the kernel updating our frame lists in
 *     place is the same memory working the other way). No engine thread sits
 *     on the data path at all, so the safety offset buys tolerance for one
 *     thread rather than two, and completion-delivery jitter -- which no
 *     thread policy bounds -- is absorbed by schedule depth instead. This is
 *     what the original kext did from clipOutputSamples. 'emuS' tunes the
 *     offset; see bind_publish and write_output_bind.
 *   - The frame lists are queued far deeper than the audio is written:
 *     num_requests x REQUEST_MS of bus schedule in flight, which costs wired
 *     memory and nothing in latency. A stall shorter than that is one silent
 *     stretch and has no other consequence. Only a stall that outlasts it
 *     leaves the schedule stale, and rebuilding the schedule is then a
 *     dropout of known length on a clock that never stopped: the cursors
 *     skip the dead bus time and the timeline stays continuous; see
 *     reschedule().
 *   - The engine thread runs under a time-constraint policy: it must keep
 *     the schedule ahead of the bus and re-anchor the clock every couple of
 *     milliseconds, and a default-priority thread's scheduling hiccups would
 *     eventually stale the schedule.
 *
 * The timeline is the device's own and is never guessed: the anchor for
 * sample 0 is the scheduled bus time of the first packet, published before
 * emu_engine_start returns, and every completed request re-anchors it from the
 * frame list's hardware timestamps through the Rust core's critically damped
 * filter. Core Audio never sees a host-clock placeholder, so there is no
 * splice when the real clock appears; a spliced timeline stalls coreaudiod's
 * IO thread for the length of the discrepancy, an audible dropout (FINDINGS).
 */

#include "usb_engine.h"
#include "ring.h"
#include "../shared/usb_util.h"
#include <os/log.h>
#include "../shared/device.h"

#include <errno.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include "../rust/emu-ca0189/include/emu_ca0189.h"

/* Requests in flight per direction: the depth of the bus schedule. It is what
 * a scheduling stall has to outlast before the schedule goes stale and has to
 * be rebuilt, and -- because Core Audio writes into the submitted buffers
 * themselves -- it is also what bounds how far ahead Core Audio may write.
 * A frame no submitted request covers has nowhere to go, so the schedule must
 * reach past the write lead, which is not the safety offset but (FINDINGS)
 *
 *     writeLead ~ 2 x bufferFrames + safetyOffset + ~208 frames
 *
 * The running value is e->num_requests, computed per session by
 * schedule_depth() from the rate, the offset and the largest buffer the HAL
 * will grant. Deepening costs wired memory and feedback-servo lag, not
 * latency. MAX_REQUESTS is well clear of the worst combination as the
 * constants stand: 87 at 44.1 kHz against the 20 ms offset ceiling. */
#define MAX_REQUESTS     128
#define MIN_REQUESTS     16

/* The largest IO buffer the HAL will hand a client on this device. Not a
 * Core Audio constant: it follows from the zero-timestamp period the plug-in
 * publishes (see EMU_ZERO_TIMESTAMP_PERIOD), so it is derived here rather
 * than measured and pasted. */
#define HAL_MAX_IO_BUFFER  (EMU_ZERO_TIMESTAMP_PERIOD * 3u / 8u > 4096u \
                            ? 4096u : EMU_ZERO_TIMESTAMP_PERIOD * 3u / 8u)
/* The residual in the write-lead law below: peak IO-cycle jitter, measured at
 * ~208 frames and rounded up. */
#define WRITE_LEAD_SLACK   256u
/* One request's span, in bus frames (ms). Also the completion cadence, which
 * sets how finely the schedule advances and how often the clock re-anchors. */
#define REQUEST_MS    2
#define MAX_ENTRIES   (REQUEST_MS * 8)
/* Requests kept in flight on the explicit feedback endpoint. Its value moves
 * every 32 ms, so this is about keeping the pipe fed, not about latency. */
#define FB_REQUESTS   4
/* How far ahead of the bus clock a schedule begins, at engine start and after
 * a rebuild. The stack needs the first frame to lie in the future; after a
 * rebuild every millisecond here is dead air, so it is small. */
#define SCHEDULE_LEAD_MS 4

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

/*
 * The device's name for log lines.
 *
 * Every message used to be prefixed "TrackerPre:" whatever was plugged in,
 * which is wrong the moment the driver serves the rest of the family -- a 0404
 * reporting that the Tracker Pre gave up sends the reader after the wrong
 * hardware. Deliberately not emu_engine_device_name(): this only reads what is
 * already known, so it never arms the hot-plug watch as a side effect of
 * logging, and it stays short enough to sit in front of every line.
 */
const char* emu_engine_log_name(void)
{
    const EmuDeviceIdentity* id = atomic_load_explicit(&gRunningIdentity, memory_order_relaxed);
    if (!id) id = atomic_load_explicit(&gIdentity, memory_order_relaxed);
    return id ? id->name : "E-MU device";
}

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

    /* The timeline allocation made at submit -- which frames this request
     * carries, and in which packet sizes. Published to the IO thread as this
     * request's slice of the map; entry_frames also drives the frames-played
     * accounting at completion, for entries the bus never carried. */
    uint64_t                   data_frame_start;
    uint64_t                   data_frame_end;
    uint32_t                   entry_frames[MAX_ENTRIES];

    /* Which playback schedule this request was submitted into. After a
     * rebuild, completions still draining from the old schedule carry
     * timestamps the rebased filter must not see; their frames still count. */
    uint64_t                   generation;
} Request;

typedef enum { ENGINE_IDLE, ENGINE_STARTING, ENGINE_STREAMING, ENGINE_FAILED } EngineStartState;

typedef struct {
    /* Even: stable. Odd: the engine is rewriting this slot. */
    _Atomic uint32_t seq;
    _Atomic(uint8_t*) buffer;
    _Atomic uint64_t  frame_start;
    _Atomic uint64_t  frame_end;
} BindSlot;

struct Engine {
    const EmuDeviceIdentity*     identity;   /* what this opened; pinned while set */
    IOUSBDeviceInterface500**    device;
    io_service_t                 service;
    IOUSBInterfaceInterface500** in_intf;
    IOUSBInterfaceInterface500** out_intf;

    uint8_t  in_pipe, out_pipe;
    uint16_t in_max, out_max;

    /* The explicit feedback endpoint on the playback interface. It keeps its
     * own bInterval -- 4 on this hardware, one entry per millisecond, even at
     * the rates whose data endpoint is serviced twice as often -- so it is
     * queued on its own geometry rather than the data pipe's. Zero when the
     * device has none, in which case nothing below runs. */
    uint8_t  fb_pipe;
    uint16_t fb_max;
    uint8_t  fb_interval;
    uint32_t fb_entries_per_request;
    uint32_t fb_num_requests;
    uint64_t next_fb_frame;
    /* Q16.16 residue of the device-sourced planner, engine thread only. */
    uint32_t fb_residue_q16;
    /* What the rate and the service interval say a packet should hold, Q16.16
     * and exact: the fractional rates cannot be checked against a truncated
     * integer. Also the centre of the band a feedback value must land in. */
    uint32_t fb_nominal_q16;
    uint32_t bytes_per_frame;
    uint32_t entries_per_ms;
    uint32_t entries_per_request;
    uint32_t nominal_frames;
    uint32_t sample_rate;
    uint64_t ticks_per_ms;


    uint64_t next_in_frame, next_out_frame;
    Request  in_requests[MAX_REQUESTS];
    Request  out_requests[MAX_REQUESTS];
    /* Four milliseconds of feedback in flight is ample for a value that
     * changes every thirty-two, and it keeps the endpoint off the schedule
     * depth the data pipes need. */
    Request  fb_requests[FB_REQUESTS];
    /* Requests actually allocated and kept in flight this session: see the
     * MAX_REQUESTS block. Never changes while streaming. */
    uint32_t num_requests;

    /* Timeline cursors: the next frame index each stream will touch on the
     * shared sample timeline. Output allocates its slice of the timeline here
     * at submit time; capture writes the input ring here on completion. Both count from 0 at the
     * scheduled stream start, which is what makes Core Audio's cycle sample
     * times land on the same slots -- and both keep bus time, not delivery;
     * see the note above the completions. */
    uint64_t out_cursor;
    uint64_t in_cursor;

    _Alignas(16) uint8_t feedback_storage[2048];
    EmuFeedback* feedback;

    _Alignas(16) uint8_t ts_filter_storage[256];
    EmuTsFilter* ts_filter;

    /* Frames the device has consumed: the planned size of every completed
     * playback entry, carried or not, plus every frame of dead bus time a
     * rebuild skipped. Core Audio's timeline is anchored to this rather than
     * to the host clock, so the two cannot drift. */
    _Atomic uint64_t frames_played;
    /* Frames the input ring has been written through, silence included;
     * always equal to in_cursor. */
    _Atomic uint64_t frames_captured;
    _Atomic uint64_t usb_errors;
    _Atomic uint64_t ts_fallbacks;
    _Atomic uint64_t resyncs;
    _Atomic uint64_t dead_frames;
    _Atomic uint64_t unfilled_playback;
    _Atomic uint64_t empty_capture;
    /* Playback entries the bus reported good but did not carry in full. The
     * completion side credits the planned size whatever happened, so a packet
     * the controller skipped leaves the timeline straight, every other counter
     * clean, and one packet of silence in the audio -- indistinguishable from
     * the device discarding it, unless this is counted. */
    _Atomic uint64_t short_playback;
    bool             schedule_clamped;

    /* Mirrors of counters owned by the Rust transport objects. Those objects
     * belong exclusively to the engine thread; property/diagnostic threads
     * read these publications instead of racing their mutable internals. */
    _Atomic uint32_t feedback_starved;
    _Atomic uint32_t feedback_overflows;
    _Atomic uint32_t ts_resets;

    /* Published feedback state. Written on the engine thread from the
     * feedback completion, read by the diagnostics path. */
    _Atomic uint64_t fb_packets;
    _Atomic uint64_t fb_silent;
    _Atomic uint64_t fb_errors;
    _Atomic uint64_t fb_rejected;
    _Atomic uint64_t fb_changes;
    _Atomic uint32_t fb_value_q16;
    _Atomic uint32_t fb_min_q16;
    _Atomic uint32_t fb_max_q16;

    /* Bumped whenever the playback schedule is rebuilt. Engine thread only. */
    uint64_t generation;

    /* Where the filter's reset count stood at the last counter reset,
     * subtracted out so a measurement window starts from zero. */
    _Atomic uint64_t ts_resets_zero;

    /* A stop request carries no payload: relaxed atomic access is sufficient,
     * and CFRunLoopStop supplies the wakeup when the request comes externally. */
    /* Stopping is an external request: Core Audio asked the engine to stop and
     * the thread should exit. Faulted is the transport failing underneath it,
     * which is not a reason to exit -- it is a reason to rebuild. Conflating
     * the two is what turned one bad submission into a day of silence. */

    /* ---- was file-scope state; per device now, so a second engine is a
     * second instance rather than a second copy of the driver. ---- */
    EmuRing input_ring;
    pthread_t thread;
    _Atomic bool running;
    bool thread_joinable;
    pthread_mutex_t lifecycle_lock;
    pthread_mutex_t stats_lock;
    EmuEngineStats final_stats;
    bool have_final_stats;
    uint32_t requested_rate;
    uint32_t safety_us;
    bool with_input;
    _Atomic bool streaming;
    _Atomic uint32_t fault_mode;
    _Atomic uint32_t fault_countdown;
    pthread_mutex_t start_lock;
    pthread_cond_t start_cond;
    EngineStartState start_state;
    _Atomic float output_gain;
    BindSlot bind_map[MAX_REQUESTS];
    _Atomic uint32_t bind_count;
    _Atomic uint32_t bind_gate;
    _Atomic uint32_t bind_bpf;
    _Atomic uint64_t bind_frontier;
    _Atomic uint64_t bind_frames;
    _Atomic uint64_t bind_unmapped;
    _Atomic uint64_t bind_unmapped_ahead;
    _Atomic uint64_t bind_write_lead;
    _Atomic uint64_t bind_races;
    _Atomic uint64_t bind_missing;
    _Atomic uint32_t timeline_seq;
    _Atomic uint64_t timeline_frames;
    _Atomic uint64_t timeline_host;

    void (* _Atomic failure_handler)(void);
    _Atomic bool stopping;
    _Atomic bool faulted;

    _Atomic uint64_t recoveries;
    _Atomic uint64_t recovery_failures;

    CFRunLoopRef  run_loop;
};

/* StartIO/StopIO are control-thread calls, so serialize the complete
 * create/join lifecycle and the reuse of the singleton Engine here. */
/* Diagnostics are control-path work. This protects the final snapshot and
 * the singleton Engine's transition between sessions without ever entering
 * the Core Audio IO path. Live counters themselves remain atomic. */
/* Whether this session opens the capture interface at all.
 *
 * With capture open its packet lengths size the playback packets; without it
 * the explicit feedback endpoint does (planner_next), so playback can run
 * alone. That is the workaround for setups on which duplex at 176.4 and
 * 192 kHz drops playback packets while IN transactions are on the bus
 * (FINDINGS). The timeline is unaffected either way: it is anchored to
 * frames_played, which playback completions maintain. */

/* Startup handshake: emu_engine_start blocks until the engine thread has the
 * streams scheduled and the timeline anchor published, or has failed. */
/*
 * The engine had no logging at all until a stream died silently and stayed
 * dead for a day and a half: Core Audio went on running IO cycles at real
 * time, every plug-in counter went on advancing, and the only trace of the
 * fault in the whole system log was an "IO started" line with no matching
 * "IO stopped". Faults log at os_log_error so they survive the default level:
 *
 *   log show --predicate 'subsystem == "net.quantum-bit.EMUTrackerPre"'
 */
static os_log_t engine_log(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create(EMU_LOG_SUBSYSTEM, "engine"); });
    return log;
}
/* The os_log category already says "engine", so the prefix carries the one
 * thing the category cannot: which device this is about. */
#define ENG_LOG(fmt, ...)   os_log(engine_log(), "%{public}s: " fmt, emu_engine_log_name(), ##__VA_ARGS__)
#define ENG_ERR(fmt, ...)   os_log_error(engine_log(), "%{public}s: " fmt, emu_engine_log_name(), ##__VA_ARGS__)
#define ENG_DEBUG(fmt, ...) os_log_debug(engine_log(), "%{public}s: " fmt, emu_engine_log_name(), ##__VA_ARGS__)

/* Rebuild attempts before the engine gives up and says so. Six with the
 * backoff below spans about eight seconds, which covers a hub renegotiating or
 * a device re-enumerating without leaving an absent device retrying forever. */
#define MAX_RECOVERY_ATTEMPTS 6
/* How long a session must have run before it counts as having recovered, and
 * so restores the retry budget. Long enough that a stream which faults straight
 * back cannot refresh it indefinitely. */
#define RECOVERY_STABLE_SECONDS 5.0
#define RECOVERY_BACKOFF_MS(attempt) (50u << ((attempt) < 5 ? (attempt) : 5))

/* Submissions a TRANSIENT injected fault fails before clearing itself. Two
 * failures fault a running stream (one submit, one after the reschedule) and
 * the third fails the first rebuild, so a test sees a failed rebuild and a
 * successful one and still lands well inside the retry budget. A budget large
 * enough to exhaust that would make "transient" indistinguishable from
 * "persistent", which is the distinction this knob exists for. */
#define TRANSIENT_FAULT_SUBMITS 3

/* Transfers actually on the bus, as opposed to e->running's "a start was
 * requested and no stop has arrived yet". */
/* Invoked once when the engine gives up for good. */


void emu_engine_inject_fault(EmuEngine* e, EmuFaultMode mode)
{
    if (!e) return;
    atomic_store_explicit(&e->fault_countdown,
                          mode == EMU_FAULT_TRANSIENT ? TRANSIENT_FAULT_SUBMITS : 0,
                          memory_order_relaxed);
    atomic_store_explicit(&e->fault_mode, (uint32_t)mode, memory_order_relaxed);
    ENG_LOG("fault injection set to %u", (unsigned)mode);
}

/* True when this submission should be failed on purpose. */
static bool fault_should_fail(Engine* e)
{
    uint32_t mode = atomic_load_explicit(&e->fault_mode, memory_order_relaxed);
    if (mode == EMU_FAULT_NONE) return false;
    if (mode == EMU_FAULT_PERSISTENT) return true;

    uint32_t left = atomic_load_explicit(&e->fault_countdown, memory_order_relaxed);
    while (left > 0) {
        if (atomic_compare_exchange_weak_explicit(&e->fault_countdown, &left, left - 1,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            return true;
        }
    }
    atomic_store_explicit(&e->fault_mode, EMU_FAULT_NONE, memory_order_relaxed);
    return false;
}


/* Written by whichever thread handles a volume change, read on the USB fill
 * path. Plain atomic load/store; no ordering is needed beyond not tearing,
 * since a gain that lands one packet late is inaudible. */

/* --- the bind map: the schedule, published to the IO thread ----------------
 *
 * How Core Audio reaches the USB buffers. Rather than staging the mix and
 * having the engine thread convert it shortly before transmission, Core Audio
 * converts straight into the USB request that will carry those frames --
 * which is what the original kext did from clipOutputSamples, and why its
 * offset could be ~1 ms: there was no second thread on the data path to be
 * late.
 *
 * What makes it possible here is that a request's byte layout is *linear* in
 * the timeline. Entries are contiguous and frReqCount is the packet's real
 * size (emu_output_packet_bytes is frames x bytes_per_frame, nothing else), so
 * for any frame the request covers,
 *
 *     byte offset = (frame - data_frame_start) x bytes_per_frame
 *
 * and the whole map is three numbers per request: where its slice of the
 * timeline starts, where it ends, and the buffer. Variable packet sizes never
 * enter it. That is the only reason this is a lookup and not a table of
 * per-entry offsets that would have to be published in lockstep.
 *
 * The hazard is recycling. A slot is reused ~num_requests x REQUEST_MS
 * after it last transmitted, for frames that far ahead of the play head, while
 * the IO thread writes at most one buffer past the safety offset -- so in the
 * steady state a writer and a resubmit are a dozen milliseconds apart on the
 * timeline and cannot meet. Under a stall they can. A seqlock per slot makes
 * that detectable rather than silent: the engine marks the slot odd before it
 * touches anything, publishes and marks it even after the submission
 * succeeded, and a writer whose sequence moved under it knows its bytes went
 * into a buffer that now means something else. It cannot take them back --
 * the damage is one packet inside a stretch that is already glitching -- but
 * `bindRaces` says so, which is the whole difference between a known cost and
 * an unexplained noise.
 *
 * Teardown is the hazard a staging buffer would not have had: these are freed
 * while the IO thread may still be inside one. A single gate combines map
 * liveness with its writer bit. Admission and closure therefore have one
 * atomic modification order: the writer either enters before close and is
 * waited for, or observes the closed bit and never touches a buffer.
 */

/* How many slots of e->bind_map are live, so the IO thread's lookup scans the
 * schedule it actually has rather than the maximum it could have. */
/* Map liveness and writer ownership share one atomic modification order. */
#define BIND_GATE_WRITING UINT32_C(1)
#define BIND_GATE_CLOSED  (UINT32_C(1) << 31)
/* The IO thread's own frontier: the first frame it has not written. The
 * output lead and the unwritten-frame count are both measured against it. */
/* Of the unmapped, those past the far end of the queue: frames Core Audio
 * wrote for bus time the engine has not scheduled yet. Tells the two failures
 * apart -- a write horizon deeper than the schedule (this) from audio for an
 * interval already written off (the rest). */
/* How far ahead of the play head Core Audio's writes actually reach, in
 * frames -- the high-water mark of (cycle end - frames played). This is the
 * number that sets how deep the request queue has to be for the direct path:
 * a map of submitted requests has no buffer at all for a frame no request
 * covers, so the schedule has to reach past where Core Audio writes. Nothing else in the driver knows it, because nothing else
 * needed to. */


/* Stage 1 keeps exactly one instance, but it is reached only through a handle
 * and owns all of its own state, so a second device is a second create() call
 * rather than a second copy of the driver. */
static Engine gTheEngine;
static bool   gTheEngineTaken;

EmuEngine* emu_engine_create(void)
{
    if (gTheEngineTaken) return NULL;
    Engine* e = &gTheEngine;
    memset(e, 0, sizeof *e);
    pthread_mutex_init(&e->lifecycle_lock, NULL);
    pthread_mutex_init(&e->stats_lock, NULL);
    pthread_mutex_init(&e->start_lock, NULL);
    pthread_cond_init(&e->start_cond, NULL);
    atomic_init(&e->running, false);
    atomic_init(&e->streaming, false);
    atomic_init(&e->stopping, false);
    atomic_init(&e->faulted, false);
    atomic_init(&e->output_gain, 1.0f);
    atomic_init(&e->bind_gate, BIND_GATE_CLOSED);
    e->start_state = ENGINE_IDLE;
    e->requested_rate = 48000;
    e->safety_us = 10000;
    e->with_input = true;
    gTheEngineTaken = true;
    return e;
}

void emu_engine_destroy(EmuEngine* e)
{
    if (!e) return;
    emu_engine_stop(e);
    pthread_mutex_destroy(&e->lifecycle_lock);
    pthread_mutex_destroy(&e->stats_lock);
    pthread_mutex_destroy(&e->start_lock);
    pthread_cond_destroy(&e->start_cond);
    gTheEngineTaken = false;
}
static void bind_reset(Engine* e)
{
    /* The preceding teardown drained the admitted writer. Keep admission
     * closed until the new session's complete map has been published. */
    atomic_store_explicit(&e->bind_gate, BIND_GATE_CLOSED, memory_order_relaxed);
    atomic_store_explicit(&e->bind_count, 0, memory_order_relaxed);
    for (int i = 0; i < MAX_REQUESTS; i++) {
        atomic_store_explicit(&e->bind_map[i].seq, 0, memory_order_relaxed);
        atomic_store_explicit(&e->bind_map[i].buffer, NULL, memory_order_relaxed);
        atomic_store_explicit(&e->bind_map[i].frame_start, 0, memory_order_relaxed);
        atomic_store_explicit(&e->bind_map[i].frame_end, 0, memory_order_relaxed);
    }
    atomic_store_explicit(&e->bind_frontier, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_unmapped, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_unmapped_ahead, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_write_lead, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_races, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_missing, 0, memory_order_relaxed);
}

/* Engine thread: this slot no longer describes anything a writer may touch.
 *
 * The fence is load-bearing and easy to lose. A release *store* orders what
 * came before it, not what comes after -- so without the fence the memset and
 * the new range in submit_playback may become visible ahead of the odd
 * marker, and a writer would see a clean sequence over a half-rewritten
 * entry: exactly the tear the seqlock exists to prevent, reported as no race
 * at all. This is the Linux write_seqlock shape: bump, then smp_wmb. */
static void bind_retire(Engine* e, size_t idx)
{
    uint32_t seq = atomic_load_explicit(&e->bind_map[idx].seq, memory_order_relaxed);
    if (seq & 1u) return;
    atomic_store_explicit(&e->bind_map[idx].seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

/* Engine thread: this request is on the bus and its buffer is zeroed; here is
 * the slice of the timeline it carries. */
static void bind_publish(Engine* e, size_t idx, uint8_t* buffer, uint64_t start, uint64_t end)
{
    BindSlot* s = &e->bind_map[idx];
    uint32_t seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
    if (!(seq & 1u)) {                       /* retire first, always */
        atomic_store_explicit(&s->seq, ++seq, memory_order_relaxed);
        atomic_thread_fence(memory_order_release);   /* see bind_retire */
    }
    atomic_store_explicit(&s->buffer, buffer, memory_order_relaxed);
    atomic_store_explicit(&s->frame_start, start, memory_order_relaxed);
    atomic_store_explicit(&s->frame_end, end, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 1, memory_order_release);
}

/*
 * Core Audio's IO thread: convert this cycle's mix into whichever submitted
 * requests carry it.
 *
 * The range spans several requests -- a 512-frame cycle is ~11 ms against a
 * 2 ms request -- so this walks it in pieces. A frame no slot covers is not
 * an error to hide: before the first request of a rebuilt schedule it is
 * audio for bus time that has already been written off as dead (reschedule),
 * and past the end of the queue it is a cycle further ahead than the driver
 * has scheduled. Either way the frames have nowhere to go; they are counted
 * and dropped, and the walk skips to the next slot that does begin ahead of
 * here rather than probing frame by frame.
 */
static void write_output_bind(Engine* e, const float* src, uint32_t count, uint64_t pos)
{
    uint32_t bpf  = atomic_load_explicit(&e->bind_bpf, memory_order_relaxed);
    float    gain = atomic_load_explicit(&e->output_gain, memory_order_relaxed);
    if (bpf == 0) return;

    uint64_t played = atomic_load_explicit(&e->frames_played, memory_order_relaxed);
    if (pos + count > played) {
        uint64_t lead = pos + count - played;
        uint64_t seen = atomic_load_explicit(&e->bind_write_lead, memory_order_relaxed);
        if (lead > seen) atomic_store_explicit(&e->bind_write_lead, lead, memory_order_relaxed);
    }

    /* Constant for the session. The outer gate keeps teardown/restart from
     * changing the map while this callback is admitted, so load it once per
     * IO cycle rather than once per request-sized slice. */
    uint32_t slots = atomic_load_explicit(&e->bind_count, memory_order_acquire);
    uint64_t bound = 0, unmapped = 0, unmapped_ahead = 0, races = 0;
    uint32_t done = 0;
    while (done < count) {
        uint64_t frame = pos + done;

        int      hit = -1;
        uint32_t seq0 = 0;
        uint8_t* buffer = NULL;
        uint64_t start = 0, end = 0, next_start = UINT64_MAX, queue_end = 0;

        for (uint32_t i = 0; i < slots; i++) {
            uint32_t s = atomic_load_explicit(&e->bind_map[i].seq, memory_order_acquire);
            if (s & 1u) continue;                      /* being rewritten */
            uint64_t st = atomic_load_explicit(&e->bind_map[i].frame_start, memory_order_relaxed);
            uint64_t en = atomic_load_explicit(&e->bind_map[i].frame_end, memory_order_relaxed);
            if (frame >= st && frame < en) {
                hit = i; seq0 = s; start = st; end = en;
                buffer = atomic_load_explicit(&e->bind_map[i].buffer, memory_order_relaxed);
                break;
            }
            if (st > frame && st < next_start) next_start = st;
            if (en > queue_end) queue_end = en;
        }

        if (hit < 0 || !buffer) {
            uint64_t skip = count - done;
            if (next_start != UINT64_MAX && next_start - frame < skip) skip = next_start - frame;
            unmapped += skip;
            if (frame >= queue_end) {
                unmapped_ahead += skip;
            }
            done += (uint32_t)skip;
            continue;
        }

        uint32_t n = (uint32_t)(end - frame);
        if (n > count - done) n = count - done;

        emu_pack_s24(buffer + (size_t)(frame - start) * bpf,
                     src + (size_t)done * EMU_RING_CHANNELS, n, gain);

        /* Recycled under us: the bytes just written belong to a slice of the
         * timeline this request no longer carries. This is diagnostics only:
         * in normal operation the schedule keeps the two accesses far apart;
         * after a schedule-sized stall the surrounding interval is already a
         * dropout. A compiler barrier keeps the check after the pack without
         * putting a full hardware fence on every request slice. The CPU may
         * still miss a pathological overlap, which is acceptable for a
         * best-effort counter that has no bearing on recovery or safety. */
        atomic_signal_fence(memory_order_seq_cst);
        if (atomic_load_explicit(&e->bind_map[hit].seq, memory_order_relaxed) != seq0) {
            races++;
        } else {
            bound += n;
        }
        done += n;
    }

    uint64_t frontier = atomic_load_explicit(&e->bind_frontier, memory_order_relaxed);
    if (pos + count > frontier) {
        atomic_store_explicit(&e->bind_frontier, pos + count, memory_order_release);
    }
    /* Diagnostics are callback-granularity observations. Aggregate locally so
     * the normal path performs one relaxed RMW, not one per 2 ms map slice. */
    if (bound) atomic_fetch_add_explicit(&e->bind_frames, bound, memory_order_relaxed);
    if (unmapped)
        atomic_fetch_add_explicit(&e->bind_unmapped, unmapped, memory_order_relaxed);
    if (unmapped_ahead)
        atomic_fetch_add_explicit(&e->bind_unmapped_ahead, unmapped_ahead,
                                  memory_order_relaxed);
    if (races) atomic_fetch_add_explicit(&e->bind_races, races, memory_order_relaxed);
}

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

static void timeline_publish(Engine* e, uint64_t frames, uint64_t host_time)
{
    uint32_t seq = atomic_load_explicit(&e->timeline_seq, memory_order_relaxed);
    atomic_store_explicit(&e->timeline_seq, seq + 1, memory_order_relaxed);  /* odd */
    /* A release store only orders earlier accesses. The fence keeps the new
     * pair from becoming visible before readers see the odd marker. */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&e->timeline_frames, frames, memory_order_relaxed);
    atomic_store_explicit(&e->timeline_host, host_time, memory_order_relaxed);
    atomic_store_explicit(&e->timeline_seq, seq + 2, memory_order_release);  /* even */
}

bool emu_engine_timeline(EmuEngine* e, uint64_t* frames, uint64_t* host_time)
{
    if (!e) return false;
    if (!atomic_load_explicit(&e->running, memory_order_acquire)) return false;

    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t before = atomic_load_explicit(&e->timeline_seq, memory_order_acquire);
        if (before & 1u) continue;                 /* write in progress */
        uint64_t f = atomic_load_explicit(&e->timeline_frames, memory_order_relaxed);
        uint64_t h = atomic_load_explicit(&e->timeline_host, memory_order_relaxed);
        /* Keep both pair loads ahead of the validation load. This is the read
         * side of the seqlock; an acquire load alone orders only what follows. */
        atomic_thread_fence(memory_order_acquire);
        uint32_t after = atomic_load_explicit(&e->timeline_seq, memory_order_relaxed);
        if (before == after) {
            if (h == 0) return false;              /* nothing published yet */
            *frames = f;
            *host_time = h;
            return true;
        }
    }
    return false;
}

/* AbsoluteTime is mach ticks split into two 32-bit halves in host order, so the
 * union reassembles the counter exactly -- this is what UnsignedWideToUInt64
 * does. Used for GetBusFrameNumber's timestamp and the frame list's
 * frTimeStamp. */
static uint64_t abs_to_ticks(AbsoluteTime t)
{
    union { AbsoluteTime at; uint64_t ticks; } u = { .at = t };
    return u.ticks;
}

/* --- public surface -------------------------------------------------------- */

void emu_engine_write_output(EmuEngine* e, const float* frames, uint32_t count, uint64_t sample_pos)
{
    if (!e) return;
    /* Core Audio serializes this device's single WriteMix stream on one IO
     * thread, so the low bit is ownership, not a general writer count. */
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &e->bind_gate, &expected, BIND_GATE_WRITING,
            memory_order_acquire, memory_order_relaxed)) {
        return;
    }

    write_output_bind(e, frames, count, sample_pos);
    atomic_fetch_and_explicit(&e->bind_gate, ~BIND_GATE_WRITING, memory_order_release);
}

void emu_engine_set_output_gain(EmuEngine* e, float gain)
{
    if (!e) return;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    atomic_store_explicit(&e->output_gain, gain, memory_order_relaxed);
}

void emu_engine_read_input(EmuEngine* e, float* frames, uint32_t count, uint64_t sample_pos)
{
    if (!e) { memset(frames, 0, (size_t)count * EMU_RING_CHANNELS * sizeof(float)); return; }
    /* e->with_input, not in_intf: this runs on the IO thread and must not follow
     * engine pointers the engine thread owns. Nothing writes the ring in this
     * mode, so reading it would only count underruns. */
    if (atomic_load_explicit(&e->running, memory_order_relaxed) && e->with_input) {
        emu_ring_read_f32(&e->input_ring, sample_pos, frames, count);
    } else {
        memset(frames, 0, (size_t)count * EMU_RING_CHANNELS * sizeof(float));
    }
}

uint64_t emu_engine_frames_played(EmuEngine* e)
{
    if (!e) return 0;
    return atomic_load_explicit(&e->frames_played, memory_order_relaxed);
}

bool emu_engine_running(EmuEngine* e)
{
    if (!e) return false;
    return atomic_load_explicit(&e->running, memory_order_acquire);
}

bool emu_engine_streaming(EmuEngine* e)
{
    if (!e) return false;
    return atomic_load_explicit(&e->streaming, memory_order_acquire);
}

void emu_engine_set_failure_handler(EmuEngine* e, void (*handler)(void))
{
    if (!e) return;
    atomic_store_explicit(&e->failure_handler, handler, memory_order_relaxed);
}

/* The last session's counters, captured after teardown drains the IO writer
 * and before the next start resets the engine. Without this, a post-mortem
 * `make check` after a bad session reads all zeros -- exactly when the
 * counters are wanted most. */

/*
 * Zeroes the counters that only exist to be read.
 *
 * Deliberately leaves frames_played and frames_captured alone: the timeline
 * derives its period from frames_played, so resetting it would send Core Audio's
 * sample time backwards, which it treats as a fault.
 */
void emu_engine_reset_counters(EmuEngine* e)
{
    if (!e) return;
    pthread_mutex_lock(&e->stats_lock);
    atomic_store_explicit(&e->input_ring.missing, 0, memory_order_relaxed);
    atomic_store_explicit(&e->input_ring.discarded, 0, memory_order_relaxed);
    atomic_store_explicit(&e->usb_errors, 0, memory_order_relaxed);
    atomic_store_explicit(&e->ts_fallbacks, 0, memory_order_relaxed);
    /* Recovery history is diagnostics like the rest: without this a second
     * measurement in the same coreaudiod session starts with the first one's
     * faults already counted, and any test asserting a clean baseline can
     * only ever pass once. */
    atomic_store_explicit(&e->recoveries, 0, memory_order_relaxed);
    atomic_store_explicit(&e->recovery_failures, 0, memory_order_relaxed);
    atomic_store_explicit(&e->resyncs, 0, memory_order_relaxed);
    atomic_store_explicit(&e->dead_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&e->unfilled_playback, 0, memory_order_relaxed);
    atomic_store_explicit(&e->empty_capture, 0, memory_order_relaxed);
    atomic_store_explicit(&e->short_playback, 0, memory_order_relaxed);
    /* The extremes and the change count describe a window; the last value and
     * the nominal describe the stream, and survive a reset. */
    atomic_store_explicit(&e->fb_packets, 0, memory_order_relaxed);
    atomic_store_explicit(&e->fb_silent, 0, memory_order_relaxed);
    atomic_store_explicit(&e->fb_errors, 0, memory_order_relaxed);
    atomic_store_explicit(&e->fb_rejected, 0, memory_order_relaxed);
    atomic_store_explicit(&e->fb_changes, 0, memory_order_relaxed);
    atomic_store_explicit(&e->fb_min_q16, 0, memory_order_relaxed);
    atomic_store_explicit(&e->fb_max_q16, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_unmapped, 0, memory_order_relaxed);
    /* Missing these two made `unmappedAhead` outlive its own superset, so a
     * cleared window reported a strict subset larger than the whole -- and
     * `unmappedAhead > 0` is exactly the "schedule too short" signal the
     * diagnostics tell the reader to trust. */
    atomic_store_explicit(&e->bind_unmapped_ahead, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_write_lead, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_races, 0, memory_order_relaxed);
    atomic_store_explicit(&e->bind_missing, 0, memory_order_relaxed);
    /* The filter is engine-thread-owned. Reset the diagnostic window from its
     * atomically published mirror rather than reading the mutable Rust object
     * concurrently. */
    atomic_store_explicit(
        &e->ts_resets_zero,
        atomic_load_explicit(&e->ts_resets, memory_order_relaxed),
        memory_order_relaxed);
    /* Asking for zeros while stopped means the last session's snapshot too,
     * otherwise stats would keep handing back the figures just cleared. */
    e->have_final_stats = false;
    pthread_mutex_unlock(&e->stats_lock);
}

void emu_engine_stats(EmuEngine* e, EmuEngineStats* stats)
{
    if (!e || !stats) return;
    if (!stats) return;
    pthread_mutex_lock(&e->stats_lock);
    if (!atomic_load_explicit(&e->running, memory_order_acquire)) {
        if (e->have_final_stats) {
            *stats = e->final_stats;
        } else {
            memset(stats, 0, sizeof *stats);
        }
        pthread_mutex_unlock(&e->stats_lock);
        return;
    }
    stats->frames_played = emu_engine_frames_played(e);
    stats->usb_errors = atomic_load_explicit(&e->usb_errors, memory_order_relaxed);
    stats->timestamp_fallbacks = atomic_load_explicit(&e->ts_fallbacks, memory_order_relaxed);
    uint64_t ts_resets = atomic_load_explicit(&e->ts_resets, memory_order_relaxed);
    uint64_t ts_resets_zero =
        atomic_load_explicit(&e->ts_resets_zero, memory_order_relaxed);
    stats->timestamp_resets = ts_resets > ts_resets_zero
        ? ts_resets - ts_resets_zero : 0;
    stats->resyncs = atomic_load_explicit(&e->resyncs, memory_order_relaxed);
    stats->dead_frames = atomic_load_explicit(&e->dead_frames, memory_order_relaxed);
    stats->unfilled_playback = atomic_load_explicit(&e->unfilled_playback, memory_order_relaxed);
    stats->short_playback = atomic_load_explicit(&e->short_playback, memory_order_relaxed);
    stats->feedback_starved =
        atomic_load_explicit(&e->feedback_starved, memory_order_relaxed);
    stats->feedback_overflows =
        atomic_load_explicit(&e->feedback_overflows, memory_order_relaxed);

    stats->feedback_packets  = atomic_load_explicit(&e->fb_packets, memory_order_relaxed);
    stats->feedback_silent   = atomic_load_explicit(&e->fb_silent, memory_order_relaxed);
    stats->feedback_errors   = atomic_load_explicit(&e->fb_errors, memory_order_relaxed);
    stats->feedback_rejected = atomic_load_explicit(&e->fb_rejected, memory_order_relaxed);
    stats->feedback_changes  = atomic_load_explicit(&e->fb_changes, memory_order_relaxed);
    stats->feedback_q16      = atomic_load_explicit(&e->fb_value_q16, memory_order_relaxed);
    stats->feedback_min_q16  = atomic_load_explicit(&e->fb_min_q16, memory_order_relaxed);
    stats->feedback_max_q16  = atomic_load_explicit(&e->fb_max_q16, memory_order_relaxed);
    stats->feedback_nominal_q16 = e->fb_nominal_q16;

    /* How far Core Audio's writes lead the play head, and how many frames
     * transmitted before it got to them: the two questions the output path
     * has to answer, asked of the frontier the IO thread publishes. */
    uint64_t frontier = atomic_load_explicit(&e->bind_frontier, memory_order_acquire);
    stats->output_lead     = frontier > stats->frames_played
                           ? (uint32_t)(frontier - stats->frames_played) : 0;
    stats->underruns       = atomic_load_explicit(&e->bind_missing, memory_order_relaxed);
    stats->frames_bound    = atomic_load_explicit(&e->bind_frames, memory_order_relaxed);
    stats->unmapped_frames = atomic_load_explicit(&e->bind_unmapped, memory_order_relaxed);
    stats->unmapped_ahead  = atomic_load_explicit(&e->bind_unmapped_ahead, memory_order_relaxed);
    stats->write_lead_max  = atomic_load_explicit(&e->bind_write_lead, memory_order_relaxed);
    stats->bind_races      = atomic_load_explicit(&e->bind_races, memory_order_relaxed);
    stats->schedule_requests = e->num_requests;
    stats->schedule_clamped  = e->schedule_clamped;

    stats->input_enabled   = e->with_input;
    stats->frames_captured = atomic_load_explicit(&e->frames_captured, memory_order_relaxed);
    stats->input_depth     = emu_ring_depth(&e->input_ring);
    stats->input_underruns = atomic_load_explicit(&e->input_ring.missing, memory_order_relaxed);
    stats->input_overruns  = atomic_load_explicit(&e->input_ring.discarded, memory_order_relaxed);
    stats->empty_capture   = atomic_load_explicit(&e->empty_capture, memory_order_relaxed);
    pthread_mutex_unlock(&e->stats_lock);
    stats->engine_streaming   = emu_engine_streaming(e) ? 1u : 0u;
    stats->recoveries         = atomic_load_explicit(&e->recoveries, memory_order_relaxed);
    stats->recovery_failures  = atomic_load_explicit(&e->recovery_failures, memory_order_relaxed);
    stats->fault_mode         = atomic_load_explicit(&e->fault_mode, memory_order_relaxed);
}

/* --- transfers ------------------------------------------------------------- */

static void capture_complete(void* refcon, IOReturn result, void* arg0);
static void playback_complete(void* refcon, IOReturn result, void* arg0);
static void reschedule(Engine* e);

static uint64_t frames_in_ms(const Engine* e, uint64_t ms)
{
    return (ms * e->sample_rate + 500) / 1000;
}

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

    if (fault_should_fail(e)) return kIOReturnNotResponding;

    IOReturn result = (*e->in_intf)->LowLatencyReadIsochPipeAsync(
        e->in_intf, e->in_pipe, req->buffer, req->frame_start,
        e->entries_per_request, 1, req->frames, capture_complete, req);

    /* Not on the bus, so not on the schedule: the frames stay unclaimed, for
     * the retry or for reschedule() to measure the gap from. */
    if (result != kIOReturnSuccess) e->next_in_frame = req->frame_start;
    return result;
}

static void feedback_complete(void* refcon, IOReturn result, void* arg0);

/* Queues one request on the explicit feedback endpoint. Four bytes per entry,
 * on the endpoint's own bInterval. */
static IOReturn submit_feedback(Request* req)
{
    Engine* e = req->engine;

    for (uint32_t i = 0; i < e->fb_entries_per_request; i++) {
        req->frames[i].frStatus   = kUSBLowLatencyIsochTransferKey;
        req->frames[i].frReqCount = e->fb_max;
        req->frames[i].frActCount = 0;
    }

    req->frame_start = e->next_fb_frame;
    e->next_fb_frame += REQUEST_MS;

    return (*e->out_intf)->LowLatencyReadIsochPipeAsync(
        e->out_intf, e->fb_pipe, req->buffer, req->frame_start,
        e->fb_entries_per_request, 1, req->frames, feedback_complete, req);
}

/*
 * What the device asks for, as opposed to what capture says it took.
 *
 * Little-endian Q16.16 sample frames per playback service interval -- the
 * reading E-MU's Windows driver uses, confirmed against this hardware at every
 * rate: 0x00600000 at 192 kHz on a 0.5 ms endpoint is 96.0000 frames, not the
 * 192 frames per millisecond the same vendor's macOS kext would have read out
 * of the identical four bytes.
 *
 * An out-of-band value is refused rather than clamped. During the stream that
 * follows a bInterval 3 stream -- the documented poisoning -- this endpoint
 * returns nonsense along with everything else, and a planner that trusted it
 * would size real packets from it.
 */
static void feedback_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    Request* req = (Request*)refcon;
    Engine* e = req->engine;

    if (result == kIOReturnIsoTooOld) reschedule(e);

    for (uint32_t i = 0; i < e->fb_entries_per_request; i++) {
        const IOUSBLowLatencyIsocFrame* f = &req->frames[i];

        if (!emu_frame_ok(f->frStatus)) {
            if (!atomic_load_explicit(&e->stopping, memory_order_relaxed))
                atomic_fetch_add_explicit(&e->fb_errors, 1, memory_order_relaxed);
            continue;
        }
        /* Silence from an asynchronous feedback endpoint is normal: this one
         * speaks about thirty times a second and says nothing in between.
         * Counted apart from errors so "quiet" and "dead" stay distinct. */
        if (f->frActCount != 4) {
            atomic_fetch_add_explicit(&e->fb_silent, 1, memory_order_relaxed);
            continue;
        }

        /* Entries are laid out by frReqCount, as everywhere else. */
        const uint8_t* p = (const uint8_t*)req->buffer + (size_t)i * e->fb_max;
        uint32_t q16 = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);

        uint32_t lo = e->fb_nominal_q16 > 65536u ? e->fb_nominal_q16 - 65536u : 0;
        uint32_t hi = e->fb_nominal_q16 + 65536u;
        if (q16 < lo || q16 > hi) {
            atomic_fetch_add_explicit(&e->fb_rejected, 1, memory_order_relaxed);
            continue;
        }

        uint32_t prev = atomic_exchange_explicit(&e->fb_value_q16, q16,
                                                 memory_order_relaxed);
        uint64_t seen = atomic_fetch_add_explicit(&e->fb_packets, 1,
                                                  memory_order_relaxed);
        if (seen == 0) {
            atomic_store_explicit(&e->fb_min_q16, q16, memory_order_relaxed);
            atomic_store_explicit(&e->fb_max_q16, q16, memory_order_relaxed);
        } else {
            if (q16 != prev)
                atomic_fetch_add_explicit(&e->fb_changes, 1, memory_order_relaxed);
            if (q16 < atomic_load_explicit(&e->fb_min_q16, memory_order_relaxed))
                atomic_store_explicit(&e->fb_min_q16, q16, memory_order_relaxed);
            if (q16 > atomic_load_explicit(&e->fb_max_q16, memory_order_relaxed))
                atomic_store_explicit(&e->fb_max_q16, q16, memory_order_relaxed);
        }
    }

    if (atomic_load_explicit(&e->stopping, memory_order_relaxed)) return;

    if (submit_feedback(req) != kIOReturnSuccess) {
        reschedule(e);
        /* A feedback pipe that will not restart costs diagnostics, not audio:
         * the planner falls back to capture on the next submit because no new
         * value arrives, and the data pipes are untouched. */
        if (submit_feedback(req) != kIOReturnSuccess) e->fb_pipe = 0;
    }
}

/*
 * How many frames the next playback entry carries.
 *
 * Capture's measurement while capture is open: the frame count of each
 * capture packet sizes the next playback packet, so the two directions are
 * locked to the same clock by construction. Without capture, the device's own
 * demand from the explicit feedback endpoint, accumulated in Q16.16 so a
 * fractional request comes out as the right mixture of whole packets rather
 * than a truncation -- the same arithmetic E-MU's Windows driver does with its
 * running fraction -- and the nominal until the first value arrives.
 */
static uint32_t planner_next(Engine* e)
{
    if (e->in_intf) return emu_feedback_next(e->feedback, e->nominal_frames);

    uint32_t q16 = atomic_load_explicit(&e->fb_value_q16, memory_order_relaxed);
    if (q16 == 0) return e->nominal_frames;

    /* Corrected before it is used, never where it is reported: the raw word
     * is what the diagnostics show. Uncorrected, the 44.1 family would be
     * under-delivered by 53.1 ppm -- 9.4 frames a second at 176.4 kHz. */
    q16 = emu_feedback_true_q16(q16);

    uint32_t acc = e->fb_residue_q16 + q16;
    e->fb_residue_q16 = acc & 0xffffu;
    return acc >> 16;
}

/* Fixes the frame list -- packet count and sizes -- and queues the request.
 * The audio bytes are NOT read here: the buffer goes out prefilled and the fill
 * binds the data later, closer to transmission. Sizes have to be decided this
 * early because the frame list is part of the submission; that only delays
 * the feedback servo's response by the in-flight window, which a rate servo
 * does not mind. */
static IOReturn submit_playback(Request* req)
{
    Engine* e = req->engine;
    size_t offset = 0;
    size_t idx = (size_t)(req - e->out_requests);

    /* From here until this request is back on the bus its buffer belongs to
     * nobody: the IO thread must not be writing the old range's audio into
     * bytes about to be zeroed for a new one. */
    bind_retire(e, idx);

    req->data_frame_start = e->out_cursor;

    for (uint32_t i = 0; i < e->entries_per_request; i++) {
        uint32_t frames = planner_next(e);
        uint32_t bytes = emu_output_packet_bytes(frames, e->bytes_per_frame);
        while (bytes > e->out_max && frames > 0) {
            frames--;
            bytes = emu_output_packet_bytes(frames, e->bytes_per_frame);
        }

        /* Contiguous: entry i begins where entry i-1's frReqCount ended. */
        req->entry_frames[i] = frames;
        e->out_cursor += frames;
        offset += bytes;

        req->frames[i].frStatus   = kUSBLowLatencyIsochTransferKey;
        req->frames[i].frReqCount = (UInt16)bytes;
        req->frames[i].frActCount = 0;
    }
    atomic_store_explicit(&e->feedback_starved,
                          emu_feedback_starved(e->feedback),
                          memory_order_relaxed);
    /* The Rust queue has counted these since it was written and nothing has
     * ever read them: overflow means capture and playback have decoupled,
     * which is not a state that should be inferred from its symptoms. */
    atomic_store_explicit(&e->feedback_overflows,
                          emu_feedback_overflows(e->feedback),
                          memory_order_relaxed);
    req->data_frame_end = e->out_cursor;

    /* What goes out until the fill lands. Also the audible verdict on late
     * binding itself: a stack that copied the buffer at submit would play
     * exactly this, and nothing but this. */
    memset(req->buffer, 0, offset);
    req->generation = e->generation;

    req->frame_start = e->next_out_frame;
    e->next_out_frame += REQUEST_MS;

    IOReturn result = fault_should_fail(e)
        ? kIOReturnNotResponding
        : (*e->out_intf)->LowLatencyWriteIsochPipeAsync(
        e->out_intf, e->out_pipe, req->buffer, req->frame_start,
        e->entries_per_request, 1, req->frames, playback_complete, req);

    /* A submission that never reached the bus must not keep its slice of the
     * timeline. No bus interval was scheduled for these frames, so this is
     * the one case the completion side will never count (see the note above
     * the completions); left allocated, they would shear the fill cursor
     * ahead of the write timeline by a request per failed submit, for good.
     * The bus frames are given back too, so a rebuild measures its gap from
     * what was actually scheduled. The feedback pops are not restored; the
     * retry draws fresh ones, and a rate servo does not miss a couple of
     * measurements. */
    if (result != kIOReturnSuccess) {
        e->out_cursor = req->data_frame_start;
        e->next_out_frame = req->frame_start;
        return result;
    }

    /* On the bus, buffer zeroed, slice of the timeline known: everything a
     * writer needs. Published only now, so a failed submission leaves the slot
     * retired rather than advertising a request that never went out. */
    bind_publish(e, idx, (uint8_t*)req->buffer,
                 req->data_frame_start, req->data_frame_end);
    return kIOReturnSuccess;
}

/*
 * Accounts for what the IO thread did not get to, at the one moment it stops
 * being able to: the request examined is the one starting to transmit now.
 *
 * Its frames past the IO thread's frontier are frames going out as their
 * submit-time zeros -- silence sent because Core Audio was late, which is the
 * only way this path can glitch. Counted in frames (`outputUnderruns`) and in
 * requests (`unfilledPlayback`).
 *
 * Nothing counts before Core Audio's first cycle: the schedule legitimately
 * runs ahead of it at every stream start, and those packets are silence by
 * design.
 */
static void bind_audit(Engine* e, size_t next)
{
    const Request* req = &e->out_requests[next % e->num_requests];
    uint64_t frontier = atomic_load_explicit(&e->bind_frontier, memory_order_acquire);
    if (frontier == 0 || req->data_frame_end <= frontier) return;

    uint64_t base = req->data_frame_start > frontier ? req->data_frame_start : frontier;
    atomic_fetch_add_explicit(&e->bind_missing, req->data_frame_end - base,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&e->unfilled_playback, 1, memory_order_relaxed);
}

/*
 * Rebuilds the bus schedule after a stall outlasted the in-flight window.
 *
 * The device's clock ran through the stall. The bus frames between the end
 * of what was scheduled and the start of what will be were bus time like any
 * other, merely with no packets in them; the cursors keep bus time, so each
 * direction's cursor skips exactly that many frames, and the timeline Core
 * Audio sees is the same straight line as before with a hole in the audio
 * where the dropout was. That is what any DMA engine looks like when it
 * underruns, and it is what the HAL's clock model is built for: the seed
 * never changes, and the IO thread never has to resynchronise. (Pausing the
 * sample clock instead needs a new seed, and the HAL answers a seed change
 * by freezing its IO thread -- a second glitch; see FINDINGS.)
 *
 * Idempotent: once a direction's next frame is ahead of the bus again a later
 * call finds nothing to rebuild. The completions of a stall drain as a burst
 * and every one of them may arrive here. Per direction, because each keeps
 * its own bus time -- normally both went stale together and both are
 * rebuilt into the same frames, as at start.
 *
 * The timestamp filter is rebased, not reset: it keeps the rate it has
 * learned and only moves its prediction to the new schedule's first
 * completion. Completions still draining from the old schedule are muted by
 * generation.
 */
static void reschedule(Engine* e)
{
    UInt64 now = 0;
    AbsoluteTime at;
    /* Through the playback interface: it is the one that is always open, and
     * the frame number belongs to the bus rather than to either of them. */
    if ((*e->out_intf)->GetBusFrameNumber(e->out_intf, &now, &at) != kIOReturnSuccess) {
        return;
    }
    uint64_t start = now + SCHEDULE_LEAD_MS;
    bool rebuilt = false;

    /* The feedback pipe carries no audio and holds no cursor, so a stale
     * schedule on it is only a gap in the diagnostics -- rebased with the
     * rest, but never counted as a rebuild. */
    if (e->next_fb_frame < start) e->next_fb_frame = start;

    if (e->next_out_frame < start) {
        uint64_t gap = frames_in_ms(e, start - e->next_out_frame);
        /* Nothing to discard on the output side: the IO thread's writes for
         * the dead interval find no request covering them and are counted as
         * unmapped, which is the accounting this dead interval needs. */
        e->out_cursor += gap;
        atomic_fetch_add_explicit(&e->frames_played, gap, memory_order_relaxed);
        atomic_fetch_add_explicit(&e->dead_frames, gap, memory_order_relaxed);
        e->next_out_frame = start;
        emu_ts_filter_rebase(e->ts_filter,
                             abs_to_ticks(at) + (SCHEDULE_LEAD_MS + REQUEST_MS) * e->ticks_per_ms);
        atomic_store_explicit(&e->ts_resets, emu_ts_filter_resets(e->ts_filter),
                              memory_order_relaxed);
        e->generation++;
        rebuilt = true;
    }
    /* Without capture next_in_frame is never advanced by anything, so it goes
     * stale immediately and would report a rebuild on every call. */
    if (e->in_intf && e->next_in_frame < start) {
        uint64_t gap = frames_in_ms(e, start - e->next_in_frame);
        emu_ring_write_silence(&e->input_ring, e->in_cursor, (uint32_t)gap);
        e->in_cursor += gap;
        atomic_fetch_add_explicit(&e->frames_captured, gap, memory_order_relaxed);
        e->next_in_frame = start;
        rebuilt = true;
    }
    if (rebuilt) atomic_fetch_add_explicit(&e->resyncs, 1, memory_order_relaxed);
}

/*
 * The cursors keep bus time, not delivery.
 *
 * out_cursor, frames_played and in_cursor all count the same thing: sample
 * frames per bus interval, from the stream's first packet. Core Audio's
 * sample time is anchored to frames_played, Core Audio writes the packets at
 * out_cursor, capture writes the input ring at in_cursor, and the phase
 * between them -- the safety offsets -- holds only while all three advance by
 * the same amount for the same interval. The device does its part
 * unconditionally: its clock runs through an interval whether the packet in
 * it was delivered, errored, arrived empty, or was never scheduled at all.
 * So the completions count every interval too -- the planned frames for a
 * playback entry the bus did not carry, a nominal packet of silence for a
 * capture entry that brought nothing -- and reschedule() counts the
 * intervals no request covered. The lost audio is silence of exactly the
 * lost length, which is what a timeline-indexed transport says a glitch
 * should be.
 * Skipping any interval instead moves one cursor and not the others, and
 * that offset never heals: every skipped packet takes its size out of the
 * safety margin for the rest of the stream, until the output is starving with
 * every other counter clean. The one interval nobody counts is one that was
 * never on the bus at all -- a failed submit -- and submit_playback gives
 * that slice back for the same reason.
 */

static void capture_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    Request* req = (Request*)refcon;
    Engine* e = req->engine;

    if (result == kIOReturnIsoTooOld) reschedule(e);

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
        uint32_t frames = 0;
        if (emu_frame_ok(f->frStatus)) {
            frames = emu_frames_in_packet(f->frActCount, e->bytes_per_frame);
        } else if (!atomic_load_explicit(&e->stopping, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&e->usb_errors, 1, memory_order_relaxed);
        }

        if (frames > 0) {
            /* Capture is the clock reference, so this happens whether or not
             * anyone is recording. */
            emu_feedback_push(e->feedback, frames);

            /* At bInterval 3 every packet leads with 4 bytes that are not
             * sample frames: the packet's own byte length, which E-MU's
             * Windows driver reads and steps over (FINDINGS). Taking the
             * frames from the first byte instead puts every sample two thirds
             * of a frame early and scrambles all of them.
             *
             * The offset is taken from the packet rather than the rate, so it
             * is zero wherever the byte count already divides, which is every
             * rate up to 96 kHz. The read stays inside the packet either way:
             * lead + frames x bytes_per_frame is frActCount exactly. */
            uint32_t lead = f->frActCount % e->bytes_per_frame;
            emu_ring_write_s24(&e->input_ring, e->in_cursor,
                               (const uint8_t*)req->buffer + offset + lead, frames);
        } else {
            /* Nothing usable for this interval: an error, or one of the empty
             * packets the device sends while its ADC spins up (a couple at
             * every start in the captured traces). The interval passed on the
             * device's clock all the same, so the input keeps its place with
             * a nominal packet of silence; skipped, every later capture
             * packet would sit one packet early on the timeline for good. No
             * feedback push: the queue holds measurements and there was none.
             * One pop draws the nominal instead, which is within a frame of
             * what the measurement would have said. */
            frames = e->nominal_frames;
            emu_ring_write_silence(&e->input_ring, e->in_cursor, frames);
            if (!atomic_load_explicit(&e->stopping, memory_order_relaxed))
                atomic_fetch_add_explicit(&e->empty_capture, 1, memory_order_relaxed);
        }
        e->in_cursor += frames;
        atomic_fetch_add_explicit(&e->frames_captured, frames, memory_order_relaxed);

        offset += f->frReqCount;
    }

    if (atomic_load_explicit(&e->stopping, memory_order_relaxed)) {
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    IOReturn kr = submit_capture(req);
    if (kr != kIOReturnSuccess) {
        ENG_DEBUG("capture submit failed 0x%08x, rescheduling", kr);
        reschedule(e);
        kr = submit_capture(req);
        if (kr != kIOReturnSuccess) {
            /* A reschedule did not help, so the transport itself is gone.
             * Leave the run loop and let the thread rebuild the stream; do NOT
             * set stopping, which would exit the thread and strand Core Audio
             * streaming into nothing. */
            ENG_ERR("capture submit failed after reschedule 0x%08x, rebuilding stream", kr);
            atomic_store_explicit(&e->faulted, true, memory_order_relaxed);
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
    }
}

static void playback_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    Request* req = (Request*)refcon;
    Engine* e = req->engine;

    if (result == kIOReturnIsoTooOld) reschedule(e);

    /* The last completed entry's hardware timestamp: the host controller
     * stamps each low-latency frame list entry as it finishes, which is far
     * steadier than callback timing -- the callback adds this thread's
     * scheduling jitter, the controller does not. */
    uint64_t stamp = 0;

    for (uint32_t i = 0; i < e->entries_per_request; i++) {
        const IOUSBLowLatencyIsocFrame* f = &req->frames[i];
        if (emu_frame_ok(f->frStatus)) {
            stamp = abs_to_ticks(f->frTimeStamp);
            /* Good status is not delivery. On an OUT entry frActCount short of
             * frReqCount is audio the bus did not carry, and the frames are
             * credited below regardless -- correctly, the device's clock ran
             * through the interval either way -- so nothing else in the driver
             * would ever notice. Counted separately from frStatus errors so
             * each says one thing: usbErrors is a transfer that failed,
             * shortPlayback is one that succeeded and moved fewer bytes. */
            if (f->frActCount != f->frReqCount &&
                !atomic_load_explicit(&e->stopping, memory_order_relaxed)) {
                atomic_fetch_add_explicit(&e->short_playback, 1, memory_order_relaxed);
            }
        } else if (!atomic_load_explicit(&e->stopping, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&e->usb_errors, 1, memory_order_relaxed);
        }
        /* By the planned size, not frActCount, and for errored entries too:
         * out_cursor took exactly these frames at submit, and the interval
         * they were scheduled into has elapsed on the device's clock whatever
         * the bus did with them. Counting only what the bus carried would
         * leave the fill cursor ahead of the timeline for the rest of the
         * stream after any errored entry. Should the last entry be the one that
         * errored, the stamp below is from an earlier one -- a sub-request
         * skew on a rare event, which the filter absorbs as jitter. */
        atomic_fetch_add_explicit(&e->frames_played, req->entry_frames[i],
                                  memory_order_relaxed);
    }

    if (!atomic_load_explicit(&e->stopping, memory_order_relaxed)) {
        /* Audited before anything else in this callback: the request after
         * this one is starting to transmit right now, so this is the last
         * moment its unwritten frames can still be counted. */
        size_t idx = (size_t)(req - e->out_requests);
        bind_audit(e, idx + 1);

        /* Published once per request rather than per entry: the cadence the
         * timestamp filter expects, and Core Audio only samples the anchor
         * every ZeroTimeStampPeriod frames anyway. Fall back to the callback
         * clock -- counted, so a stack that stops filling frTimeStamp cannot
         * silently degrade -- and let the filter absorb either source's jitter.
         *
         * Only for requests of the current schedule: after a rebuild, the
         * filter has been rebased to the new schedule and completions still
         * draining from the old one would pull it back across the gap. They
         * keep their frames and their resubmission; the clock ignores them. */
        if (req->generation == e->generation) {
            uint64_t now = mach_absolute_time();
            uint64_t skew = stamp > now ? stamp - now : now - stamp;
            if (stamp == 0 || skew > e->ticks_per_ms * 1000) {
                stamp = now;
                atomic_fetch_add_explicit(&e->ts_fallbacks, 1, memory_order_relaxed);
            }
            uint64_t filtered = emu_ts_filter_apply(e->ts_filter, stamp);
            atomic_store_explicit(&e->ts_resets, emu_ts_filter_resets(e->ts_filter),
                                  memory_order_relaxed);
            timeline_publish(e, 
                atomic_load_explicit(&e->frames_played, memory_order_relaxed), filtered);
        }

        IOReturn kr = submit_playback(req);
        if (kr != kIOReturnSuccess) {
            ENG_DEBUG("playback submit failed 0x%08x, rescheduling", kr);
            reschedule(e);
            kr = submit_playback(req);
            if (kr != kIOReturnSuccess) {
                ENG_ERR("playback submit failed after reschedule 0x%08x, rebuilding stream", kr);
                atomic_store_explicit(&e->faulted, true, memory_order_relaxed);
                CFRunLoopStop(CFRunLoopGetCurrent());
            }
        }
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

/*
 * Drop the handles teardown has already released, and nothing else.
 *
 * A rebuild needs the Engine's resource fields empty so setup starts clean and
 * teardown cannot double-free, while every counter and the timeline position
 * carry across untouched.
 */
static void clear_resources(Engine* e)
{
    e->identity = NULL;
    e->device   = NULL;
    e->service  = 0;
    e->in_intf  = NULL;
    e->out_intf = NULL;
    e->run_loop = NULL;
    e->in_pipe = e->out_pipe = 0;
    e->fb_pipe = 0;
    for (int i = 0; i < MAX_REQUESTS; i++) {
        e->in_requests[i].buffer  = NULL; e->in_requests[i].frames  = NULL;
        e->out_requests[i].buffer = NULL; e->out_requests[i].frames = NULL;
    }
    for (int i = 0; i < FB_REQUESTS; i++) {
        e->fb_requests[i].buffer = NULL; e->fb_requests[i].frames = NULL;
    }
}

static void teardown(Engine* e)
{
    /* The direct path hands the IO thread pointers into buffers this function
     * frees, so a writer can be inside one right now. Closing the shared gate
     * atomically excludes a new writer and records one already admitted;
     * retire every slot, then wait for that bounded real-time routine to
     * leave before destroying anything. There is deliberately no unsafe
     * timeout: once admission is closed a healthy writer cannot block, and
     * freeing beneath an unhealthy one would turn a stuck daemon into memory
     * corruption. */
    atomic_fetch_or_explicit(&e->bind_gate, BIND_GATE_CLOSED, memory_order_acq_rel);
    for (int i = 0; i < MAX_REQUESTS; i++) bind_retire(e, (size_t)i);
    while ((atomic_load_explicit(&e->bind_gate, memory_order_acquire) &
            BIND_GATE_WRITING) != 0) {
        usleep(1000);
    }

    for (int i = 0; i < MAX_REQUESTS; i++) {
        if (e->in_requests[i].buffer && e->in_intf)
            (*e->in_intf)->LowLatencyDestroyBuffer(e->in_intf, e->in_requests[i].buffer);
        if (e->in_requests[i].frames && e->in_intf)
            (*e->in_intf)->LowLatencyDestroyBuffer(e->in_intf, e->in_requests[i].frames);
        if (e->out_requests[i].buffer && e->out_intf)
            (*e->out_intf)->LowLatencyDestroyBuffer(e->out_intf, e->out_requests[i].buffer);
        if (e->out_requests[i].frames && e->out_intf)
            (*e->out_intf)->LowLatencyDestroyBuffer(e->out_intf, e->out_requests[i].frames);
    }
    for (int i = 0; i < FB_REQUESTS; i++) {
        if (e->fb_requests[i].buffer && e->out_intf)
            (*e->out_intf)->LowLatencyDestroyBuffer(e->out_intf, e->fb_requests[i].buffer);
        if (e->fb_requests[i].frames && e->out_intf)
            (*e->out_intf)->LowLatencyDestroyBuffer(e->out_intf, e->fb_requests[i].frames);
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
}

/*
 * With only num_requests x REQUEST_MS in flight, an engine thread that loses
 * the CPU for a few milliseconds drops audio, so it declares its cadence to
 * the scheduler. Failure is survivable -- the thread still runs, just without
 * the guarantee -- which is why the result is not checked.
 *
 * One arrival per REQUEST_MS, and it carries *both* directions: capture and
 * playback requests are submitted into identical bus frames, so their
 * completions land together and this one budget covers the pair.
 *
 * The numbers are not the obvious ones, because thread_policy.h forces
 * computation up to constraint/2 whenever it is declared smaller:
 *
 *   - constraint is 1 ms, half the completion period. Declaring the full
 *     2 ms would drag computation up to 1 ms with it and reserve half a core
 *     every period; 1 ms holds the reservation at 500 us while asking for
 *     more urgency than the cadence strictly needs.
 *   - computation is therefore written as the 500 us the clamp produces, not
 *     as a smaller figure the kernel would silently discard. It is far more
 *     than the work -- a completion is two IOKit submits, the frames-played
 *     accounting and one clock re-anchor, with no audio conversion on this
 *     thread at all -- but it is the floor this constraint implies, so it is
 *     what the declaration should say.
 *   - preemptible is documented as IGNORED; it is assigned only so the whole
 *     struct is initialised.
 *
 * None of this covers the hop before the arrival. A thread policy bounds when
 * a runnable thread gets the CPU, not how promptly the USB stack delivers the
 * completion that wakes it, so a stall longer than the in-flight window is not
 * something widening these values can fix.
 */
static void set_engine_thread_policy(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ticks_per_ns = (double)tb.denom / (double)tb.numer;

    thread_time_constraint_policy_data_t policy;
    policy.period      = (uint32_t)(REQUEST_MS * 1000000.0 * ticks_per_ns);
    policy.computation = (uint32_t)(500000.0 * ticks_per_ns);
    policy.constraint  = (uint32_t)(1000000.0 * ticks_per_ns);
    policy.preemptible = TRUE;

    /* mach_thread_self() hands out a send right per call, so it is dropped
     * again here; leaking one per stream start would accumulate in the host
     * daemon across a session's worth of StartIO. */
    thread_port_t thread = mach_thread_self();
    thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), thread);
}

/* Back to timeshare before the slow half of teardown. Aborting the pipes,
 * draining the last completions and unwiring the low-latency buffers is
 * multi-millisecond kernel work with no deadline behind it, and real-time
 * priority is the wrong band to do it in. */
static void clear_engine_thread_policy(void)
{
    thread_extended_policy_data_t policy;
    policy.timeshare = TRUE;

    thread_port_t thread = mach_thread_self();
    thread_policy_set(thread, THREAD_EXTENDED_POLICY,
                      (thread_policy_t)&policy, THREAD_EXTENDED_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), thread);
}

static void signal_start_state(Engine* e, EngineStartState state)
{
    pthread_mutex_lock(&e->start_lock);
    e->start_state = state;
    pthread_cond_broadcast(&e->start_cond);
    pthread_mutex_unlock(&e->start_lock);
}

/*
 * How deep the schedule has to be for the direct bind, in requests.
 *
 * Core Audio does not write at the safety offset; it writes at
 *
 *     writeLead ~ 2 x bufferFrames + safetyOffset + ~208 frames
 *
 * (the HAL computes a cycle's output time one buffer period ahead of
 * presentation and then hands over a buffer-length range, so the far end
 * lands at offset + 2 x buffer; FINDINGS has the sweep this was fitted to).
 * A frame no submitted request covers has nowhere to go on this path, so the
 * schedule must reach past that -- for the *largest* buffer the HAL will
 * grant, not the one in use, because a client may change it mid-stream and
 * the schedule cannot be rebuilt underneath it.
 *
 * Derived rather than tuned, so raising the safety offset, changing the
 * zero-timestamp period or running at a rate with fewer frames per request
 * moves it on its own instead of silently overrunning a hard-coded depth.
 */
static uint32_t schedule_depth(Engine* e, uint32_t safety_us)
{
    uint32_t per_request = e->nominal_frames * e->entries_per_request;
    if (per_request == 0) { e->schedule_clamped = true; return MAX_REQUESTS; }

    /* The ceiling, not `safety_us`: the write lead follows the offset
     * coreaudiod cached, which a runtime 'emuS' change does not touch. See
     * EMU_OUTPUT_SAFETY_MAX_US. `safety_us` only ever raises it. */
    if (safety_us < EMU_OUTPUT_SAFETY_MAX_US) safety_us = EMU_OUTPUT_SAFETY_MAX_US;
    uint64_t offset_frames = (uint64_t)e->sample_rate * safety_us / 1000000u;
    uint64_t lead = 2ull * HAL_MAX_IO_BUFFER + offset_frames + WRITE_LEAD_SLACK;
    uint64_t need = (lead + per_request - 1) / per_request + 4;  /* +4 margin */

    if (need < MIN_REQUESTS) need = MIN_REQUESTS;
    /* Clamping here means the schedule may not reach the write lead, which
     * shows up only as `unmappedAhead` climbing -- so it is reported rather
     * than absorbed. Unreachable as the constants stand; that is the point. */
    if (need > MAX_REQUESTS) { need = MAX_REQUESTS; e->schedule_clamped = true; }
    return (uint32_t)need;
}

/* One streaming session: bring the device up, run it, take it down again.
 * Returns what ended it, so the thread above can tell a stop from a fault. */
typedef enum { kSessionSetupFailed = -1, kSessionStopped = 0, kSessionFaulted = 1 } SessionEnd;

static SessionEnd stream_session(Engine* e, bool announced)
{
    emu_ring_reset(&e->input_ring);

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    e->ticks_per_ms = (uint64_t)(1000000.0 * (double)tb.denom / (double)tb.numer);
    e->sample_rate = e->requested_rate;
    bind_reset(e);

#define ENGINE_FAIL() do { teardown(e); return kSessionSetupFailed; } while (0)

    e->feedback = emu_feedback_init(e->feedback_storage);
    if (!e->feedback) ENGINE_FAIL();

    if (!open_device(e)) ENGINE_FAIL();

    IOUSBConfigurationDescriptorPtr cfg = NULL;
    EmuDeviceModel model;
    if ((*e->device)->GetConfigurationDescriptorPtr(e->device, 0, &cfg) != kIOReturnSuccess ||
        emu_parse_config_descriptor((const uint8_t*)cfg,
                                    OSSwapLittleToHostInt16(cfg->wTotalLength), &model) != 0) {
        ENGINE_FAIL();
    }

    if (!set_clock_rate(e, &model, e->requested_rate)) ENGINE_FAIL();

    if (!emu_find_interface(e->device, 1, &e->out_intf)) ENGINE_FAIL();
    if (e->with_input && !emu_find_interface(e->device, 2, &e->in_intf)) ENGINE_FAIL();

    if ((*e->out_intf)->USBInterfaceOpen(e->out_intf) != kIOReturnSuccess) {
        ENGINE_FAIL();
    }
    if (e->in_intf && (*e->in_intf)->USBInterfaceOpen(e->in_intf) != kIOReturnSuccess) {
        ENGINE_FAIL();
    }

    /* Interface 2 stays at alternate setting 0 when it is not opened: no
     * bandwidth reserved for it, and no IN transaction on the bus. */
    const EmuAltSetting *out_alt = NULL, *in_alt = NULL;
    if (!select_alt(e->out_intf, &model, 1, e->requested_rate, &out_alt)) ENGINE_FAIL();
    if (e->in_intf && !select_alt(e->in_intf, &model, 2, e->requested_rate, &in_alt)) {
        ENGINE_FAIL();
    }

    if (!emu_find_isoc_pipe(e->out_intf, kUSBOut, &e->out_pipe, &e->out_max)) {
        ENGINE_FAIL();
    }
    if (e->in_intf &&
        !emu_find_isoc_pipe(e->in_intf, kUSBIn, &e->in_pipe, &e->in_max)) {
        ENGINE_FAIL();
    }
    /* The only isochronous IN pipe on the playback interface is the explicit
     * feedback endpoint. Its absence is not a failure: the transport does not
     * depend on it, so a device without one simply reports nothing. */
    if (!emu_find_isoc_pipe_full(e->out_intf, kUSBIn, &e->fb_pipe,
                                 &e->fb_max, &e->fb_interval)) {
        e->fb_pipe = 0;
    }

    e->bytes_per_frame = out_alt->channels * out_alt->subframe_size;
    if (e->bytes_per_frame == 0) e->bytes_per_frame = 6;
    /* The direct path's whole map, besides the per-request ranges: a
     * request's byte layout is linear in this. */
    atomic_store_explicit(&e->bind_bpf, e->bytes_per_frame, memory_order_relaxed);

    uint32_t period = 1u << (out_alt->interval - 1);
    e->entries_per_ms = period >= 8 ? 1 : (8 / period);
    e->entries_per_request = REQUEST_MS * e->entries_per_ms;
    if (e->entries_per_request > MAX_ENTRIES) e->entries_per_request = MAX_ENTRIES;

    double interval_ms = (double)period * 0.125;
    e->nominal_frames = (uint32_t)(e->requested_rate * interval_ms / 1000.0);
    /* Exact, in Q16.16: at 176.4 kHz a 0.5 ms interval holds 88.2 frames, and
     * a feedback value has to be judged against that rather than against the
     * 88 the truncation above leaves. */
    e->fb_nominal_q16 = (uint32_t)(((uint64_t)e->requested_rate << 16) * period / 8000u);
    if (e->fb_pipe) {
        uint32_t fb_period = 1u << (e->fb_interval - 1);
        uint32_t fb_per_ms = fb_period >= 8 ? 1 : (8 / fb_period);
        e->fb_entries_per_request = REQUEST_MS * fb_per_ms;
        if (e->fb_entries_per_request > MAX_ENTRIES)
            e->fb_entries_per_request = MAX_ENTRIES;
        e->fb_num_requests = FB_REQUESTS;
    }
    /* Now that frames-per-request is known. Before any buffer is allocated. */
    e->num_requests = schedule_depth(e, e->safety_us);
    emu_feedback_set_nominal(e->feedback, e->requested_rate, (uint64_t)(interval_ms * 1e6));

    CFRunLoopSourceRef in_source = NULL, out_source = NULL;
    if ((*e->out_intf)->CreateInterfaceAsyncEventSource(e->out_intf, &out_source) != kIOReturnSuccess) {
        ENGINE_FAIL();
    }
    if (e->in_intf &&
        (*e->in_intf)->CreateInterfaceAsyncEventSource(e->in_intf, &in_source) != kIOReturnSuccess) {
        ENGINE_FAIL();
    }
    e->run_loop = CFRunLoopGetCurrent();
    if (in_source) CFRunLoopAddSource(e->run_loop, in_source, kCFRunLoopDefaultMode);
    CFRunLoopAddSource(e->run_loop, out_source, kCFRunLoopDefaultMode);

    UInt32 list_bytes = e->entries_per_request * sizeof(IOUSBLowLatencyIsocFrame);
    for (uint32_t i = 0; i < e->num_requests; i++) {
        e->in_requests[i].engine = e;
        e->out_requests[i].engine = e;
        if ((*e->out_intf)->LowLatencyCreateBuffer(e->out_intf, &e->out_requests[i].buffer,
                (UInt32)e->entries_per_request * e->out_max, kUSBLowLatencyWriteBuffer) != kIOReturnSuccess ||
            (*e->out_intf)->LowLatencyCreateBuffer(e->out_intf, (void**)&e->out_requests[i].frames,
                list_bytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess) {
            ENGINE_FAIL();
        }
        if (e->in_intf &&
            ((*e->in_intf)->LowLatencyCreateBuffer(e->in_intf, &e->in_requests[i].buffer,
                (UInt32)e->entries_per_request * e->in_max, kUSBLowLatencyReadBuffer) != kIOReturnSuccess ||
             (*e->in_intf)->LowLatencyCreateBuffer(e->in_intf, (void**)&e->in_requests[i].frames,
                list_bytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess)) {
            ENGINE_FAIL();
        }
    }
    for (uint32_t i = 0; i < e->fb_num_requests; i++) {
        UInt32 fb_list_bytes =
            e->fb_entries_per_request * sizeof(IOUSBLowLatencyIsocFrame);
        e->fb_requests[i].engine = e;
        if ((*e->out_intf)->LowLatencyCreateBuffer(e->out_intf, &e->fb_requests[i].buffer,
                (UInt32)e->fb_entries_per_request * e->fb_max,
                kUSBLowLatencyReadBuffer) != kIOReturnSuccess ||
            (*e->out_intf)->LowLatencyCreateBuffer(e->out_intf, (void**)&e->fb_requests[i].frames,
                fb_list_bytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess) {
            /* Diagnostics, not audio: give the endpoint up rather than the
             * stream. Whatever was allocated is freed by teardown. */
            e->fb_num_requests = i;
            e->fb_pipe = 0;
            break;
        }
    }

    UInt64 now = 0;
    AbsoluteTime at;
    if ((*e->out_intf)->GetBusFrameNumber(e->out_intf, &now, &at) != kIOReturnSuccess) {
        ENGINE_FAIL();
    }
    e->next_in_frame = now + SCHEDULE_LEAD_MS;
    e->next_out_frame = now + SCHEDULE_LEAD_MS;
    e->next_fb_frame = now + SCHEDULE_LEAD_MS;

    /* The timeline starts here, and it starts *known*: sample 0 is the first
     * frame of the first packet, scheduled SCHEDULE_LEAD_MS bus frames ahead
     * of the (frame number, host time) pair the controller just gave us.
     * Anchoring from the schedule instead of waiting for the first completion
     * means Core Audio's very first GetZeroTimeStamp is already on the
     * device's timeline -- there is no host-clock placeholder to splice away
     * from later, and a splice stalls the IO thread (FINDINGS). */
    uint64_t start_host = abs_to_ticks(at) + SCHEDULE_LEAD_MS * e->ticks_per_ms;
    e->ts_filter = emu_ts_filter_init(e->ts_filter_storage, start_host,
                                      REQUEST_MS * e->ticks_per_ms);
    if (!e->ts_filter) ENGINE_FAIL();

    /*
     * Resume on the timeline rather than restarting it.
     *
     * On a first start session_reset has already put frames_played at zero, so
     * this is the original "sample 0 is the first packet". On a *rebuild* it is
     * the frame the device had reached before the fault, and it has to be:
     * Core Audio's sample time follows frames_played and does not restart mid
     * session, so publishing zero here would leave it writing near the start of
     * the timeline while the fresh requests carry frames from far along it --
     * no request covering anything written, every frame dropped as unmapped,
     * and silence with the whole transport reporting healthy.
     *
     * The cursors are pinned to the same value for the same reason: out_cursor
     * survives teardown, and left alone it would resume an in-flight window
     * ahead of where the anchor says the device is.
     */
    uint64_t resume = atomic_load_explicit(&e->frames_played, memory_order_relaxed);
    e->out_cursor = resume;
    e->in_cursor  = resume;
    timeline_publish(e, resume, start_host);

#undef ENGINE_FAIL

    set_engine_thread_policy();

    /* Capture first, so playback has measurements waiting instead of starving
     * through its whole first request. The first playback request is bound
     * by its submit; the rest are bound by the completion sweep as Core Audio
     * starts writing -- until then they are silence either way. */
    bool submitted = true;
    for (uint32_t i = 0; i < e->num_requests && submitted && e->in_intf; i++) {
        submitted = submit_capture(&e->in_requests[i]) == kIOReturnSuccess;
    }
    for (uint32_t i = 0; i < e->num_requests && submitted; i++) {
        submitted = submit_playback(&e->out_requests[i]) == kIOReturnSuccess;
    }
    if (!submitted) {
        teardown(e);
        return kSessionSetupFailed;
    }
    /* Last, and never fatal. Reading the device's stated demand is a
     * measurement placed alongside the stream, not a part of it. */
    for (uint32_t i = 0; i < e->fb_num_requests && e->fb_pipe; i++) {
        if (submit_feedback(&e->fb_requests[i]) != kIOReturnSuccess) {
            e->fb_pipe = 0;
        }
    }

    atomic_store_explicit(&e->bind_count, e->num_requests, memory_order_release);

    /* The map describes the whole queue now, so writers may use it. Opening
     * the gate publishes every preceding map write before StartIO returns. */
    atomic_store_explicit(&e->bind_gate, 0, memory_order_release);

    atomic_store_explicit(&e->running, true, memory_order_release);
    atomic_store_explicit(&e->streaming, true, memory_order_release);
    /* The start handshake is answered once, by the first session. A rebuild
     * must not signal it again: StartIO has long since returned. */
    if (!announced) signal_start_state(e, ENGINE_STREAMING);

    while (!atomic_load_explicit(&e->stopping, memory_order_relaxed) &&
           !atomic_load_explicit(&e->faulted, memory_order_relaxed)) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    }
    atomic_store_explicit(&e->streaming, false, memory_order_release);

    clear_engine_thread_policy();

    (*e->out_intf)->AbortPipe(e->out_intf, e->out_pipe);
    if (e->in_intf) (*e->in_intf)->AbortPipe(e->in_intf, e->in_pipe);
    if (e->fb_pipe) (*e->out_intf)->AbortPipe(e->out_intf, e->fb_pipe);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);

    if (in_source) {
        CFRunLoopRemoveSource(e->run_loop, in_source, kCFRunLoopDefaultMode);
        CFRelease(in_source);
    }
    CFRunLoopRemoveSource(e->run_loop, out_source, kCFRunLoopDefaultMode);
    CFRelease(out_source);

    /* Closing the gate drains the last IO callback, so take the post-mortem
     * only after teardown: every callback-local diagnostic has landed and no
     * live counter can still change. Engine storage is reset at the next
     * start, under e->stats_lock, rather than while diagnostics may read it. */
    teardown(e);
    return atomic_load_explicit(&e->faulted, memory_order_relaxed)
         ? kSessionFaulted : kSessionStopped;
}

/*
 * The engine thread: run a session, and rebuild it if the transport failed.
 *
 * Previously two consecutive failed submissions set `stopping`, the thread ran
 * teardown and exited, and nothing on the system knew: Core Audio went on
 * calling DoIOOperation, the write path went on dropping frames because the
 * bind gate was closed, and the only way back was restarting coreaudiod. A
 * failed submission is now a fault, faults are rebuilt through, and only an
 * exhausted retry budget ends the session -- telling the plug-in, rather than
 * going quiet.
 */
/*
 * Start a session's timeline from zero.
 *
 * Core Audio restarts its own sample time at every StartIO, so the engine has
 * to restart with it: leave out_cursor where the last session ended and the
 * two sides address different frames entirely -- Core Audio writes near zero,
 * the requests cover somewhere past nine million, no request covers anything
 * written, and every frame is dropped as unmapped. That is silence with the
 * whole transport reporting healthy.
 *
 * A *rebuild* must never come here. There the timeline is mid-flight and
 * agreed between both sides, and restarting it would send Core Audio's sample
 * time backwards, which it treats as a fault.
 *
 * This replaces a memset of the whole Engine, which cannot be used any more:
 * the locks now live in the struct and are initialised once in create(), and
 * emu_engine_start writes the configuration before this thread runs. Zeroing
 * either would be worse than the bug it fixes, so the fields are listed.
 */
static void session_reset(Engine* e)
{
    atomic_store_explicit(&e->frames_played, 0, memory_order_relaxed);
    atomic_store_explicit(&e->frames_captured, 0, memory_order_relaxed);
    e->out_cursor = 0;
    e->in_cursor  = 0;
    e->generation = 0;

    atomic_store_explicit(&e->timeline_seq, 0, memory_order_relaxed);
    atomic_store_explicit(&e->timeline_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&e->timeline_host, 0, memory_order_relaxed);

    bind_reset(e);
    emu_engine_reset_counters(e);
}

static void* engine_thread(void* arg)
{
    Engine* e = (Engine*)arg;

    /* Once, here rather than per session: a rebuild must keep frames_played --
     * Core Audio derives sample time from it and treats a backwards step as a
     * fault -- and every counter with it. */
    /* Once per session, before the rebuild loop: the timeline starts at zero
     * with Core Audio's, and every rebuild below keeps it. */
    session_reset(e);
    atomic_store_explicit(&e->stopping, false, memory_order_relaxed);
    atomic_store_explicit(&e->faulted, false, memory_order_relaxed);

    bool announced = false;
    unsigned attempt = 0;

    for (;;) {
        atomic_store_explicit(&e->faulted, false, memory_order_relaxed);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        SessionEnd end = stream_session(e, announced);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ran_for = (double)(t1.tv_sec - t0.tv_sec)
                       + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

        if (end == kSessionStopped) break;

        /*
         * The budget counts *consecutive* failed rebuilds, so a session that
         * genuinely recovered and played gives it back. Without this the
         * allowance is cumulative over the engine's whole life: six faults
         * recovered cleanly across an afternoon, and the seventh -- equally
         * recoverable -- would take the device offline instead.
         *
         * Gated on having run for a while rather than merely on having reached
         * the streaming state, so a fault that lets setup succeed and then
         * fails immediately cannot refresh the budget forever and retry
         * without end.
         */
        if (end == kSessionFaulted && ran_for >= RECOVERY_STABLE_SECONDS) {
            attempt = 0;
        }

        if (end == kSessionSetupFailed && !announced) {
            /* A first start that never came up fails fast rather than
             * retrying: StartIO is blocked on this handshake, and a device
             * that is not there when Core Audio asks is better reported than
             * waited for. */
            ENG_ERR("stream did not come up at %u Hz", e->requested_rate);
            signal_start_state(e, ENGINE_FAILED);
            break;
        }
        announced = true;

        if (atomic_load_explicit(&e->stopping, memory_order_relaxed)) break;
        if (end == kSessionSetupFailed) {
            atomic_fetch_add_explicit(&e->recovery_failures, 1, memory_order_relaxed);
        }

        if (++attempt > MAX_RECOVERY_ATTEMPTS) {
            ENG_ERR("giving up after %u failed rebuilds; marking the device not alive",
                    MAX_RECOVERY_ATTEMPTS);
            break;
        }

        unsigned delay_ms = RECOVERY_BACKOFF_MS(attempt - 1);
        ENG_ERR("transport fault, rebuilding in %u ms (attempt %u of %u)",
                delay_ms, attempt, MAX_RECOVERY_ATTEMPTS);

        /* teardown has already released the hardware, so a device that needs
         * to re-enumerate is not held open while we wait. Only the resource
         * handles are cleared; every counter carries across, because a fault
         * that resets the evidence is a fault nobody can diagnose after it. */
        clear_resources(e);

        /* Slept in slices rather than in one go: StopIO joins this thread, so
         * a single 1.6 s wait would be 1.6 s of coreaudiod blocked on a device
         * the user has already switched away from. */
        for (unsigned waited = 0; waited < delay_ms; waited += 25) {
            if (atomic_load_explicit(&e->stopping, memory_order_relaxed)) break;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 25 * 1000000L };
            nanosleep(&ts, NULL);
        }

        if (atomic_load_explicit(&e->stopping, memory_order_relaxed)) break;
        if (end != kSessionSetupFailed) {
            atomic_fetch_add_explicit(&e->recoveries, 1, memory_order_relaxed);
            ENG_LOG("rebuilding at frame %llu",
                    (unsigned long long)atomic_load_explicit(&e->frames_played,
                                                             memory_order_relaxed));
        }
    }

    atomic_store_explicit(&e->streaming, false, memory_order_release);
    emu_engine_stats(e, &e->final_stats);
    pthread_mutex_lock(&e->stats_lock);
    e->have_final_stats = true;
    pthread_mutex_unlock(&e->stats_lock);

    bool gave_up = announced &&
                   !atomic_load_explicit(&e->stopping, memory_order_relaxed);
    atomic_store_explicit(&e->running, false, memory_order_release);

    /* Last, and outside everything: the handler re-enters the plug-in, which
     * must not find the engine half torn down. */
    if (gave_up) {
        void (*handler)(void) =
            atomic_load_explicit(&e->failure_handler, memory_order_relaxed);
        if (handler) handler();
    }
    return NULL;
}

bool emu_engine_start(EmuEngine* e, uint32_t sample_rate, uint32_t output_safety_us,
                      bool with_input)
{
    if (!e) return false;
    pthread_mutex_lock(&e->lifecycle_lock);
    if (atomic_load_explicit(&e->running, memory_order_acquire)) {
        pthread_mutex_unlock(&e->lifecycle_lock);
        return true;
    }
    /* A transport may have stopped itself after a runtime USB failure. Reap
     * that finished session before reusing its singleton Engine storage. */
    if (e->thread_joinable) {
        pthread_join(e->thread, NULL);
        e->thread_joinable = false;
    }
    e->requested_rate = sample_rate;
    e->safety_us = output_safety_us;
    e->with_input = with_input;
    atomic_store_explicit(&e->timeline_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&e->timeline_host, 0, memory_order_relaxed);

    pthread_mutex_lock(&e->start_lock);
    e->start_state = ENGINE_STARTING;
    pthread_mutex_unlock(&e->start_lock);

    if (pthread_create(&e->thread, NULL, engine_thread, e) != 0) {
        pthread_mutex_lock(&e->start_lock);
        e->start_state = ENGINE_IDLE;
        pthread_mutex_unlock(&e->start_lock);
        pthread_mutex_unlock(&e->lifecycle_lock);
        return false;
    }
    e->thread_joinable = true;

    /* Wait for the streams to be on the bus. Normal bring-up is tens of
     * milliseconds; the bound exists so a wedged control transfer surfaces as
     * a failed start rather than a hung coreaudiod. StartIO is not on the IO
     * path, so blocking here is the sanctioned way to start slowly -- and it
     * is what guarantees the timeline anchor exists before the first
     * GetZeroTimeStamp. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 8;

    pthread_mutex_lock(&e->start_lock);
    while (e->start_state == ENGINE_STARTING) {
        if (pthread_cond_timedwait(&e->start_cond, &e->start_lock, &deadline) == ETIMEDOUT) break;
    }
    bool ok = (e->start_state == ENGINE_STREAMING);
    pthread_mutex_unlock(&e->start_lock);

    if (!ok) {
        atomic_store_explicit(&e->stopping, true, memory_order_relaxed);
        pthread_join(e->thread, NULL);
        e->thread_joinable = false;
        pthread_mutex_lock(&e->start_lock);
        e->start_state = ENGINE_IDLE;
        pthread_mutex_unlock(&e->start_lock);
        pthread_mutex_unlock(&e->lifecycle_lock);
        return false;
    }

    pthread_mutex_unlock(&e->lifecycle_lock);
    return true;
}

void emu_engine_stop(EmuEngine* e)
{
    if (!e) return;
    pthread_mutex_lock(&e->lifecycle_lock);
    if (!e->thread_joinable) {
        pthread_mutex_unlock(&e->lifecycle_lock);
        return;
    }
    bool was_running = atomic_load_explicit(&e->running, memory_order_acquire);
    atomic_store_explicit(&e->stopping, true, memory_order_relaxed);
    if (was_running && e->run_loop) CFRunLoopStop(e->run_loop);
    pthread_join(e->thread, NULL);
    e->thread_joinable = false;
    atomic_store_explicit(&e->running, false, memory_order_release);
    pthread_mutex_lock(&e->start_lock);
    e->start_state = ENGINE_IDLE;
    pthread_mutex_unlock(&e->start_lock);
    pthread_mutex_unlock(&e->lifecycle_lock);
}
