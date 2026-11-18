/* A transport with no USB behind it, for the two-process tests.
 *
 * Implements the part of usb_engine.h the engine service uses, so the service
 * under test is the production one (driver/engine_service.c) and only the
 * hardware is pretend. The pretend device:
 *
 *   - runs on the host clock at exactly the nominal rate, one bus millisecond
 *     at a time, with the 44.1 kHz family's uneven packets (44, 44, ... 45);
 *   - anchors sample 0 a few milliseconds after start, as the real schedule
 *     does, and republishes (frames, host time) every two milliseconds;
 *   - loops playback straight back into capture with no latency: the frame
 *     transmitted at sample time N is captured at sample time N. So a client
 *     that reads back what it wrote, at the index it wrote it, has checked
 *     sample placement across the whole boundary;
 *   - goes through the production 24-bit packing in both directions;
 *   - transmits silence for a frame nobody wrote in time, and counts it.
 *
 * What is attached comes from the file named by EMU_FAKE_BUS, one unit per
 * line ("3f0a 14100000 SERIAL"), re-read when it changes: that is hot-plug.
 */

#include <dispatch/dispatch.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../driver/usb_engine.h"
#include "../../driver/ring.h"
#include "../../shared/usb_util.h"

#define START_LEAD_MS 4
#define BYTES_PER_FRAME 6

struct Engine {
    uint16_t product;
    uint64_t location;
    EmuRing* ring;
    EmuRing  own_ring;

    pthread_t    thread;
    bool         joinable;
    _Atomic bool running;
    _Atomic bool stop;
    uint32_t     rate;
    bool         with_input;

    _Atomic uint32_t gain_bits;
    _Atomic uint32_t fault;
    void (*failed)(void*);
    void*  failed_context;

    /* Playback as the device will see it: packed, indexed by frame. */
    uint8_t          store[EMU_RING_FRAMES * BYTES_PER_FRAME];
    _Atomic uint64_t frontier;          /* first frame the writer has not reached */
    _Atomic uint64_t played;
    _Atomic uint32_t books_seq;
    _Atomic uint64_t offered, refused, ahead;
    /* The books at the last reset; _Atomic only so two control threads may
     * touch them, each is written whole from one coherent reading. */
    _Atomic uint64_t base_offered, base_refused, base_ahead, base_bound, base_late;
    _Atomic uint64_t bound;
    _Atomic uint64_t late;              /* written after they were transmitted */
    _Atomic uint64_t underruns;         /* transmitted before they were written */
    _Atomic uint64_t write_lead_max;

    os_unfair_lock tl_lock;
    uint64_t tl_frames, tl_host;
};

/* --- the bus ---------------------------------------------------------------- */

static void (*gObserver)(void);
static EmuUnit gBus[4];
static unsigned gBusCount;
static os_unfair_lock gBusLock = OS_UNFAIR_LOCK_INIT;
static char gBusText[1024];

static bool read_bus(void)
{
    const char* path = getenv("EMU_FAKE_BUS");
    char text[sizeof gBusText] = "";
    FILE* f = path ? fopen(path, "r") : NULL;
    if (f) {
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose(f);
    }
    if (strcmp(text, gBusText) == 0) return false;

    EmuUnit units[4];
    unsigned n = 0;
    char copy[sizeof text];
    strlcpy(copy, text, sizeof copy);
    for (char* line = strtok(copy, "\n"); line && n < 4; line = strtok(NULL, "\n")) {
        unsigned product = 0;
        unsigned long long location = 0;
        char serial[80] = "";
        if (sscanf(line, "%x %llx %79s", &product, &location, serial) != 3) continue;
        const EmuDeviceIdentity* identity = emu_device_for_product((uint16_t)product);
        if (!identity) continue;
        memset(&units[n], 0, sizeof units[n]);
        units[n].identity = identity;
        units[n].location_id = location;
        strlcpy(units[n].serial, serial, sizeof units[n].serial);
        n++;
    }
    os_unfair_lock_lock(&gBusLock);
    memcpy(gBus, units, sizeof gBus);
    gBusCount = n;
    strlcpy(gBusText, text, sizeof gBusText);
    os_unfair_lock_unlock(&gBusLock);
    return true;
}

