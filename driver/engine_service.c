/*
 * The engine as its own process, for the optional two-part build
 * (docs/SPLIT-DRIVER-PLAN.md). Owns USB; serves the HAL over the boundary in
 * engine_shared.h. Unprivileged: it refuses to run as root, and launchd starts
 * it as the developer.
 *
 *     emu-engine <service>                       serve
 *     emu-engine ctl status|shutdown [<service>] ask a running engine
 *
 * It drives the transport through usb_engine.h and nothing else, so the same
 * file serves the real USB engine and the tests' fake one.
 *
 * Threads:
 *
 *   control queue   the listener, the unit table, who the owner is.
 *   one queue per   open/start/stop/close for that unit, in order. A start
 *   unit            blocks for as long as the device takes -- up to seconds --
 *                   and must not hold up the other unit or a status request.
 *   one pump per    forwards the HAL's playback writes into the transport and
 *   running unit    republishes its clock. The only real-time thread here
 *                   besides the transport's own.
 *
 * The control queue never waits on a unit queue. Unit queues reach the owner
 * connection through a lock, not through the control queue.
 */

#include <dispatch/dispatch.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <os/lock.h>
#include <os/log.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include "engine_shared.h"
#include "../shared/usb_util.h"

/* Who may be the HAL. macOS loads an AudioServerPlugIn into one of two Apple
 * hosts; require Apple's anchor and one of those exact identities, not a
 * process name. (From the generic driver template's peer policy.) */
#define HAL_PEER_REQUIREMENT \
    "anchor apple and (identifier \"com.apple.audio.coreaudiod\" or " \
    "identifier \"com.apple.audio.Core-Audio-Driver-Service.helper\")"

#define MAX_UNITS 4u
#define BUILD_STRING EMU_VERSION " (git " EMU_GITREV ")"

enum { ROLE_CONTROL = 1, ROLE_HAL = 2 };

typedef struct Unit {
    /* control queue */
    bool     in_use;
    bool     present;
    uint16_t product;
    uint64_t location;
    char     serial[80];
    dispatch_queue_t queue;

    /* the unit's own queue */
    EmuEngine* engine;
    EmuShared* shm;
    size_t     bytes;
    bool       running;
    uint64_t   generation;
    pthread_t  pump;
    _Atomic bool pump_stop;
} Unit;

static Unit             gUnits[MAX_UNITS];
static dispatch_queue_t gControl;
static bool             gQuitting;                      /* control queue */
static os_unfair_lock   gOwnerLock = OS_UNFAIR_LOCK_INIT;
static xpc_connection_t gOwner;                          /* gOwnerLock */
static double           gTicksPerNs;

static os_log_t service_log(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create(EMU_LOG_SUBSYSTEM, "service"); });
    return log;
}
#define SLOG(fmt, ...) do { os_log(service_log(), fmt, ##__VA_ARGS__); } while (0)

static uint64_t ticks_to_ns(uint64_t ticks) { return (uint64_t)((double)ticks / gTicksPerNs); }

/* --- the owner --------------------------------------------------------------- */

static xpc_connection_t owner_retain(void)
{
    os_unfair_lock_lock(&gOwnerLock);
    xpc_connection_t c = gOwner ? xpc_retain(gOwner) : NULL;
    os_unfair_lock_unlock(&gOwnerLock);
    return c;
}

static bool is_owner(xpc_connection_t peer)
{
    os_unfair_lock_lock(&gOwnerLock);
    bool yes = peer == gOwner;
    os_unfair_lock_unlock(&gOwnerLock);
    return yes;
}

static void send_event(xpc_object_t event)
{
    xpc_connection_t owner = owner_retain();
    if (owner) {
        xpc_connection_send_message(owner, event);
        xpc_release(owner);
    }
    xpc_release(event);
}

/* --- the pump ------------------------------------------------------------------ */

static void pump_policy(void)
{
    /* One arrival per pump interval, a few microseconds of work. The same
     * shape as the transport thread's policy and for the same reason: an
     * ordinary thread's scheduling hiccups would be added to every frame. */
    thread_time_constraint_policy_data_t policy;
    policy.period      = (uint32_t)(EMU_PUMP_INTERVAL_US * 1000.0 * gTicksPerNs);
    policy.computation = (uint32_t)(EMU_PUMP_INTERVAL_US * 250.0 * gTicksPerNs);
    policy.constraint  = (uint32_t)(EMU_PUMP_INTERVAL_US * 500.0 * gTicksPerNs);
    policy.preemptible = TRUE;
    thread_port_t thread = mach_thread_self();
    thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), thread);
}

