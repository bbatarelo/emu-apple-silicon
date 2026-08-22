/*
 * Single-producer single-consumer ring of interleaved stereo Float32 frames.
 *
 * This is the join between two clocks that do not agree. Core Audio delivers
 * fixed 512-frame buffers on the host clock; USB consumes variable packets --
 * 44 or 45 frames at 44.1 kHz -- on the device clock. Neither side may block or
 * allocate, so the ring absorbs the difference and reports when it cannot.
 *
 * Lock-free by construction: the producer only advances the write index and the
 * consumer only advances the read index, both with release/acquire ordering.
 * Capacity is a power of two so the wrap is a mask rather than a division.
 */

#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define EMU_RING_FRAMES   32768u          /* ~680 ms at 48 kHz, ~170 ms at 192 */
#define EMU_RING_MASK     (EMU_RING_FRAMES - 1u)
#define EMU_RING_CHANNELS 2u

typedef struct {
    float data[EMU_RING_FRAMES * EMU_RING_CHANNELS];
    _Atomic uint64_t write;
    _Atomic uint64_t read;
    /* Diagnostics, not control flow. A driver that hides these is unfixable. */
    _Atomic uint64_t underruns;   /* consumer wanted frames that were not there */
    _Atomic uint64_t overruns;    /* producer had frames the ring could not hold */
} EmuRing;

static inline void emu_ring_reset(EmuRing* ring)
{
    atomic_store_explicit(&ring->write, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->read, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->underruns, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->overruns, 0, memory_order_relaxed);
    memset(ring->data, 0, sizeof ring->data);
}

static inline uint32_t emu_ring_filled(const EmuRing* ring)
{
    uint64_t w = atomic_load_explicit(&ring->write, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&ring->read, memory_order_acquire);
    return (uint32_t)(w - r);
}

/* Producer side. Drops the oldest data rather than blocking when full: in an
 * audio path, arriving late is worse than arriving incomplete. */
static inline void emu_ring_write(EmuRing* ring, const float* frames, uint32_t count)
{
    uint64_t w = atomic_load_explicit(&ring->write, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&ring->read, memory_order_acquire);

    uint32_t space = EMU_RING_FRAMES - (uint32_t)(w - r);
    if (count > space) {
        atomic_fetch_add_explicit(&ring->overruns, count - space, memory_order_relaxed);
        count = space;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (uint32_t)((w + i) & EMU_RING_MASK) * EMU_RING_CHANNELS;
        ring->data[slot]     = frames[i * EMU_RING_CHANNELS];
        ring->data[slot + 1] = frames[i * EMU_RING_CHANNELS + 1];
    }
    atomic_store_explicit(&ring->write, w + count, memory_order_release);
}

/*
 * Capture producer: 24-bit packed little-endian in, Float32 out.
 *
 * Drops the newest frames when full, exactly like the playback direction.
 *
 * An earlier version dropped the *oldest* by advancing the read index, so that
 * an idle ring would not go stale. That is a data race: in a single-producer
 * single-consumer ring only the consumer may write `read`, and having both
 * sides move it tore the samples badly enough to read as full-scale noise on a
 * disconnected input. Staleness is the consumer's problem to solve, and
 * emu_ring_read_f32 solves it by skipping a backlog it alone owns.
 */
static inline void emu_ring_write_s24(EmuRing* ring, const uint8_t* src, uint32_t count)
{
    uint64_t w = atomic_load_explicit(&ring->write, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&ring->read, memory_order_acquire);

    uint32_t space = EMU_RING_FRAMES - (uint32_t)(w - r);
    if (count > space) {
        atomic_fetch_add_explicit(&ring->overruns, count - space, memory_order_relaxed);
        count = space;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (uint32_t)((w + i) & EMU_RING_MASK) * EMU_RING_CHANNELS;
        for (uint32_t ch = 0; ch < EMU_RING_CHANNELS; ch++) {
            const uint8_t* p = src + (i * EMU_RING_CHANNELS + ch) * 3;
            /* Sign-extend 24 bits into 32 before scaling. */
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (v & 0x800000) v |= (int32_t)0xff000000;
            ring->data[slot + ch] = (float)v / 8388608.0f;
        }
    }
    atomic_store_explicit(&ring->write, w + count, memory_order_release);
}