unsigned emu_enumerate_units(EmuUnit* out, unsigned max)
{
    os_unfair_lock_lock(&gBusLock);
    unsigned n = gBusCount < max ? gBusCount : max;
    memcpy(out, gBus, n * sizeof *out);
    os_unfair_lock_unlock(&gBusLock);
    return n;
}

bool emu_engine_set_identity_observer(void (*observer)(void))
{
    gObserver = observer;
    read_bus();
    dispatch_queue_t q = dispatch_queue_create("fake.hotplug", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 25 * NSEC_PER_MSEC,
                              5 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{ if (read_bus() && gObserver) gObserver(); });
    dispatch_resume(timer);
    return true;
}

/* --- the device ------------------------------------------------------------- */

static uint64_t ms_ticks(void)
{
    static uint64_t ticks;
    if (!ticks) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        ticks = 1000000ull * tb.denom / tb.numer;
    }
    return ticks;
}

static void publish(EmuEngine* e, uint64_t frames, uint64_t host)
{
    os_unfair_lock_lock(&e->tl_lock);
    e->tl_frames = frames;
    e->tl_host = host;
    os_unfair_lock_unlock(&e->tl_lock);
}

/* The real transport thread is real-time; an ordinary one in a background
 * daemon has its timers coalesced by milliseconds, and the device would then
 * be late in a way no device is. */
static void device_thread_policy(void)
{
    thread_time_constraint_policy_data_t policy;
    policy.period      = (uint32_t)ms_ticks();
    policy.computation = (uint32_t)(ms_ticks() / 8);
    policy.constraint  = (uint32_t)(ms_ticks() / 2);
    policy.preemptible = TRUE;
    thread_port_t thread = mach_thread_self();
    thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), thread);
}

static void* device_thread(void* arg)
{
    EmuEngine* e = (EmuEngine*)arg;
    device_thread_policy();
    uint64_t t0 = e->tl_host;                   /* bus time of sample 0 */
    uint64_t played = 0;
    uint32_t acc = 0;
    static const uint8_t kSilence[64 * BYTES_PER_FRAME];

    for (uint64_t ms = 1; !atomic_load(&e->stop); ms++) {
        mach_wait_until(t0 + ms * ms_ticks());

        /* This millisecond's packet: rate/1000 frames, the remainder carried,
         * which is 44 or 45 at 44.1 kHz and 176 or 177 at 176.4. */
        acc += e->rate;
        uint32_t n = acc / 1000;
        acc %= 1000;

        uint64_t frontier = atomic_load(&e->frontier);
        for (uint32_t done = 0; done < n; ) {
            uint64_t frame = played + done;
            uint32_t slot = (uint32_t)(frame & EMU_RING_MASK);
            uint32_t run = n - done;
            if (run > EMU_RING_FRAMES - slot) run = EMU_RING_FRAMES - slot;
            if (run > 64) run = 64;
            uint8_t* packet = &e->store[(size_t)slot * BYTES_PER_FRAME];
            if (e->with_input) emu_ring_write_s24(e->ring, frame, packet, run);
            /* Transmitted: what goes out the next time round is silence
             * unless someone writes it again. */
            memcpy(packet, kSilence, (size_t)run * BYTES_PER_FRAME);
            done += run;
        }
        if (frontier != 0 && played + n > frontier) {
            uint64_t base = played > frontier ? played : frontier;
            atomic_fetch_add(&e->underruns, played + n - base);
        }
        played += n;
        atomic_store(&e->played, played);
        if ((ms & 1) == 0) publish(e, played, t0 + ms * ms_ticks());

        /* A transport that gives up, the way the real one does after its
         * rebuild budget: stops on its own and says so, once. */
        if (atomic_load(&e->fault) == EMU_FAULT_PERSISTENT && ms >= 20) {
            atomic_store(&e->running, false);
            if (e->failed) e->failed(e->failed_context);
            return NULL;
        }
    }
    return NULL;
}

