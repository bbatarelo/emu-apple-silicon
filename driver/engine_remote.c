/*
 * The engine, seen from the HAL, when the engine is another process.
 *
 * Implements usb_engine.h -- and the unit enumeration plugin.c takes from
 * shared/usb_util.h -- over the boundary in engine_shared.h, so plugin.c is the
 * same file in both builds and links against this instead of usb_engine.c.
 * Nothing here touches USB.
 *
 * Three kinds of caller, and what each may do:
 *
 *   Core Audio's IO threads   write_output, read_input, timeline. Shared
 *                             memory only: no message, no lock, no wait. With
 *                             no session they do nothing, at once.
 *   Control threads           create/start/stop/destroy, stats, reset, fault.
 *                             May send a message and wait for its reply, with
 *                             a bound; a missing service fails immediately.
 *   The connection queue      owns the XPC connection and nothing else. It
 *                             never calls into the plug-in: observer and
 *                             failure callbacks run on their own queue, so a
 *                             callback that stops an engine can wait for a
 *                             reply without waiting for itself.
 *
 * An engine handle here is a proxy for one physical unit. Proxies and their
 * shared regions are never freed: an IO thread may still hold either when the
 * device is retired, and a unit that returns to the same port gets the same
 * proxy back.
 */

#include <dispatch/dispatch.h>
#include <os/lock.h>
#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xpc/xpc.h>

#include "engine_shared.h"
#include "../shared/usb_util.h"

#define MAX_PROXIES 16u
#define MAX_UNITS   8u

/* Bounds on control waits. The engine's own start is bounded at 8 s and its
 * stop by the USB drain; these sit just outside so the engine's answer, not
 * this timer, is what normally ends the wait. */
#define HELLO_WAIT_MS   500u
#define OPEN_WAIT_MS   2000u
#define START_WAIT_MS 10000u
#define STOP_WAIT_MS   6000u
#define SMALL_WAIT_MS  1000u
/* How long the last inventory is kept after the engine goes away. An engine
 * being replaced is back well inside it, and its devices stay published --
 * with their settings -- instead of vanishing and reappearing as new ones. */
#define INVENTORY_GRACE_S 10
#define RECONNECT_S        1

struct Engine {
    bool     used;                       /* gLock */
    bool     known;                      /* gLock: slot has ever held a unit */
    uint16_t product;
    uint64_t location;

    pthread_mutex_t control;             /* open/start/stop/close, one at a time */
    _Atomic(EmuShared*) shm;
    xpc_object_t shmem;                  /* control */
    size_t   bytes;                      /* control */
    uint64_t opened_epoch;               /* control: connection the engine mapped it in */
    _Atomic bool tainted;                /* region may still have a writer */

    _Atomic bool     running;
    _Atomic bool     with_input;
    _Atomic uint64_t generation;         /* of the running session */
    _Atomic uint32_t fault;
    _Atomic uint32_t gain_bits;
    _Atomic uint64_t unsent_frames;      /* written by Core Audio with no session to take them */
    void (* _Atomic failed)(void*);
    void* _Atomic failed_context;

    os_unfair_lock stats_lock;
    EmuEngineStats final;                /* last session's, as usb_engine.h promises */
    bool           have_final;
    EmuEngineStats recent;               /* last snapshot read whole, while running */
    uint64_t       recent_tail;          /* ...and how much of `play` it accounts for */
    bool           have_recent;
    /* Frames the engine took from `play` that no published figures ever
     * covered: its session ended first. Bound or not, nobody can say. */
    _Atomic uint64_t unreconciled;
};

static struct Engine  gProxies[MAX_PROXIES];
static os_unfair_lock gLock = OS_UNFAIR_LOCK_INIT;      /* proxies' identity, the inventory */
static EmuUnit        gUnits[MAX_UNITS];
static unsigned       gUnitCount;

static dispatch_queue_t gQueue;          /* serial: the connection */
static dispatch_queue_t gCallbacks;      /* serial: into the plug-in */
static dispatch_queue_t gWork;           /* replies and background opens */
static xpc_connection_t gConn;           /* gQueue */
static uint64_t         gEpochCounter;   /* gQueue */
static _Atomic uint64_t gEpoch;          /* 0: no engine has answered hello */
static _Atomic(void (*)(void)) gObserver;
static _Atomic uint32_t gRetiredRegions;