static void note_max(_Atomic uint64_t* cell, uint64_t value)
{
    if (value > atomic_load_explicit(cell, memory_order_relaxed))
        atomic_store_explicit(cell, value, memory_order_relaxed);
}

/* The HAL's volume, into the transport. A new engine starts at unity, and the
 * first frames forwarded must already carry the level the HAL holds, so this
 * runs before anything is bound -- at start, and ahead of the records in every
 * pass. */
static void apply_gain(EmuEngine* e, EmuShared* s, uint32_t* last, bool* known)
{
    uint32_t bits = atomic_load_explicit(&s->gain_bits, memory_order_relaxed);
    if (*known && bits == *last) return;
    float gain;
    memcpy(&gain, &bits, sizeof gain);
    emu_engine_set_output_gain(e, gain == gain ? gain : 0.0f);   /* NaN: silence */
    *last = bits;
    *known = true;
}

/* Plays Core Audio's part towards the transport: each record is one WriteMix,
 * replayed in order with the HAL's own sample time, so everything the bind
 * path does about gaps, rewrites and late frames is what it does one-part. */
static void* pump_thread(void* arg)
{
    Unit* u = (Unit*)arg;
    EmuShared* s = u->shm;
    EmuEngine* e = u->engine;
    pump_policy();

    uint64_t interval = (uint64_t)(EMU_PUMP_INTERVAL_US * 1000.0 * gTicksPerNs);
    uint64_t next = mach_absolute_time() + interval;
    uint32_t gain_bits = 0;
    bool     gain_known = false;
    uint64_t last_frames = UINT64_MAX, last_host = UINT64_MAX;

    while (!atomic_load_explicit(&u->pump_stop, memory_order_relaxed)) {
        uint64_t now = mach_absolute_time();
        if (now > next) note_max(&s->pump.wake_late_max_ns, ticks_to_ns(now - next));

        uint64_t depth = atomic_load_explicit(&s->q_head, memory_order_acquire)
                       - atomic_load_explicit(&s->q_tail, memory_order_relaxed);
        if (depth <= EMU_WRITE_QUEUE &&
            depth > atomic_load_explicit(&s->pump.queue_depth_max, memory_order_relaxed)) {
            atomic_store_explicit(&s->pump.queue_depth_max, (uint32_t)depth, memory_order_relaxed);
        }

        apply_gain(e, s, &gain_bits, &gain_known);

        EmuWriteRecord r;
        while (emu_shared_peek(s, &r)) {
            uint64_t at = mach_absolute_time();
            uint64_t delay = at > r.host ? ticks_to_ns(at - r.host) : 0;
            note_max(&s->pump.copy_delay_max_ns, delay);
            unsigned bucket = 0;
            for (uint64_t edge = 125000; bucket < EMU_PUMP_DELAY_BUCKETS - 1 && delay >= edge; edge *= 2)
                bucket++;
            atomic_fetch_add_explicit(&s->pump.copy_delay[bucket], 1, memory_order_relaxed);

            /* The frames are this record's until emu_shared_done: the HAL
             * does not write where they are, however far ahead it gets. */
            uint32_t slot  = (uint32_t)(r.at & EMU_RING_MASK);
            uint32_t first = EMU_RING_FRAMES - slot;
            if (first > r.count) first = r.count;
            emu_engine_write_output(e, &s->play[(size_t)slot * EMU_RING_CHANNELS], first, r.pos);
            if (first < r.count)
                emu_engine_write_output(e, &s->play[0], r.count - first, r.pos + first);
            emu_shared_done(s, &r);

            atomic_fetch_add_explicit(&s->pump.records, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&s->pump.frames, r.count, memory_order_relaxed);
        }

        uint64_t frames = 0, host = 0;
        if (emu_engine_timeline(e, &frames, &host) && (frames != last_frames || host != last_host)) {
            emu_shared_publish_timeline(s, frames, host);
            last_frames = frames;
            last_host = host;
        }

        atomic_store_explicit(&s->heartbeat, mach_absolute_time(), memory_order_relaxed);
        atomic_fetch_add_explicit(&s->pump.passes, 1, memory_order_relaxed);

        next += interval;
        now = mach_absolute_time();
        if (next <= now) next = now + interval;      /* fell behind: no burst to catch up */
        mach_wait_until(next);
    }
    return NULL;
}

/* --- one unit, on its own queue ------------------------------------------------- */