EmuEngine* emu_engine_create(uint16_t product_id, uint64_t location_id)
{
    EmuEngine* e = calloc(1, sizeof *e);
    e->product = product_id;
    e->location = location_id;
    e->ring = &e->own_ring;
    e->tl_lock = OS_UNFAIR_LOCK_INIT;
    atomic_store(&e->gain_bits, 0x3f800000u);
    return e;
}

void emu_engine_destroy(EmuEngine* e)
{
    if (!e) return;
    emu_engine_stop(e);
    free(e);
}

bool emu_engine_set_input_ring(EmuEngine* e, struct EmuRing* ring)
{
    if (e->joinable) return false;
    e->ring = ring ? ring : &e->own_ring;
    return true;
}

void emu_engine_set_failure_handler(EmuEngine* e, void (*handler)(void*), void* context)
{
    e->failed = handler;
    e->failed_context = context;
}

bool emu_engine_start(EmuEngine* e, uint32_t sample_rate, uint32_t output_safety_us, bool with_input)
{
    (void)output_safety_us;
    if (atomic_load(&e->running)) return true;
    if (e->joinable) { pthread_join(e->thread, NULL); e->joinable = false; }

    /* The one-shot startup fault is consumed by the start it disturbs. */
    uint32_t expected = EMU_FAULT_STARTUP_STALE;
    atomic_compare_exchange_strong(&e->fault, &expected, EMU_FAULT_NONE);

    e->rate = sample_rate;
    e->with_input = with_input;
    memset(e->store, 0, sizeof e->store);
    emu_ring_reset(e->ring);
    atomic_store(&e->frontier, 0);
    atomic_store(&e->played, 0);
    atomic_store(&e->bound, 0);
    atomic_store(&e->offered, 0);
    atomic_store(&e->ahead, 0);
    atomic_store(&e->late, 0);
    atomic_store(&e->base_offered, 0);
    atomic_store(&e->base_bound, 0);
    atomic_store(&e->base_ahead, 0);
    atomic_store(&e->base_late, 0);
    atomic_store(&e->underruns, 0);
    atomic_store(&e->write_lead_max, 0);
    atomic_store(&e->stop, false);

    /* The anchor exists before start returns, as the real engine promises. */
    publish(e, 0, mach_absolute_time() + START_LEAD_MS * ms_ticks());
    atomic_store(&e->running, true);
    if (pthread_create(&e->thread, NULL, device_thread, e) != 0) {
        atomic_store(&e->running, false);
        return false;
    }
    e->joinable = true;
    return true;
}

void emu_engine_stop(EmuEngine* e)
{
    if (!e || !e->joinable) return;
    atomic_store(&e->stop, true);
    pthread_join(e->thread, NULL);
    e->joinable = false;
    atomic_store(&e->running, false);
}

bool emu_engine_timeline(EmuEngine* e, uint64_t* frames, uint64_t* host_time)
{
    if (!atomic_load(&e->running)) return false;
    os_unfair_lock_lock(&e->tl_lock);
    *frames = e->tl_frames;
    *host_time = e->tl_host;
    os_unfair_lock_unlock(&e->tl_lock);
    return *host_time != 0;
}

/* The books as of one moment between writes. */
static void books(EmuEngine* e, uint64_t* offered, uint64_t* refused, uint64_t* bound,
                  uint64_t* ahead, uint64_t* late)
{
    for (int attempt = 0; attempt < 100000; attempt++) {
        uint32_t before = atomic_load(&e->books_seq);
        *offered = atomic_load(&e->offered);
        *refused = atomic_load(&e->refused);
        *bound   = atomic_load(&e->bound);
        *ahead   = atomic_load(&e->ahead);
        *late    = atomic_load(&e->late);
        if (!(before & 1u) && atomic_load(&e->books_seq) == before) return;
    }
}

