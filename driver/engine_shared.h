/*
 * The boundary between the resident HAL and the replaceable engine process,
 * for the optional two-part build (docs/SPLIT-DRIVER-PLAN.md). The one-part
 * release driver does not include this file.
 *
 * Two channels, kept apart on purpose:
 *
 *   - Control is XPC: hello, open, start, stop, reset, fault, close, and the
 *     engine's own events (units changed, session failed). It may wait, with a
 *     bound, and it never runs on a Core Audio IO thread.
 *   - Audio and the clock are one region of shared memory per unit, created
 *     by the HAL and mapped by the engine. The IO thread touches nothing else:
 *     no message, no lock, no wait, and every index it follows is masked, so
 *     nothing the other process writes can send it outside the region.
 *
 * What crosses, and who writes it:
 *
 *   playback   The HAL appends each WriteMix's frames to `play` and queues a
 *              record of where they are and which sample time they are for.
 *              The engine's pump thread takes records in order and hands the
 *              same frames to emu_engine_write_output -- the call Core Audio
 *              makes directly in the one-part build -- so the bind map, its
 *              seqlock, its gate and its counters are the one-part ones,
 *              reached one pump interval later. That interval is the cost of
 *              the split and is measured (EmuPumpStats).
 *
 *              `play` is a FIFO, not a timeline: frames sit where they were
 *              appended, and the HAL refuses a write that would reach frames
 *              the pump has not finished with. So a record's samples are the
 *              ones published for its sample time for as long as the record
 *              exists, whatever sample times Core Audio names and however
 *              long the pump stalls; what does not fit is dropped whole,
 *              before anything is bound, and counted.
 *   capture    The engine writes the shared `capture` ring from its completion
 *              handler exactly as it writes its own; the HAL reads it by sample
 *              time. No extra thread, no extra deadline.
 *   timeline   The engine's (frames, host time) anchor, republished here under
 *              a seqlock. mach_absolute_time is one clock for both processes.
 *   stats      A periodic snapshot of EmuEngineStats, so diagnostics never
 *              wait on the engine and outlive it.
 *
 * Addresses never cross. The engine's USB buffers stay its own.
 *
 * The region belongs to the HAL, which never unmaps it: an IO thread may hold
 * the pointer at any moment, and the engine can die at any moment, so the only
 * safe lifetime is the host process's. The engine can leave a seqlock odd by
 * dying inside a publication; readers are bounded and say so rather than spin.
 */

#pragma once

#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ring.h"
#include "usb_engine.h"

/* The engine's Mach service, and its launchd label. Its own name: nothing
 * else on the machine -- including other drivers' development engines --
 * answers to it. */
#define EMU_ENGINE_SERVICE "net.batarelo.EMUTrackerPre.Engine"

/* Bumped for any change to the region layout, EmuEngineStats, or the meaning
 * of a control message. A mismatch is refused at hello and at open; nothing is
 * reinterpreted. */
#define EMU_ENGINE_ABI 1u

#define EMU_SHARED_MAGIC UINT64_C(0x31304d4853554d45)   /* "EMUSHM01" */

/* The most one WriteMix may carry. The HAL grants at most 3072 frames on this
 * device (EMU_ZERO_TIMESTAMP_PERIOD); anything larger is refused, not split. */
#define EMU_SHARED_MAX_IO 4096u

/* Write records in flight. One per WriteMix, popped every pump interval, so
 * the steady depth is 0 or 1; the rest is room for a pump stall. */
#define EMU_WRITE_QUEUE      256u
#define EMU_WRITE_QUEUE_MASK (EMU_WRITE_QUEUE - 1u)

/* How often the engine's pump looks for new playback. This is the deadline the
 * split adds inside the output safety offset: a frame Core Audio wrote reaches
 * its USB request up to this much (plus the pump's wake jitter) later than in
 * the one-part build. */
#define EMU_PUMP_INTERVAL_US 500u

/* A session's engine is considered gone when its pump has not run for this
 * long. Far above any scheduling stall; it catches a hung process, which no
 * XPC error reports. */
#define EMU_HEARTBEAT_TIMEOUT_MS 2000u

typedef struct {
    uint64_t pos;       /* first frame, on the device timeline */
    uint64_t host;      /* mach time the HAL finished copying it */
    uint64_t at;        /* where its frames start in `play`: frames appended before it */
    uint32_t count;     /* frames, <= EMU_SHARED_MAX_IO */
    uint32_t reserved;
} EmuWriteRecord;