static os_log_t remote_log(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create(EMU_LOG_SUBSYSTEM, "remote"); });
    return log;
}
#define RLOG(fmt, ...) os_log(remote_log(), "engine link: " fmt, ##__VA_ARGS__)
#define RERR(fmt, ...) os_log_error(remote_log(), "engine link: " fmt, ##__VA_ARGS__)

/* --- requests --------------------------------------------------------------- */

typedef enum { kReplyOK, kReplyNoEngine, kReplyTimeout } ReplyStatus;

typedef struct {
    _Atomic unsigned     refs;
    xpc_object_t         value;
    dispatch_semaphore_t done;
} Pending;

static void pending_release(Pending* p)
{
    if (atomic_fetch_sub(&p->refs, 1) != 1) return;
    if (p->value) xpc_release(p->value);
    dispatch_release(p->done);
    free(p);
}

/* Sends and waits, off the connection queue. kReplyOK hands back a dictionary
 * the caller releases. kReplyNoEngine is an answer too -- there is no engine,
 * or it went away -- and arrives at once; only kReplyTimeout means an engine
 * that may still be doing something. Consumes `msg`. */
static ReplyStatus request(xpc_object_t msg, uint32_t wait_ms, xpc_object_t* reply)
{
    *reply = NULL;
    __block xpc_connection_t c = NULL;
    dispatch_sync(gQueue, ^{
        if (gConn && atomic_load(&gEpoch)) c = xpc_retain(gConn);
    });
    if (!c) { xpc_release(msg); return kReplyNoEngine; }

    Pending* p = calloc(1, sizeof *p);
    atomic_init(&p->refs, 2);
    p->done = dispatch_semaphore_create(0);
    xpc_connection_send_message_with_reply(c, msg, gWork, ^(xpc_object_t value) {
        p->value = xpc_retain(value);
        dispatch_semaphore_signal(p->done);
        pending_release(p);
    });
    xpc_release(msg);
    xpc_release(c);

    ReplyStatus status = kReplyTimeout;
    if (dispatch_semaphore_wait(p->done, dispatch_time(DISPATCH_TIME_NOW,
                                (int64_t)wait_ms * (int64_t)NSEC_PER_MSEC)) == 0) {
        if (xpc_get_type(p->value) == XPC_TYPE_DICTIONARY) {
            *reply = xpc_retain(p->value);
            status = kReplyOK;
        } else {
            status = kReplyNoEngine;
        }
    }
    pending_release(p);
    return status;
}

static xpc_object_t unit_message(const struct Engine* e, const char* op)
{
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "op", op);
    xpc_dictionary_set_uint64(msg, "product", e->product);
    xpc_dictionary_set_uint64(msg, "location", e->location);
    return msg;
}

static bool reply_ok(xpc_object_t reply)
{
    return reply && xpc_dictionary_get_bool(reply, "ok");
}

/* --- final statistics ------------------------------------------------------- */

/* Keeps what the engine last said about a session that has just ended, so it
 * can still be read after the engine is replaced. */
static void capture_final(struct Engine* e)
{
    EmuShared* shm = atomic_load(&e->shm);
    EmuEngineStats stats;
    uint64_t covered = 0;
    bool fresh = shm && emu_shared_read_stats(shm, &stats, &covered);
    os_unfair_lock_lock(&e->stats_lock);
    /* An engine that died inside a publication left nothing readable; the
     * copy taken while it was still running is at most half a second old. */
    if (!fresh && e->have_recent) { stats = e->recent; covered = e->recent_tail; fresh = true; }
    if (fresh) {
        /* After a clean stop the last figures cover everything the engine
         * took. After anything else they may not, and what they miss is
         * kept as its own number rather than left to look like nothing. */
        uint64_t taken = atomic_load_explicit(&shm->pcm_tail, memory_order_acquire);
        if (taken - covered <= EMU_RING_FRAMES)
            atomic_fetch_add_explicit(&e->unreconciled, taken - covered, memory_order_relaxed);
        stats.engine_streaming = 0;
        e->final = stats;
        e->have_final = true;
    }
    e->have_recent = false;
    os_unfair_lock_unlock(&e->stats_lock);
    if (fresh) atomic_store(&e->fault, stats.fault_mode);
}

/* Keeps a whole copy of a running session's figures on this side of the
 * boundary. Control threads only. */