void emu_engine_write_output(EmuEngine* e, const float* frames, uint32_t count, uint64_t sample_pos)
{
    /* The same books, kept the same way, as the real engine: see bind_seq in
     * usb_engine.c. */
    atomic_fetch_add(&e->books_seq, 1);
    if (!atomic_load(&e->running)) {
        atomic_fetch_add(&e->refused, count);
        atomic_fetch_add(&e->books_seq, 1);
        return;
    }
    float gain;
    uint32_t bits = atomic_load(&e->gain_bits);
    memcpy(&gain, &bits, sizeof gain);

    uint64_t played = atomic_load(&e->played);
    if (sample_pos + count > played && sample_pos + count - played > atomic_load(&e->write_lead_max))
        atomic_store(&e->write_lead_max, sample_pos + count - played);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t frame = sample_pos + i;
        if (frame < played) { atomic_fetch_add(&e->late, 1); continue; }
        if (frame - played >= EMU_RING_FRAMES) { atomic_fetch_add(&e->ahead, 1); continue; }
        emu_pack_s24(&e->store[(size_t)(frame & EMU_RING_MASK) * BYTES_PER_FRAME],
                     frames + (size_t)i * 2, 1, gain);
        atomic_fetch_add(&e->bound, 1);
    }
    if (sample_pos + count > atomic_load(&e->frontier)) atomic_store(&e->frontier, sample_pos + count);
    atomic_fetch_add(&e->offered, count);
    atomic_fetch_add(&e->books_seq, 1);
}

void emu_engine_set_output_gain(EmuEngine* e, float gain)
{
    uint32_t bits;
    memcpy(&bits, &gain, sizeof bits);
    atomic_store(&e->gain_bits, bits);
}

void emu_engine_stats(EmuEngine* e, EmuEngineStats* stats)
{
    memset(stats, 0, sizeof *stats);
    stats->frames_played    = atomic_load(&e->played);
    stats->frames_captured  = e->with_input ? stats->frames_played : 0;
    uint64_t offered, refused, bound, ahead, late;
    books(e, &offered, &refused, &bound, &ahead, &late);
    stats->frames_offered  = offered - atomic_load(&e->base_offered);
    stats->frames_refused  = refused - atomic_load(&e->base_refused);
    stats->frames_bound    = bound - atomic_load(&e->base_bound);
    stats->unmapped_ahead  = ahead - atomic_load(&e->base_ahead);
    stats->unmapped_frames = late - atomic_load(&e->base_late) + stats->unmapped_ahead;
    stats->underruns        = atomic_load(&e->underruns);
    stats->write_lead_max   = atomic_load(&e->write_lead_max);
    stats->input_enabled    = e->with_input;
    stats->input_underruns  = (uint32_t)atomic_load(&e->ring->missing);
    stats->input_overruns   = (uint32_t)atomic_load(&e->ring->discarded);
    stats->engine_streaming = atomic_load(&e->running) ? 1u : 0u;
    stats->fault_mode       = atomic_load(&e->fault);
    stats->schedule_requests = 81;
}

void emu_engine_reset_counters(EmuEngine* e)
{
    /* Rebased from one reading, never zeroed under a write: see bind_base
     * in usb_engine.c. */
    uint64_t offered, refused, bound, ahead, late;
    books(e, &offered, &refused, &bound, &ahead, &late);
    atomic_store(&e->base_offered, offered);
    atomic_store(&e->base_refused, refused);
    atomic_store(&e->base_bound, bound);
    atomic_store(&e->base_ahead, ahead);
    atomic_store(&e->base_late, late);
    atomic_store(&e->underruns, 0);
    atomic_store(&e->write_lead_max, 0);
    atomic_store(&e->ring->missing, 0);
    atomic_store(&e->ring->discarded, 0);
}

void emu_engine_inject_fault(EmuEngine* e, EmuFaultMode mode)
{
    atomic_store(&e->fault, (uint32_t)mode);
}