enum { EMU_SHARED_IDLE = 0, EMU_SHARED_STREAMING = 1 };

/* What the pump saw. Engine-written, plain atomics: diagnostics only. */
#define EMU_PUMP_DELAY_BUCKETS 8u
typedef struct {
    _Atomic uint64_t passes;
    _Atomic uint64_t records;           /* WriteMix calls forwarded */
    _Atomic uint64_t frames;            /* ...and the frames in them */
    _Atomic uint64_t invalid;           /* records refused: bad count */
    _Atomic uint64_t reserved_count;    /* was: records lapped before the copy; cannot happen */
    _Atomic uint64_t resyncs;           /* queue indices found inconsistent */
    _Atomic uint32_t queue_depth_max;
    _Atomic uint32_t reserved;
    /* Wake error: how late a pass started against its schedule. */
    _Atomic uint64_t wake_late_max_ns;
    /* Copy delay: HAL finished writing -> engine began binding. The whole
     * added deadline, per record. Buckets are <125 us, <250, <500, <1 ms,
     * <2, <4, <8, and the rest. */
    _Atomic uint64_t copy_delay_max_ns;
    _Atomic uint64_t copy_delay[EMU_PUMP_DELAY_BUCKETS];
} EmuPumpStats;

typedef struct {
    /* --- fixed at creation by the HAL; validated by the engine at open ---- */
    uint64_t magic;
    uint32_t abi;
    uint32_t ring_frames;
    uint32_t channels;
    uint32_t queue_slots;
    uint32_t stats_bytes;
    uint32_t reserved0;
    uint64_t total_bytes;

    /* --- HAL-written ------------------------------------------------------ */
    /* Which session the region currently serves. The HAL bumps it before each
     * start; a stop, or anything else, carrying another value is stale. */
    _Alignas(64) _Atomic uint64_t generation;
    _Atomic uint32_t gain_bits;         /* linear output gain, a float's bits */
    _Atomic uint32_t reserved1;
    _Atomic uint64_t q_head;            /* next record the HAL will write */
    _Atomic uint64_t pcm_head;          /* frames the HAL has appended to `play`, ever */
    _Atomic uint64_t hal_dropped;       /* writes refused: no record slot or no room in `play` */
    _Atomic uint64_t hal_dropped_frames;/* ...and the frames in them */
    _Atomic uint64_t hal_oversize;      /* writes refused: > EMU_SHARED_MAX_IO */

    /* --- engine-written --------------------------------------------------- */
    _Alignas(64) _Atomic uint64_t q_tail;   /* first record the pump has not finished with */
    _Atomic uint64_t pcm_tail;          /* frames of `play` the pump has finished with, ever */
    _Atomic uint32_t engine_state;      /* EMU_SHARED_IDLE / _STREAMING */
    _Atomic uint32_t tl_seq;            /* even: stable */
    _Atomic uint64_t engine_generation; /* the generation engine_state is about */
    _Atomic uint64_t tl_frames;
    _Atomic uint64_t tl_host;
    _Atomic uint64_t heartbeat;         /* mach time of the pump's last pass */
    _Atomic uint32_t stats_seq;         /* even: stable */
    _Atomic uint32_t stats_count;       /* snapshots published; 0 = none yet */
    EmuEngineStats   stats;
    EmuPumpStats     pump;

    /* --- records and PCM -------------------------------------------------- */
    _Alignas(64) EmuWriteRecord queue[EMU_WRITE_QUEUE];
    /* Interleaved stereo Float32, appended: the frame appended n-th sits at
     * slot n mod EMU_RING_FRAMES. Valid between pcm_tail and pcm_head.
     * HAL-written. */
    _Alignas(64) float play[EMU_RING_FRAMES * EMU_RING_CHANNELS];
    /* Engine-written, HAL-read; the HAL's erase head and its consumed/missing
     * counters are the reader's, as in the one-part build. */
    _Alignas(64) EmuRing capture;
} EmuShared;

/* --- region ---------------------------------------------------------------- */