static void unit_publish_stats(Unit* u)
{
    if (!u->shm || !u->engine) return;
    EmuEngineStats stats;
    memset(&stats, 0, sizeof stats);
    emu_engine_stats(u->engine, &stats);
    emu_shared_publish_stats(u->shm, &stats);
}

static void unit_stop(Unit* u)
{
    if (!u->running) return;
    atomic_store(&u->pump_stop, true);
    pthread_join(u->pump, NULL);
    emu_engine_stop(u->engine);
    atomic_store_explicit(&u->shm->engine_state, EMU_SHARED_IDLE, memory_order_release);
    u->running = false;
    unit_publish_stats(u);              /* the session's final figures */
    SLOG("%04x@%08llx stopped", u->product, (unsigned long long)u->location);
}

static void unit_detach(Unit* u)
{
    unit_stop(u);
    if (!u->shm) return;
    emu_engine_set_input_ring(u->engine, NULL);
    munmap(u->shm, u->bytes);
    u->shm = NULL;
    u->bytes = 0;
}

static const char* unit_open(Unit* u, xpc_object_t memory)
{
    if (!memory || xpc_get_type(memory) != XPC_TYPE_SHMEM) return "no shared memory";
    void* p = NULL;
    size_t bytes = xpc_shmem_map(memory, &p);
    if (!bytes || !p) return "shared memory would not map";
    if (!emu_shared_valid((const EmuShared*)p, bytes)) {
        munmap(p, bytes);
        return "shared region is not this ABI";
    }
    /* The HAL touched and wired its mapping of these pages; this is another
     * mapping of them, and its first touch of each would otherwise be a fault
     * taken on the pump or the transport thread, mid-stream. */
    volatile unsigned char sink = 0;
    for (size_t at = 0; at < bytes; at += (size_t)getpagesize()) sink += ((const unsigned char*)p)[at];
    (void)sink;
    (void)mlock(p, bytes);
    unit_detach(u);                     /* the same owner replacing its region */
    if (!emu_engine_set_input_ring(u->engine, &((EmuShared*)p)->capture)) {
        munmap(p, bytes);
        return "transport busy";
    }
    u->shm = (EmuShared*)p;
    u->bytes = bytes;
    atomic_store_explicit(&u->shm->engine_state, EMU_SHARED_IDLE, memory_order_release);
    unit_publish_stats(u);
    return NULL;
}

static bool rate_supported(uint64_t rate)
{
    static const uint32_t kRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
    for (unsigned i = 0; i < sizeof kRates / sizeof kRates[0]; i++)
        if (rate == kRates[i]) return true;
    return false;
}

static const char* unit_start(Unit* u, uint64_t generation, uint64_t rate, uint64_t safety_us,
                              bool with_input)
{
    if (!u->shm) return "not open";
    if (!rate_supported(rate) || safety_us > EMU_OUTPUT_SAFETY_MAX_US) return "bad parameters";
    if (u->running && u->generation == generation) return NULL;
    /* The HAL names the session it is starting; anything else is a message
     * from a session that has since been replaced. */
    if (atomic_load_explicit(&u->shm->generation, memory_order_acquire) != generation)
        return "stale generation";
    unit_stop(u);

    EmuShared* s = u->shm;
    /* Records queued for an earlier session are not this one's audio. */
    emu_shared_discard(s);
    memset(&s->pump, 0, sizeof s->pump);
    atomic_store_explicit(&s->engine_generation, generation, memory_order_relaxed);

    uint32_t gain_bits = 0;
    bool gain_known = false;
    apply_gain(u->engine, s, &gain_bits, &gain_known);

    if (!emu_engine_start(u->engine, (uint32_t)rate, (uint32_t)safety_us, with_input))
        return "device did not start";

    uint64_t frames = 0, host = 0;
    if (!emu_engine_timeline(u->engine, &frames, &host)) {
        emu_engine_stop(u->engine);
        return "no timeline anchor";
    }
    /* Before the reply, so the HAL's first GetZeroTimeStamp has the device's
     * clock, as it does one-part. */
    emu_shared_publish_timeline(s, frames, host);
    atomic_store_explicit(&s->heartbeat, mach_absolute_time(), memory_order_relaxed);
    atomic_store_explicit(&s->engine_state, EMU_SHARED_STREAMING, memory_order_release);

    atomic_store(&u->pump_stop, false);
    if (pthread_create(&u->pump, NULL, pump_thread, u) != 0) {
        emu_engine_stop(u->engine);
        atomic_store_explicit(&s->engine_state, EMU_SHARED_IDLE, memory_order_release);
        return "no pump thread";
    }
    u->running = true;
    u->generation = generation;
    unit_publish_stats(u);
    SLOG("%04x@%08llx started: %llu Hz, safety %llu us, input %d, generation %llu", u->product,
         (unsigned long long)u->location, (unsigned long long)rate, (unsigned long long)safety_us,
         (int)with_input, (unsigned long long)generation);
    return NULL;
}