static bool read_live(struct Engine* e, EmuShared* shm, EmuEngineStats* out, uint64_t* tail)
{
    EmuEngineStats stats;
    uint64_t covered = 0;
    bool ok = emu_shared_read_stats(shm, &stats, &covered);
    os_unfair_lock_lock(&e->stats_lock);
    if (ok) { e->recent = stats; e->recent_tail = covered; e->have_recent = true; }
    else if (e->have_recent) { stats = e->recent; covered = e->recent_tail; ok = true; }
    os_unfair_lock_unlock(&e->stats_lock);
    if (ok && out) *out = stats;
    if (ok && tail) *tail = covered;
    return ok;
}

/* A session ended underneath its clients. Any thread; never the IO thread. */
static void session_lost(struct Engine* e, const char* why)
{
    if (!atomic_exchange(&e->running, false)) return;
    capture_final(e);
    RERR("session on %04x@%08llx ended: %{public}s", e->product,
         (unsigned long long)e->location, why);
    void (*handler)(void*) = atomic_load(&e->failed);
    void* context = atomic_load(&e->failed_context);
    if (handler) dispatch_async(gCallbacks, ^{ handler(context); });
}

/* --- the inventory ---------------------------------------------------------- */

static void notify_observer(void)
{
    void (*observer)(void) = atomic_load(&gObserver);
    if (observer) dispatch_async(gCallbacks, ^{ observer(); });
}

/* gQueue. Returns whether the set changed. */
static bool apply_units(xpc_object_t list)
{
    EmuUnit units[MAX_UNITS];
    unsigned n = 0;
    size_t count = (list && xpc_get_type(list) == XPC_TYPE_ARRAY) ? xpc_array_get_count(list) : 0;
    for (size_t i = 0; i < count && n < MAX_UNITS; i++) {
        xpc_object_t u = xpc_array_get_value(list, i);
        if (xpc_get_type(u) != XPC_TYPE_DICTIONARY) continue;
        const EmuDeviceIdentity* identity =
            emu_device_for_product((uint16_t)xpc_dictionary_get_uint64(u, "product"));
        const char* serial = xpc_dictionary_get_string(u, "serial");
        if (!identity) continue;                  /* a product this HAL does not know */
        memset(&units[n], 0, sizeof units[n]);
        units[n].identity    = identity;
        units[n].location_id = xpc_dictionary_get_uint64(u, "location");
        strlcpy(units[n].serial, serial ? serial : "", sizeof units[n].serial);
        n++;
    }

    os_unfair_lock_lock(&gLock);
    bool changed = n != gUnitCount;
    for (unsigned i = 0; i < n && !changed; i++) {
        changed = units[i].identity != gUnits[i].identity
               || units[i].location_id != gUnits[i].location_id
               || strcmp(units[i].serial, gUnits[i].serial) != 0;
    }
    memcpy(gUnits, units, sizeof gUnits);
    gUnitCount = n;
    os_unfair_lock_unlock(&gLock);
    return changed;
}

unsigned emu_enumerate_units(EmuUnit* out, unsigned max)
{
    os_unfair_lock_lock(&gLock);
    unsigned n = gUnitCount < max ? gUnitCount : max;
    memcpy(out, gUnits, n * sizeof *out);
    os_unfair_lock_unlock(&gLock);
    return n;
}

/* --- the shared region ------------------------------------------------------ */

/* e->control held. Gives the proxy a region the engine has never seen, if it
 * has none or the one it has may still be written by an engine that never
 * confirmed it had stopped. The old one is abandoned in place, mapped: an IO
 * thread may be inside it, and so may that engine. */
static bool ensure_region(struct Engine* e)
{
    if (atomic_load(&e->shm) && !atomic_load(&e->tainted)) return true;

    size_t page  = (size_t)getpagesize();
    size_t bytes = (sizeof(EmuShared) + page - 1) / page * page;
    void* p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return false;
    xpc_object_t shmem = xpc_shmem_create(p, bytes);
    if (!shmem) { munmap(p, bytes); return false; }

    emu_shared_init((EmuShared*)p, bytes);     /* touches every page */
    (void)mlock(p, bytes);                     /* best effort: the IO thread reads it */
    atomic_store(&((EmuShared*)p)->gain_bits, atomic_load(&e->gain_bits));

    if (atomic_load(&e->shm)) {
        uint32_t n = atomic_fetch_add(&gRetiredRegions, 1) + 1;
        RERR("retired a shared region whose engine never confirmed stopping (%u so far)", n);
    }
    if (e->shmem) xpc_release(e->shmem);
    e->shmem = shmem;
    e->bytes = bytes;
    atomic_store(&e->tainted, false);
    e->opened_epoch = 0;
    atomic_store(&e->shm, (EmuShared*)p);
    return true;
}