static inline void emu_shared_init(EmuShared* s, size_t bytes)
{
    memset(s, 0, sizeof *s);
    s->magic       = EMU_SHARED_MAGIC;
    s->abi         = EMU_ENGINE_ABI;
    s->ring_frames = EMU_RING_FRAMES;
    s->channels    = EMU_RING_CHANNELS;
    s->queue_slots = EMU_WRITE_QUEUE;
    s->stats_bytes = (uint32_t)sizeof(EmuEngineStats);
    s->total_bytes = bytes;
    atomic_store_explicit(&s->gain_bits, 0x3f800000u, memory_order_relaxed);   /* 1.0f */
}

/* The engine's check before it trusts a mapping it was handed. */
static inline bool emu_shared_valid(const EmuShared* s, size_t mapped_bytes)
{
    return s && mapped_bytes >= sizeof *s
        && s->magic == EMU_SHARED_MAGIC && s->abi == EMU_ENGINE_ABI
        && s->ring_frames == EMU_RING_FRAMES && s->channels == EMU_RING_CHANNELS
        && s->queue_slots == EMU_WRITE_QUEUE
        && s->stats_bytes == sizeof(EmuEngineStats)
        && s->total_bytes >= sizeof *s && s->total_bytes <= mapped_bytes;
}

/* --- playback: HAL side (Core Audio's IO thread) --------------------------- */

static inline void emu_shared_write(EmuShared* s, const float* frames, uint32_t count, uint64_t pos)
{
    if (count == 0) return;
    if (count > EMU_SHARED_MAX_IO) {
        atomic_fetch_add_explicit(&s->hal_oversize, 1, memory_order_relaxed);
        return;
    }
    uint64_t head     = atomic_load_explicit(&s->q_head, memory_order_relaxed);
    uint64_t tail     = atomic_load_explicit(&s->q_tail, memory_order_acquire);
    uint64_t pcm_head = atomic_load_explicit(&s->pcm_head, memory_order_relaxed);
    uint64_t pcm_tail = atomic_load_explicit(&s->pcm_tail, memory_order_acquire);
    /* No record slot, no room for the frames without reaching ones the pump
     * still owns, or indices the engine left somewhere impossible: there is
     * nowhere this write may go. Dropped whole and counted, before any byte
     * is copied; the frames go out as the zeros their request was submitted
     * with. Unsigned differences, so a tail ahead of its head reads as huge
     * and lands here too. */
    if (head - tail >= EMU_WRITE_QUEUE || pcm_head - pcm_tail > EMU_RING_FRAMES - count) {
        atomic_fetch_add_explicit(&s->hal_dropped, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s->hal_dropped_frames, count, memory_order_relaxed);
        return;
    }

    uint32_t slot  = (uint32_t)(pcm_head & EMU_RING_MASK);
    uint32_t first = EMU_RING_FRAMES - slot;
    if (first > count) first = count;
    memcpy(&s->play[(size_t)slot * EMU_RING_CHANNELS], frames,
           (size_t)first * EMU_RING_CHANNELS * sizeof(float));
    if (first < count) {
        memcpy(&s->play[0], frames + (size_t)first * EMU_RING_CHANNELS,
               (size_t)(count - first) * EMU_RING_CHANNELS * sizeof(float));
    }

    EmuWriteRecord* r = &s->queue[head & EMU_WRITE_QUEUE_MASK];
    r->pos   = pos;
    r->host  = mach_absolute_time();
    r->at    = pcm_head;
    r->count = count;
    atomic_store_explicit(&s->pcm_head, pcm_head + count, memory_order_relaxed);
    atomic_store_explicit(&s->q_head, head + 1, memory_order_release);
}

/* --- playback: engine side (the pump thread) -------------------------------- */

/* Looks at the oldest record without giving it up: its frames stay the
 * pump's, and the HAL will not write over them, until emu_shared_done. False
 * when there is none. A record that cannot be what the HAL wrote -- a count
 * out of range -- is discarded here and never reaches the caller; whatever a
 * record says about where its frames are is only ever used masked. */