/* The transport's own thread, once, when it has given up on the device. It
 * must not be joined from itself, so the work moves to the unit's queue. */
static void unit_failed(void* context)
{
    Unit* u = (Unit*)context;
    dispatch_async(u->queue, ^{
        if (!u->running) return;
        uint64_t generation = u->generation;
        unit_stop(u);
        xpc_object_t event = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(event, "event", "failed");
        xpc_dictionary_set_uint64(event, "product", u->product);
        xpc_dictionary_set_uint64(event, "location", u->location);
        xpc_dictionary_set_uint64(event, "generation", generation);
        send_event(event);
    });
}

static void describe_unit(Unit* u, xpc_object_t d)
{
    xpc_dictionary_set_bool(d, "open", u->shm != NULL);
    xpc_dictionary_set_bool(d, "running", u->running);
    xpc_dictionary_set_uint64(d, "generation", u->generation);
    if (!u->shm) return;
    EmuShared* s = u->shm;
    EmuPumpStats* p = &s->pump;
#define PUT(name, value) xpc_dictionary_set_uint64(d, name, (uint64_t)(value))
    PUT("pumpIntervalUs", EMU_PUMP_INTERVAL_US);
    PUT("pumpPasses", atomic_load(&p->passes));
    PUT("pumpRecords", atomic_load(&p->records));
    PUT("pumpFrames", atomic_load(&p->frames));
    PUT("pumpInvalid", atomic_load(&p->invalid));
    PUT("pumpResyncs", atomic_load(&p->resyncs));
    PUT("queueDepthMax", atomic_load(&p->queue_depth_max));
    PUT("wakeLateMaxNs", atomic_load(&p->wake_late_max_ns));
    PUT("copyDelayMaxNs", atomic_load(&p->copy_delay_max_ns));
    PUT("halDropped", atomic_load(&s->hal_dropped));
    PUT("halDroppedFrames", atomic_load(&s->hal_dropped_frames));
    PUT("halOversize", atomic_load(&s->hal_oversize));
#undef PUT
    xpc_object_t buckets = xpc_array_create(NULL, 0);
    for (unsigned i = 0; i < EMU_PUMP_DELAY_BUCKETS; i++)
        xpc_array_set_uint64(buckets, XPC_ARRAY_APPEND, atomic_load(&p->copy_delay[i]));
    xpc_dictionary_set_value(d, "copyDelayBuckets", buckets);
    xpc_release(buckets);

    EmuEngineStats stats;
    memset(&stats, 0, sizeof stats);
    emu_engine_stats(u->engine, &stats);
    xpc_dictionary_set_uint64(d, "framesPlayed", stats.frames_played);
    xpc_dictionary_set_uint64(d, "framesBound", stats.frames_bound);
    xpc_dictionary_set_uint64(d, "outputUnderruns", stats.underruns);
    xpc_dictionary_set_uint64(d, "unfilledPlayback", stats.unfilled_playback);
    xpc_dictionary_set_uint64(d, "bindRaces", stats.bind_races);
    xpc_dictionary_set_uint64(d, "writeLeadMax", stats.write_lead_max);
    xpc_dictionary_set_uint64(d, "inputUnderruns", stats.input_underruns);
    xpc_dictionary_set_uint64(d, "engineStreaming", stats.engine_streaming);
}

/* --- the unit table, on the control queue ---------------------------------------- */

static xpc_object_t units_array(void)
{
    xpc_object_t list = xpc_array_create(NULL, 0);
    for (unsigned i = 0; i < MAX_UNITS; i++) {
        Unit* u = &gUnits[i];
        if (!u->in_use || !u->present) continue;
        xpc_object_t d = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_uint64(d, "product", u->product);
        xpc_dictionary_set_uint64(d, "location", u->location);
        xpc_dictionary_set_string(d, "serial", u->serial);
        xpc_array_append_value(list, d);
        xpc_release(d);
    }
    return list;
}

static Unit* find_unit(uint64_t product, uint64_t location)
{
    for (unsigned i = 0; i < MAX_UNITS; i++) {
        Unit* u = &gUnits[i];
        if (u->in_use && u->present && u->product == product && u->location == location) return u;
    }
    return NULL;
}