/* e->control held. Makes sure the engine now connected has this unit's region
 * mapped. */
static bool ensure_open(struct Engine* e)
{
    uint64_t epoch = atomic_load(&gEpoch);
    if (!epoch) return false;
    if (!ensure_region(e)) return false;
    if (e->opened_epoch == epoch) return true;

    xpc_object_t msg = unit_message(e, "open");
    xpc_dictionary_set_value(msg, "memory", e->shmem);
    xpc_object_t reply = NULL;
    ReplyStatus status = request(msg, OPEN_WAIT_MS, &reply);
    bool ok = status == kReplyOK && reply_ok(reply);
    if (reply) xpc_release(reply);
    if (ok) e->opened_epoch = epoch;
    else if (status != kReplyNoEngine)
        RERR("engine did not accept the shared region for %04x@%08llx", e->product,
             (unsigned long long)e->location);
    return ok;
}

static void open_in_background(struct Engine* e)
{
    dispatch_async(gWork, ^{
        pthread_mutex_lock(&e->control);
        bool wanted;
        os_unfair_lock_lock(&gLock);
        wanted = e->used;
        os_unfair_lock_unlock(&gLock);
        if (wanted) ensure_open(e);
        pthread_mutex_unlock(&e->control);
    });
}

/* --- the connection --------------------------------------------------------- */

static void connect_on_queue(void);
static dispatch_semaphore_t gFirstAnswer;

static const char* service_name(void)
{
#ifdef EMU_REMOTE_TEST_SERVICE
    const char* name = getenv("EMU_ENGINE_SERVICE");
    if (name && *name) return name;
#endif
    return EMU_ENGINE_SERVICE;
}

/* gQueue. The engine is gone, or was never there. */
static void lost_on_queue(xpc_connection_t c, bool had_engine)
{
    if (c != gConn) return;
    xpc_connection_cancel(c);
    xpc_release(c);
    gConn = NULL;
    atomic_store(&gEpoch, 0);

    for (unsigned i = 0; i < MAX_PROXIES; i++) session_lost(&gProxies[i], "the engine went away");

    if (had_engine) {
        RLOG("engine gone; keeping its devices for %d s", INVENTORY_GRACE_S);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, INVENTORY_GRACE_S * (int64_t)NSEC_PER_SEC),
                       gQueue, ^{
            if (atomic_load(&gEpoch)) return;
            if (apply_units(NULL)) {
                RERR("no engine after %d s; withdrawing its devices", INVENTORY_GRACE_S);
                notify_observer();
            }
        });
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, RECONNECT_S * (int64_t)NSEC_PER_SEC),
                   gQueue, ^{ connect_on_queue(); });
    dispatch_semaphore_signal(gFirstAnswer);
}

/* gQueue. Something the engine said unasked. */
static void event_on_queue(xpc_object_t event)
{
    const char* name = xpc_dictionary_get_string(event, "event");
    if (!name) return;
    if (strcmp(name, "units") == 0) {
        if (apply_units(xpc_dictionary_get_value(event, "units"))) notify_observer();
        return;
    }
    if (strcmp(name, "failed") == 0) {
        uint16_t product  = (uint16_t)xpc_dictionary_get_uint64(event, "product");
        uint64_t location = xpc_dictionary_get_uint64(event, "location");
        uint64_t generation = xpc_dictionary_get_uint64(event, "generation");
        for (unsigned i = 0; i < MAX_PROXIES; i++) {
            struct Engine* e = &gProxies[i];
            if (e->product == product && e->location == location &&
                atomic_load(&e->generation) == generation) {
                session_lost(e, "the engine gave up on the device");
            }
        }
    }
}