/* Most input backlog worth keeping, in frames. Beyond this the consumer is
 * being handed audio that is older than the latency anyone would accept, which
 * happens whenever nothing was recording for a while. */
#define EMU_INPUT_MAX_BACKLOG  4096u
#define EMU_INPUT_KEEP_BACKLOG 1024u

/* Capture consumer, on Core Audio's real-time thread. Missing frames become
 * silence and are counted.
 *
 * Also owns discarding stale backlog. The consumer is the only side allowed to
 * move `read`, so this is where it has to happen -- doing it in the producer is
 * what corrupted the stream before. */
static inline uint32_t emu_ring_read_f32(EmuRing* ring, float* dst, uint32_t count)
{
    uint64_t r = atomic_load_explicit(&ring->read, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&ring->write, memory_order_acquire);

    if ((uint32_t)(w - r) > EMU_INPUT_MAX_BACKLOG) {
        uint64_t target = w - EMU_INPUT_KEEP_BACKLOG;
        atomic_fetch_add_explicit(&ring->overruns, (uint32_t)(target - r),
                                  memory_order_relaxed);
        r = target;
    }

    uint32_t available = (uint32_t)(w - r);
    uint32_t taken = count < available ? count : available;

    for (uint32_t i = 0; i < taken; i++) {
        uint32_t slot = (uint32_t)((r + i) & EMU_RING_MASK) * EMU_RING_CHANNELS;
        dst[i * EMU_RING_CHANNELS]     = ring->data[slot];
        dst[i * EMU_RING_CHANNELS + 1] = ring->data[slot + 1];
    }
    atomic_store_explicit(&ring->read, r + taken, memory_order_release);

    if (taken < count) {
        atomic_fetch_add_explicit(&ring->underruns, count - taken, memory_order_relaxed);
        memset(dst + (size_t)taken * EMU_RING_CHANNELS, 0,
               (size_t)(count - taken) * EMU_RING_CHANNELS * sizeof(float));
    }
    return taken;
}

/*
 * Consumer side, writing straight out as 24-bit packed little-endian -- the
 * only format any of this device's alternate settings offer.
 *
 * Converts during the copy rather than in a staging buffer, because this runs
 * on the USB completion path where an extra pass and an extra buffer both cost
 * more than they are worth. Missing frames become silence, and are counted.
 */
static inline uint32_t emu_ring_read_s24(EmuRing* ring, uint8_t* dst, uint32_t count,
                                         float gain)
{
    uint64_t r = atomic_load_explicit(&ring->read, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&ring->write, memory_order_acquire);

    uint32_t available = (uint32_t)(w - r);
    uint32_t taken = count < available ? count : available;

    for (uint32_t i = 0; i < taken; i++) {
        uint32_t slot = (uint32_t)((r + i) & EMU_RING_MASK) * EMU_RING_CHANNELS;
        for (uint32_t ch = 0; ch < EMU_RING_CHANNELS; ch++) {
            /* Gain is applied here rather than when Core Audio hands us the
             * buffer, so a volume change takes effect on the next packet
             * instead of after everything already queued has drained. */
            float sample = ring->data[slot + ch] * gain;
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            int32_t v = (int32_t)(sample * 8388607.0f);
            uint8_t* p = dst + (i * EMU_RING_CHANNELS + ch) * 3;
            p[0] = (uint8_t)(v & 0xff);
            p[1] = (uint8_t)((v >> 8) & 0xff);
            p[2] = (uint8_t)((v >> 16) & 0xff);
        }
    }
    atomic_store_explicit(&ring->read, r + taken, memory_order_release);

    if (taken < count) {
        atomic_fetch_add_explicit(&ring->underruns, count - taken, memory_order_relaxed);
        memset(dst + (size_t)taken * EMU_RING_CHANNELS * 3, 0,
               (size_t)(count - taken) * EMU_RING_CHANNELS * 3);
    }
    return taken;
}