static inline bool emu_shared_peek(EmuShared* s, EmuWriteRecord* out)
{
    for (;;) {
        uint64_t tail = atomic_load_explicit(&s->q_tail, memory_order_relaxed);
        uint64_t head = atomic_load_explicit(&s->q_head, memory_order_acquire);
        if (head == tail) return false;
        uint64_t pcm_head = atomic_load_explicit(&s->pcm_head, memory_order_relaxed);
        if (head - tail > EMU_WRITE_QUEUE) {
            /* Not a state the two sides can reach together. Start again from
             * where the HAL is rather than read slots it may be rewriting. */
            atomic_store_explicit(&s->pcm_tail, pcm_head, memory_order_release);
            atomic_store_explicit(&s->q_tail, head, memory_order_release);
            atomic_fetch_add_explicit(&s->pump.resyncs, 1, memory_order_relaxed);
            return false;
        }
        *out = s->queue[tail & EMU_WRITE_QUEUE_MASK];
        if (out->count != 0 && out->count <= EMU_SHARED_MAX_IO) return true;

        /* Skipped, and its length is not believed either: the frames given
         * back run up to where the next record's begin, or to everything
         * appended if it was the last. */
        atomic_fetch_add_explicit(&s->pump.invalid, 1, memory_order_relaxed);
        uint64_t resume = tail + 1 == head ? pcm_head
                                           : s->queue[(tail + 1) & EMU_WRITE_QUEUE_MASK].at;
        atomic_store_explicit(&s->pcm_tail, resume, memory_order_release);
        atomic_store_explicit(&s->q_tail, tail + 1, memory_order_release);
    }
}

/* The record emu_shared_peek returned has been bound: its frames and its
 * slot go back to the HAL. */
static inline void emu_shared_done(EmuShared* s, const EmuWriteRecord* r)
{
    atomic_store_explicit(&s->pcm_tail, r->at + r->count, memory_order_release);
    atomic_store_explicit(&s->q_tail,
                          atomic_load_explicit(&s->q_tail, memory_order_relaxed) + 1,
                          memory_order_release);
}

/* Empties the queue without binding anything: a new session's start. Engine
 * side, with no pump running. */
static inline void emu_shared_discard(EmuShared* s)
{
    atomic_store_explicit(&s->pcm_tail, atomic_load_explicit(&s->pcm_head, memory_order_acquire),
                          memory_order_release);
    atomic_store_explicit(&s->q_tail, atomic_load_explicit(&s->q_head, memory_order_acquire),
                          memory_order_release);
}

/* --- timeline --------------------------------------------------------------- */

static inline void emu_shared_publish_timeline(EmuShared* s, uint64_t frames, uint64_t host)
{
    /* `| 1` rather than `+ 1`: a predecessor that died mid-publication left
     * the sequence odd, and this writer must not make it look stable. */
    uint32_t seq = atomic_load_explicit(&s->tl_seq, memory_order_relaxed) | 1u;
    atomic_store_explicit(&s->tl_seq, seq, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->tl_frames, frames, memory_order_relaxed);
    atomic_store_explicit(&s->tl_host, host, memory_order_relaxed);
    atomic_store_explicit(&s->tl_seq, seq + 1, memory_order_release);
}

/* Bounded: false if the writer is mid-publication for all of the attempts,
 * which is also what a writer that died there looks like. */
static inline bool emu_shared_read_timeline(EmuShared* s, uint64_t* frames, uint64_t* host)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t before = atomic_load_explicit(&s->tl_seq, memory_order_acquire);
        if (before & 1u) continue;
        uint64_t f = atomic_load_explicit(&s->tl_frames, memory_order_relaxed);
        uint64_t h = atomic_load_explicit(&s->tl_host, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->tl_seq, memory_order_relaxed) == before) {
            if (h == 0) return false;
            *frames = f;
            *host = h;
            return true;
        }
    }
    return false;
}

/* --- stats ------------------------------------------------------------------ */

static inline void emu_shared_publish_stats(EmuShared* s, const EmuEngineStats* stats)
{
    uint32_t seq = atomic_load_explicit(&s->stats_seq, memory_order_relaxed) | 1u;
    atomic_store_explicit(&s->stats_seq, seq, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    memcpy(&s->stats, stats, sizeof *stats);
    atomic_fetch_add_explicit(&s->stats_count, 1, memory_order_relaxed);
    atomic_store_explicit(&s->stats_seq, seq + 1, memory_order_release);
}

static inline bool emu_shared_read_stats(EmuShared* s, EmuEngineStats* out)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t before = atomic_load_explicit(&s->stats_seq, memory_order_acquire);
        if (before & 1u) continue;
        if (atomic_load_explicit(&s->stats_count, memory_order_relaxed) == 0) return false;
        memcpy(out, &s->stats, sizeof *out);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->stats_seq, memory_order_relaxed) == before) return true;
    }
    return false;
}