static void connect_on_queue(void)
{
    if (gConn) return;
    uint64_t flags = XPC_CONNECTION_MACH_SERVICE_PRIVILEGED;
#ifdef EMU_REMOTE_TEST_SERVICE
    if (getenv("EMU_ENGINE_SERVICE")) flags = 0;      /* the test's own user-domain service */
#endif
    xpc_connection_t c = xpc_connection_create_mach_service(service_name(), gQueue, flags);
    if (!c) return;
    gConn = c;
    xpc_connection_set_event_handler(c, ^(xpc_object_t event) {
        xpc_type_t type = xpc_get_type(event);
        if (type == XPC_TYPE_ERROR) lost_on_queue(c, atomic_load(&gEpoch) != 0);
        else if (type == XPC_TYPE_DICTIONARY && c == gConn) event_on_queue(event);
    });
    xpc_connection_resume(c);

    xpc_object_t hello = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(hello, "op", "hello");
    xpc_dictionary_set_uint64(hello, "abi", EMU_ENGINE_ABI);
    xpc_dictionary_set_string(hello, "build", EMU_VERSION " (git " EMU_GITREV ")");
    xpc_connection_send_message_with_reply(c, hello, gQueue, ^(xpc_object_t reply) {
        if (c != gConn) return;
        if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY) return;   /* the error handler has it */
        uint64_t abi = xpc_dictionary_get_uint64(reply, "abi");
        if (!xpc_dictionary_get_bool(reply, "ok") || abi != EMU_ENGINE_ABI) {
            /* Not an engine this HAL can use. Publishing nothing is the
             * truthful state; it is retried, since the usual cause is an
             * engine about to be replaced by a matching one. */
            const char* why = xpc_dictionary_get_string(reply, "error");
            RERR("engine refused: %{public}s (its ABI %llu, ours %u)", why ? why : "no reason given",
                 (unsigned long long)abi, EMU_ENGINE_ABI);
            lost_on_queue(c, atomic_load(&gEpoch) != 0);
            return;
        }
        atomic_store(&gEpoch, ++gEpochCounter);
        const char* build = xpc_dictionary_get_string(reply, "build");
        RLOG("engine connected: pid %lld, %{public}s", (long long)xpc_dictionary_get_int64(reply, "pid"),
             build ? build : "unknown build");
        if (apply_units(xpc_dictionary_get_value(reply, "units"))) notify_observer();
        os_unfair_lock_lock(&gLock);
        for (unsigned i = 0; i < MAX_PROXIES; i++) {
            if (gProxies[i].used) open_in_background(&gProxies[i]);
        }
        os_unfair_lock_unlock(&gLock);
        dispatch_semaphore_signal(gFirstAnswer);
    });
    xpc_release(hello);
}

/* gQueue, twice a second. A hung engine raises no XPC error: its pump simply
 * stops, and Core Audio would go on playing into nothing. */
static void watchdog_on_queue(void)
{
    static uint64_t limit;
    if (!limit) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        limit = (uint64_t)EMU_HEARTBEAT_TIMEOUT_MS * 1000000ull * tb.denom / tb.numer;
    }
    for (unsigned i = 0; i < MAX_PROXIES; i++) {
        struct Engine* e = &gProxies[i];
        EmuShared* shm = atomic_load(&e->shm);
        if (!shm || !atomic_load(&e->running)) continue;
        read_live(e, shm, NULL, NULL);
        uint64_t beat = atomic_load_explicit(&shm->heartbeat, memory_order_relaxed);
        uint64_t now = mach_absolute_time();          /* sampled after the read */
        if (beat && now > beat && now - beat > limit) {
            /* Still there, so it may still write: never hand it this region
             * again. */
            atomic_store(&e->tainted, true);
            session_lost(e, "the engine stopped responding");
        }
    }
}

static void link_start(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        for (unsigned i = 0; i < MAX_PROXIES; i++) {
            pthread_mutex_init(&gProxies[i].control, NULL);
            gProxies[i].stats_lock = OS_UNFAIR_LOCK_INIT;
            atomic_init(&gProxies[i].gain_bits, 0x3f800000u);
        }
        gQueue     = dispatch_queue_create("net.batarelo.EMUTrackerPre.link", DISPATCH_QUEUE_SERIAL);
        gCallbacks = dispatch_queue_create("net.batarelo.EMUTrackerPre.link.callbacks", DISPATCH_QUEUE_SERIAL);
        gWork      = dispatch_queue_create("net.batarelo.EMUTrackerPre.link.work", DISPATCH_QUEUE_CONCURRENT);
        gFirstAnswer = dispatch_semaphore_create(0);
        dispatch_async(gQueue, ^{ connect_on_queue(); });

        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gQueue);
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                  500 * NSEC_PER_MSEC, 100 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer, ^{ watchdog_on_queue(); });
        dispatch_resume(timer);                       /* lives as long as the host does */

        /* Core Audio reads the device list as soon as Initialize returns, so
         * give an engine that is already up the chance to say what it has.
         * Bounded: an absent engine is not an error, just an empty list that
         * fills in when one arrives. */
        dispatch_semaphore_wait(gFirstAnswer, dispatch_time(DISPATCH_TIME_NOW,
                                HELLO_WAIT_MS * (int64_t)NSEC_PER_MSEC));
    });
}