static void retire_unit(Unit* u)
{
    u->present = false;
    dispatch_async(u->queue, ^{
        unit_detach(u);
        emu_engine_destroy(u->engine);
        u->engine = NULL;
        u->generation = 0;
        dispatch_async(gControl, ^{ u->in_use = false; });
    });
}

static void refresh_units(void)
{
    EmuUnit found[MAX_UNITS];
    unsigned n = emu_enumerate_units(found, MAX_UNITS);
    bool changed = false;

    for (unsigned i = 0; i < MAX_UNITS; i++) {
        Unit* u = &gUnits[i];
        if (!u->in_use || !u->present) continue;
        bool still = false;
        for (unsigned k = 0; k < n; k++) {
            if (found[k].location_id == u->location &&
                found[k].identity->product_id == u->product) still = true;
        }
        if (!still) {
            SLOG("%04x@%08llx detached", u->product, (unsigned long long)u->location);
            retire_unit(u);
            changed = true;
        }
    }
    for (unsigned k = 0; k < n; k++) {
        if (find_unit(found[k].identity->product_id, found[k].location_id)) continue;
        for (unsigned i = 0; i < MAX_UNITS; i++) {
            Unit* u = &gUnits[i];
            if (u->in_use) continue;
            u->in_use = u->present = true;
            u->product = found[k].identity->product_id;
            u->location = found[k].location_id;
            strlcpy(u->serial, found[k].serial, sizeof u->serial);
            if (!u->queue) u->queue = dispatch_queue_create("net.batarelo.EMUTrackerPre.unit",
                                                            DISPATCH_QUEUE_SERIAL);
            EmuEngine* engine = emu_engine_create(u->product, u->location);
            emu_engine_set_failure_handler(engine, unit_failed, u);
            dispatch_sync(u->queue, ^{ u->engine = engine; });   /* the queue is idle: see retire_unit */
            SLOG("%04x@%08llx attached", u->product, (unsigned long long)u->location);
            changed = true;
            break;
        }
    }
    if (!changed) return;
    xpc_object_t event = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(event, "event", "units");
    xpc_object_t list = units_array();
    xpc_dictionary_set_value(event, "units", list);
    xpc_release(list);
    send_event(event);
}

static void units_changed(void)
{
    dispatch_async(gControl, ^{ refresh_units(); });
}

/* Stops every session and calls `then` on the control queue when all of them
 * have confirmed -- which, for USB, means the transfers have been returned. */
static void stop_everything(bool detach, dispatch_block_t then)
{
    dispatch_group_t group = dispatch_group_create();
    for (unsigned i = 0; i < MAX_UNITS; i++) {
        Unit* u = &gUnits[i];
        if (!u->in_use) continue;
        dispatch_group_async(group, u->queue, ^{
            if (detach) unit_detach(u); else unit_stop(u);
        });
    }
    dispatch_group_notify(group, gControl, then);
    dispatch_release(group);
}

/* --- messages, on the control queue ----------------------------------------------- */

static void reply_and_release(xpc_connection_t peer, xpc_object_t reply, const char* error)
{
    xpc_dictionary_set_bool(reply, "ok", error == NULL);
    if (error) xpc_dictionary_set_string(reply, "error", error);
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
}

static void handle_unit_op(xpc_connection_t peer, xpc_object_t msg, xpc_object_t reply, const char* op)
{
    Unit* u = find_unit(xpc_dictionary_get_uint64(msg, "product"),
                        xpc_dictionary_get_uint64(msg, "location"));
    if (!u) { reply_and_release(peer, reply, "no such unit"); return; }
    if (gQuitting) { reply_and_release(peer, reply, "engine is shutting down"); return; }

    xpc_retain(msg);
    xpc_retain(peer);
    dispatch_async(u->queue, ^{
        const char* error = NULL;
        if (!u->engine) {
            error = "no such unit";
        } else if (strcmp(op, "open") == 0) {
            error = unit_open(u, xpc_dictionary_get_value(msg, "memory"));
        } else if (strcmp(op, "start") == 0) {
            uint64_t generation = xpc_dictionary_get_uint64(msg, "generation");
            error = unit_start(u, generation, xpc_dictionary_get_uint64(msg, "rate"),
                               xpc_dictionary_get_uint64(msg, "safety_us"),
                               xpc_dictionary_get_bool(msg, "with_input"));
            xpc_dictionary_set_uint64(reply, "generation", generation);
        } else if (strcmp(op, "stop") == 0) {
            /* A stop for a session that is no longer the current one must
             * not touch the current one. */
            if (u->running && xpc_dictionary_get_uint64(msg, "generation") != u->generation) {
                xpc_dictionary_set_bool(reply, "stale", true);
                error = "stale generation";
            } else {
                unit_stop(u);
            }
        } else if (strcmp(op, "close") == 0) {
            unit_detach(u);
        } else if (strcmp(op, "reset") == 0) {
            emu_engine_reset_counters(u->engine);
            if (u->shm) {
                uint64_t passes = atomic_load(&u->shm->pump.passes);
                memset(&u->shm->pump, 0, sizeof u->shm->pump);
                atomic_store(&u->shm->pump.passes, passes);
            }
            unit_publish_stats(u);
        } else if (strcmp(op, "fault") == 0) {
            uint64_t mode = xpc_dictionary_get_uint64(msg, "mode");
            if (mode > EMU_FAULT_STARTUP_STALE) error = "bad parameters";
            else {
                emu_engine_inject_fault(u->engine, (EmuFaultMode)mode);
                unit_publish_stats(u);
            }
        } else {
            error = "unknown operation";
        }
        reply_and_release(peer, reply, error);
        xpc_release(msg);
        xpc_release(peer);
    });
}