/* --- usb_engine.h ----------------------------------------------------------- */

bool emu_engine_set_identity_observer(void (*observer)(void))
{
    atomic_store(&gObserver, observer);
    link_start();
    /* True with no engine too: the watch that matters here is the connection,
     * and it is armed. Refusing would leave the plug-in uninitialized for
     * good, with no way to notice an engine starting later. */
    return true;
}

bool emu_engine_device_attached(void)
{
    os_unfair_lock_lock(&gLock);
    bool any = gUnitCount > 0;
    os_unfair_lock_unlock(&gLock);
    return any;
}

const char* emu_engine_device_name(void) { return "E-MU USB Audio"; }
const char* emu_engine_log_name(void)    { return "E-MU device"; }

EmuEngine* emu_engine_create(uint16_t product_id, uint64_t location_id)
{
    link_start();
    struct Engine* e = NULL;
    os_unfair_lock_lock(&gLock);
    for (unsigned i = 0; i < MAX_PROXIES && !e; i++) {
        struct Engine* p = &gProxies[i];
        if (p->known && !p->used && p->product == product_id && p->location == location_id) e = p;
    }
    for (unsigned i = 0; i < MAX_PROXIES && !e; i++) {
        if (!gProxies[i].known) e = &gProxies[i];
    }
    if (e) {
        e->known = e->used = true;
        e->product = product_id;
        e->location = location_id;
    }
    os_unfair_lock_unlock(&gLock);
    if (!e) {
        RERR("no proxy left for %04x@%08llx", product_id, (unsigned long long)location_id);
        return NULL;
    }
    atomic_store(&e->failed, NULL);
    atomic_store(&e->failed_context, NULL);
    atomic_store(&e->fault, EMU_FAULT_NONE);
    atomic_store(&e->gain_bits, 0x3f800000u);
    os_unfair_lock_lock(&e->stats_lock);
    e->have_final = false;
    os_unfair_lock_unlock(&e->stats_lock);
    open_in_background(e);
    return e;
}

void emu_engine_destroy(EmuEngine* e)
{
    if (!e) return;
    emu_engine_stop(e);
    pthread_mutex_lock(&e->control);
    if (e->opened_epoch && e->opened_epoch == atomic_load(&gEpoch)) {
        xpc_object_t reply = NULL;
        request(unit_message(e, "close"), SMALL_WAIT_MS, &reply);
        if (reply) xpc_release(reply);
    }
    e->opened_epoch = 0;
    pthread_mutex_unlock(&e->control);
    os_unfair_lock_lock(&gLock);
    e->used = false;
    os_unfair_lock_unlock(&gLock);
}

bool emu_engine_set_input_ring(EmuEngine* e, struct EmuRing* ring)
{
    (void)e; (void)ring;
    return false;       /* the ring is the shared one; there is nothing to choose */
}

bool emu_engine_start(EmuEngine* e, uint32_t sample_rate, uint32_t output_safety_us, bool with_input)
{
    if (!e) return false;
    if (atomic_load(&e->running)) return true;

    pthread_mutex_lock(&e->control);
    bool ok = ensure_open(e);
    if (ok) {
        EmuShared* shm = atomic_load(&e->shm);
        uint64_t generation = atomic_fetch_add(&shm->generation, 1) + 1;
        atomic_store(&e->with_input, with_input);

        xpc_object_t msg = unit_message(e, "start");
        xpc_dictionary_set_uint64(msg, "generation", generation);
        xpc_dictionary_set_uint64(msg, "rate", sample_rate);
        xpc_dictionary_set_uint64(msg, "safety_us", output_safety_us);
        xpc_dictionary_set_bool(msg, "with_input", with_input);
        xpc_object_t reply = NULL;
        ReplyStatus status = request(msg, START_WAIT_MS, &reply);
        ok = status == kReplyOK && reply_ok(reply)
          && xpc_dictionary_get_uint64(reply, "generation") == generation;
        if (reply) xpc_release(reply);

        /* The engine publishes the anchor before it answers, so the first
         * GetZeroTimeStamp is on the device's clock here too. An answer
         * without one is not a started stream. */
        uint64_t frames = 0, host = 0;
        if (ok && !emu_shared_read_timeline(shm, &frames, &host)) {
            RERR("engine reported a start with no timeline anchor");
            ok = false;
            status = kReplyTimeout;     /* state unknown: treat as unconfirmed */
        }
        if (ok) {
            atomic_store(&e->generation, generation);
            os_unfair_lock_lock(&e->stats_lock);
            e->have_final = false;
            e->have_recent = false;
            os_unfair_lock_unlock(&e->stats_lock);
            atomic_store(&e->running, true);
        } else if (status == kReplyTimeout) {
            /* It may yet start, or be half way. Ask it to stop that
             * generation, do not wait, and never reuse this region. */
            atomic_store(&e->tainted, true);
            xpc_object_t stop = unit_message(e, "stop");
            xpc_dictionary_set_uint64(stop, "generation", generation);
            xpc_object_t ignored = NULL;
            request(stop, 1, &ignored);
            if (ignored) xpc_release(ignored);
        }
    }
    pthread_mutex_unlock(&e->control);
    return ok;
}