static void handle_status(xpc_connection_t peer, xpc_object_t reply)
{
    xpc_dictionary_set_uint64(reply, "abi", EMU_ENGINE_ABI);
    xpc_dictionary_set_int64(reply, "pid", getpid());
    xpc_dictionary_set_string(reply, "build", BUILD_STRING);
    xpc_connection_t owner = owner_retain();
    xpc_dictionary_set_int64(reply, "halPid", owner ? xpc_connection_get_pid(owner) : 0);
    if (owner) xpc_release(owner);

    xpc_object_t list = xpc_array_create(NULL, 0);
    xpc_dictionary_set_value(reply, "units", list);
    dispatch_group_t group = dispatch_group_create();
    for (unsigned i = 0; i < MAX_UNITS; i++) {
        Unit* u = &gUnits[i];
        if (!u->in_use || !u->present) continue;
        xpc_object_t d = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_uint64(d, "product", u->product);
        xpc_dictionary_set_uint64(d, "location", u->location);
        xpc_dictionary_set_string(d, "serial", u->serial);
        xpc_array_append_value(list, d);
        dispatch_group_async(group, u->queue, ^{
            if (u->engine) describe_unit(u, d);
            xpc_release(d);
        });
    }
    xpc_release(list);
    xpc_retain(peer);
    dispatch_group_notify(group, gControl, ^{
        reply_and_release(peer, reply, NULL);
        xpc_release(peer);
    });
    dispatch_release(group);
}

static void handle_message(xpc_connection_t peer, xpc_object_t msg, int role)
{
    const char* op = xpc_dictionary_get_string(msg, "op");
    xpc_object_t reply = xpc_dictionary_create_reply(msg);
    if (!op || !reply) { if (reply) xpc_release(reply); return; }

    if (strcmp(op, "status") == 0) {
        handle_status(peer, reply);

    } else if (strcmp(op, "shutdown") == 0) {
        if (!(role & ROLE_CONTROL)) { reply_and_release(peer, reply, "not permitted"); return; }
        gQuitting = true;
        xpc_retain(peer);
        stop_everything(false, ^{
            /* Every transfer has been returned. Say so, and only leave once
             * the answer has been handed to the transport: an exit that races
             * its own acknowledgement looks, to the caller, like a crash. */
            xpc_dictionary_set_int64(reply, "pid", getpid());
            xpc_dictionary_set_string(reply, "build", BUILD_STRING);
            reply_and_release(peer, reply, NULL);
            xpc_connection_send_barrier(peer, ^{ exit(0); });
        });

    } else if (strcmp(op, "hello") == 0) {
        if (!(role & ROLE_HAL)) { reply_and_release(peer, reply, "not permitted"); return; }
        uint64_t abi = EMU_ENGINE_ABI;
#ifdef EMU_ENGINE_TEST_HOOKS
        if (getenv("EMU_ENGINE_FAKE_ABI")) abi = strtoull(getenv("EMU_ENGINE_FAKE_ABI"), NULL, 10);
#endif
        xpc_dictionary_set_uint64(reply, "abi", abi);
        xpc_dictionary_set_int64(reply, "pid", getpid());
        xpc_dictionary_set_string(reply, "build", BUILD_STRING);
        if (xpc_dictionary_get_uint64(msg, "abi") != abi) {
            reply_and_release(peer, reply, "ABI mismatch");
            return;
        }
        if (gQuitting) { reply_and_release(peer, reply, "engine is shutting down"); return; }
        os_unfair_lock_lock(&gOwnerLock);
        bool taken = gOwner && gOwner != peer;
        if (!taken && !gOwner) gOwner = xpc_retain(peer);
        os_unfair_lock_unlock(&gOwnerLock);
        /* One HAL at a time. A second is refused rather than allowed to
         * share the devices; the first one's connection ending is what frees
         * the place. */
        if (taken) { reply_and_release(peer, reply, "another HAL is connected"); return; }
        xpc_object_t list = units_array();
        xpc_dictionary_set_value(reply, "units", list);
        xpc_release(list);
        const char* build = xpc_dictionary_get_string(msg, "build");
        SLOG("HAL connected: pid %d, %{public}s", xpc_connection_get_pid(peer), build ? build : "?");
        reply_and_release(peer, reply, NULL);

#ifdef EMU_ENGINE_TEST_HOOKS
    } else if (strcmp(op, "test-crash") == 0) {
        /* Dies the way a real fault would: no stop, no reply, and -- if asked
         * -- half way through publishing the clock. */
        Unit* u = find_unit(xpc_dictionary_get_uint64(msg, "product"),
                            xpc_dictionary_get_uint64(msg, "location"));
        bool mid = xpc_dictionary_get_bool(msg, "mid_publication");
        xpc_release(reply);
        if (u && u->queue) {
            dispatch_async(u->queue, ^{
                if (mid && u->shm) {
                    atomic_store(&u->pump_stop, true);
                    if (u->running) pthread_join(u->pump, NULL);
                    atomic_fetch_or(&u->shm->tl_seq, 1u);
                    atomic_fetch_or(&u->shm->stats_seq, 1u);
                }
                _exit(17);
            });
        } else {
            _exit(17);
        }
    } else if (strcmp(op, "test-hang") == 0) {
        /* Stops the pump and leaves everything else as it is: an engine that
         * is still there and no longer doing anything. */
        Unit* u = find_unit(xpc_dictionary_get_uint64(msg, "product"),
                            xpc_dictionary_get_uint64(msg, "location"));
        if (u) dispatch_async(u->queue, ^{
            if (u->running) { atomic_store(&u->pump_stop, true); pthread_join(u->pump, NULL); }
            sleep(3600);
        });
        reply_and_release(peer, reply, NULL);
#endif

    } else if (role & ROLE_HAL) {
        if (!is_owner(peer)) { reply_and_release(peer, reply, "not the connected HAL"); return; }
        handle_unit_op(peer, msg, reply, op);

    } else {
        reply_and_release(peer, reply, "not permitted");
    }
}

static void peer_gone(xpc_connection_t peer)
{
    os_unfair_lock_lock(&gOwnerLock);
    bool was_owner = peer == gOwner;
    if (was_owner) { xpc_release(gOwner); gOwner = NULL; }
    os_unfair_lock_unlock(&gOwnerLock);
    if (!was_owner) return;
    /* The HAL's host has exited or restarted. Its sessions have no client
     * and its regions no reader; a new HAL brings its own. */
    SLOG("HAL disconnected; stopping its sessions");
    stop_everything(true, ^{});
}

static int serve(const char* service)
{
    gControl = dispatch_queue_create("net.batarelo.EMUTrackerPre.engine", DISPATCH_QUEUE_SERIAL);

    if (!emu_engine_set_identity_observer(units_changed)) {
        fprintf(stderr, "emu-engine: could not watch for devices\n");
        return 1;
    }
    dispatch_sync(gControl, ^{ refresh_units(); });

    xpc_connection_t listener = xpc_connection_create_mach_service(
        service, gControl, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_CONNECTION) return;
        xpc_connection_t peer = (xpc_connection_t)event;
        int role;
        if (xpc_connection_get_euid(peer) == geteuid()) {
            /* The developer: may ask and may shut the engine down, and is not
             * an audio host. */
            role = ROLE_CONTROL;
#ifdef EMU_ENGINE_TEST_HOOKS
            role |= ROLE_HAL;       /* the tests' stand-in HAL runs as the developer */
#endif
        } else {
            if (xpc_connection_set_peer_code_signing_requirement(peer, HAL_PEER_REQUIREMENT) != 0) {
                xpc_connection_cancel(peer);
                return;
            }
            role = ROLE_HAL;
        }
        xpc_connection_set_target_queue(peer, gControl);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) {
            xpc_type_t type = xpc_get_type(msg);
            if (type == XPC_TYPE_ERROR) peer_gone(peer);
            else if (type == XPC_TYPE_DICTIONARY) handle_message(peer, msg, role);
        });
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);

    /* Diagnostics, 20 times a second, for every open unit: the HAL reads these
     * from shared memory and never asks. */
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gControl);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                              50 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{
        for (unsigned i = 0; i < MAX_UNITS; i++) {
            Unit* u = &gUnits[i];
            if (u->in_use && u->present) dispatch_async(u->queue, ^{ unit_publish_stats(u); });
        }
    });
    dispatch_resume(timer);

    /* launchd's way of asking: the same clean stop, without a reply to send. */
    signal(SIGTERM, SIG_IGN);
    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, gControl);
    dispatch_source_set_event_handler(term, ^{
        gQuitting = true;
        stop_everything(false, ^{ exit(0); });
    });
    dispatch_resume(term);

    SLOG("engine %{public}s serving %{public}s as uid %d, ABI %u", BUILD_STRING, service,
         (int)geteuid(), EMU_ENGINE_ABI);
    dispatch_main();
}