void emu_engine_stop(EmuEngine* e)
{
    if (!e) return;
    pthread_mutex_lock(&e->control);
    /* First, so the IO thread stops feeding a session that is ending. */
    if (atomic_exchange(&e->running, false)) {
        xpc_object_t msg = unit_message(e, "stop");
        xpc_dictionary_set_uint64(msg, "generation", atomic_load(&e->generation));
        xpc_object_t reply = NULL;
        ReplyStatus status = request(msg, STOP_WAIT_MS, &reply);
        if (status == kReplyTimeout) {
            RERR("engine did not confirm stopping %04x@%08llx", e->product,
                 (unsigned long long)e->location);
            atomic_store(&e->tainted, true);
        }
        if (reply) xpc_release(reply);
        capture_final(e);
    }
    pthread_mutex_unlock(&e->control);
}

bool emu_engine_running(EmuEngine* e)
{
    return e && atomic_load_explicit(&e->running, memory_order_acquire);
}

bool emu_engine_streaming(EmuEngine* e)
{
    if (!emu_engine_running(e)) return false;
    EmuShared* shm = atomic_load(&e->shm);
    return shm && atomic_load_explicit(&shm->engine_state, memory_order_acquire) == EMU_SHARED_STREAMING;
}

/* IO thread. */
void emu_engine_write_output(EmuEngine* e, const float* frames, uint32_t count, uint64_t sample_pos)
{
    if (!e) return;
    EmuShared* shm = atomic_load_explicit(&e->shm, memory_order_relaxed);
    if (shm && atomic_load_explicit(&e->running, memory_order_relaxed)) {
        emu_shared_write(shm, frames, count, sample_pos);
    } else {
        /* No session to send them to. Counted, as the one-part engine counts
         * a write it has no session for. */
        atomic_fetch_add_explicit(&e->unsent_frames, count, memory_order_relaxed);
    }
}

/* What never reached the engine's books, and what has not reached them yet.
 * Read live: a loss shows at once, and frames still in flight are reported as
 * in flight rather than as written and unbound. */
static void add_boundary_books(struct Engine* e, EmuShared* shm, EmuEngineStats* stats,
                               bool have_covered, uint64_t covered)
{
    uint64_t dropped = atomic_load_explicit(&e->unsent_frames, memory_order_relaxed);
    uint64_t queued = 0, unpublished = 0;
    if (shm) {
        dropped += atomic_load_explicit(&shm->hal_dropped_frames, memory_order_relaxed)
                 + atomic_load_explicit(&shm->hal_oversize_frames, memory_order_relaxed)
                 + atomic_load_explicit(&shm->pump.discarded_frames, memory_order_relaxed);
        uint64_t head = atomic_load_explicit(&shm->pcm_head, memory_order_relaxed);
        uint64_t tail = atomic_load_explicit(&shm->pcm_tail, memory_order_acquire);
        if (head - tail <= EMU_RING_FRAMES) queued = head - tail;
        if (have_covered && tail - covered <= EMU_RING_FRAMES) unpublished = tail - covered;
    }
    stats->frames_dropped      = dropped;
    stats->frames_queued       = queued;
    stats->frames_unpublished  = unpublished;
    stats->frames_unreconciled = atomic_load_explicit(&e->unreconciled, memory_order_relaxed);
}