/* --- ctl -------------------------------------------------------------------------- */

static void print_json(xpc_object_t v, int indent)
{
    xpc_type_t t = xpc_get_type(v);
    if (t == XPC_TYPE_DICTIONARY) {
        printf("{\n");
        __block bool first = true;
        xpc_dictionary_apply(v, ^bool(const char* key, xpc_object_t value) {
            if (!first) printf(",\n");
            first = false;
            printf("%*s\"%s\": ", indent + 2, "", key);
            print_json(value, indent + 2);
            return true;
        });
        printf("\n%*s}", indent, "");
    } else if (t == XPC_TYPE_ARRAY) {
        printf("[");
        for (size_t i = 0; i < xpc_array_get_count(v); i++) {
            if (i) printf(", ");
            print_json(xpc_array_get_value(v, i), indent + 2);
        }
        printf("]");
    } else if (t == XPC_TYPE_STRING) {
        printf("\"%s\"", xpc_string_get_string_ptr(v));
    } else if (t == XPC_TYPE_UINT64) {
        printf("%llu", (unsigned long long)xpc_uint64_get_value(v));
    } else if (t == XPC_TYPE_INT64) {
        printf("%lld", (long long)xpc_int64_get_value(v));
    } else if (t == XPC_TYPE_BOOL) {
        printf("%s", xpc_bool_get_value(v) ? "true" : "false");
    } else {
        printf("null");
    }
}

static int control(const char* op, const char* service, bool user_domain)
{
    dispatch_queue_t q = dispatch_queue_create("emu-engine.ctl", DISPATCH_QUEUE_SERIAL);
    xpc_connection_t c = xpc_connection_create_mach_service(
        service, q, user_domain ? 0 : XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
    xpc_connection_set_event_handler(c, ^(xpc_object_t event) { (void)event; });
    xpc_connection_resume(c);

    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "op", op);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block int result = 1;
    xpc_connection_send_message_with_reply(c, msg, q, ^(xpc_object_t reply) {
        if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY) {
            print_json(reply, 0);
            printf("\n");
            result = xpc_dictionary_get_bool(reply, "ok") ? 0 : 1;
        } else {
            fprintf(stderr, "emu-engine: no engine answered on %s\n", service);
        }
        dispatch_semaphore_signal(done);
    });
    /* A shutdown waits for USB to drain; nothing here forces it. */
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 15 * (int64_t)NSEC_PER_SEC))) {
        fprintf(stderr, "emu-engine: no answer within 15 s; nothing was killed\n");
        return 1;
    }
    return result;
}

int main(int argc, char** argv)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gTicksPerNs = (double)tb.denom / (double)tb.numer;

    if (argc >= 3 && strcmp(argv[1], "ctl") == 0 &&
        (strcmp(argv[2], "status") == 0 || strcmp(argv[2], "shutdown") == 0)) {
        const char* service = EMU_ENGINE_SERVICE;
        bool user_domain = false;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--user-domain") == 0) user_domain = true;
            else service = argv[i];
        }
        return control(argv[2], service, user_domain);
    }
    if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "usage: emu-engine <service>\n"
                        "       emu-engine ctl status|shutdown [<service>] [--user-domain]\n");
        return 2;
    }
    if (geteuid() == 0) {
        fprintf(stderr, "emu-engine: refusing to run as root\n");
        return 2;
    }
    return serve(argv[1]);
}