/* IO thread. */
void emu_engine_read_input(EmuEngine* e, float* frames, uint32_t count, uint64_t sample_pos)
{
    EmuShared* shm = e ? atomic_load_explicit(&e->shm, memory_order_relaxed) : NULL;
    if (shm && atomic_load_explicit(&e->running, memory_order_relaxed)
            && atomic_load_explicit(&e->with_input, memory_order_relaxed)) {
        emu_ring_read_f32(&shm->capture, sample_pos, frames, count);
    } else {
        memset(frames, 0, (size_t)count * EMU_RING_CHANNELS * sizeof(float));
    }
}

/* IO thread. */
bool emu_engine_timeline(EmuEngine* e, uint64_t* frames, uint64_t* host_time)
{
    if (!e || !atomic_load_explicit(&e->running, memory_order_acquire)) return false;
    EmuShared* shm = atomic_load_explicit(&e->shm, memory_order_relaxed);
    if (!shm) return false;
    if (atomic_load_explicit(&shm->engine_state, memory_order_acquire) != EMU_SHARED_STREAMING ||
        atomic_load_explicit(&shm->engine_generation, memory_order_relaxed)
            != atomic_load_explicit(&e->generation, memory_order_relaxed)) {
        return false;
    }
    return emu_shared_read_timeline(shm, frames, host_time);
}

void emu_engine_set_output_gain(EmuEngine* e, float gain)
{
    if (!e) return;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    uint32_t bits;
    memcpy(&bits, &gain, sizeof bits);
    atomic_store(&e->gain_bits, bits);
    EmuShared* shm = atomic_load(&e->shm);
    if (shm) atomic_store_explicit(&shm->gain_bits, bits, memory_order_relaxed);
}

void emu_engine_stats(EmuEngine* e, EmuEngineStats* stats)
{
    if (!e || !stats) return;
    memset(stats, 0, sizeof *stats);
    EmuShared* shm = atomic_load(&e->shm);
    bool running = atomic_load(&e->running);

    bool have = false, have_covered = false;
    uint64_t covered = 0;
    if (!running) {
        os_unfair_lock_lock(&e->stats_lock);
        if (e->have_final) { *stats = e->final; have = true; }
        os_unfair_lock_unlock(&e->stats_lock);
    }
    if (!have && shm) {
        EmuEngineStats live;
        if (running ? read_live(e, shm, &live, &covered) : emu_shared_read_stats(shm, &live, &covered)) {
            *stats = live;
            have_covered = running;     /* stopped: what was missed is already in `unreconciled` */
        }
    }
    if (!running) {
        stats->engine_streaming = 0;
        stats->fault_mode = atomic_load(&e->fault);
    }
    add_boundary_books(e, shm, stats, have_covered, covered);
}

uint64_t emu_engine_frames_played(EmuEngine* e)
{
    EmuEngineStats stats;
    memset(&stats, 0, sizeof stats);
    emu_engine_stats(e, &stats);
    return stats.frames_played;
}

void emu_engine_reset_counters(EmuEngine* e)
{
    if (!e) return;
    xpc_object_t reply = NULL;
    request(unit_message(e, "reset"), SMALL_WAIT_MS, &reply);
    if (reply) xpc_release(reply);
    EmuShared* shm = atomic_load(&e->shm);
    if (shm) {
        atomic_store_explicit(&shm->hal_dropped, 0, memory_order_relaxed);
        atomic_store_explicit(&shm->hal_dropped_frames, 0, memory_order_relaxed);
        atomic_store_explicit(&shm->hal_oversize, 0, memory_order_relaxed);
        atomic_store_explicit(&shm->hal_oversize_frames, 0, memory_order_relaxed);
    }
    atomic_store_explicit(&e->unsent_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&e->unreconciled, 0, memory_order_relaxed);
    os_unfair_lock_lock(&e->stats_lock);
    e->have_final = false;
    os_unfair_lock_unlock(&e->stats_lock);
}

void emu_engine_set_failure_handler(EmuEngine* e, void (*handler)(void*), void* context)
{
    if (!e) return;
    atomic_store(&e->failed_context, context);
    atomic_store(&e->failed, handler);
}

void emu_engine_inject_fault(EmuEngine* e, EmuFaultMode mode)
{
    if (!e) return;
    atomic_store(&e->fault, (uint32_t)mode);
    xpc_object_t msg = unit_message(e, "fault");
    xpc_dictionary_set_uint64(msg, "mode", (uint64_t)mode);
    xpc_object_t reply = NULL;
    request(msg, SMALL_WAIT_MS, &reply);
    if (reply) xpc_release(reply);
}
