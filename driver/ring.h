/*
 * Timeline-indexed circular buffer of interleaved stereo Float32 frames: the
 * join between the capture stream and Core Audio.
 *
 * Its defining property is that it is *not* a FIFO. Both sides address it by
 * absolute frame index on the device's sample timeline,
 * `slot = frame mod EMU_RING_FRAMES`: the USB engine writes at the frame it
 * just received, Core Audio reads at the sample time its IO cycle names. The
 * phase between the two is therefore fixed by the timeline itself -- the
 * safety offset the driver publishes -- not by whichever side happened to
 * start first.
 *
 * A FIFO would derive the phase from arrival order instead, and every
 * underrun would slip it permanently: unbounded latency growth after any
 * glitch, and a burst of crackle at each stream start while the ring "found"
 * a workable phase (see FINDINGS). Timeline indexing is how IOAudioFamily's
 * sample buffers work; an underrun there is one silent packet, not a regime
 * change.
 *
 * The consumer zeroes every slot behind it (the erase head, again from
 * IOAudioFamily). A slot the producer never reaches therefore plays silence,
 * not a stale lap of audio.
 *
 * Output does not come through here at all: Core Audio writes its mix
 * straight into the submitted USB request buffers, so there is nothing to
 * stage. The packing helpers below are shared with that path, which is the
 * only reason they live in this header.
 *
 * Lock-free: the producer alone advances `frontier`, the consumer alone
 * advances `consumed`, with release/acquire ordering. Under fault the two may
 * touch the same slot; the torn frames land inside an already-glitching
 * stretch, which is the same trade IOAudioFamily makes.
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
    /* First frame index the producer has not yet written. */
    _Atomic uint64_t frontier;
    /* First frame index the consumer has not yet read, for depth diagnostics. */
    _Atomic uint64_t consumed;
    /* Diagnostics, not control flow. A driver that hides these is unfixable. */
    _Atomic uint64_t missing;     /* frames consumed before the producer wrote them */
    _Atomic uint64_t discarded;   /* producer writes rejected as off-timeline */
} EmuRing;

static inline void emu_ring_reset(EmuRing* ring)
{
    atomic_store_explicit(&ring->frontier, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->consumed, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->missing, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->discarded, 0, memory_order_relaxed);
    memset(ring->data, 0, sizeof ring->data);
}

/* How far the producer's writes lead the consumer's reads, in frames. The
 * steady-state value is the driver's buffered latency; zero means the consumer
 * is about to read slots nobody filled. */
static inline uint32_t emu_ring_depth(const EmuRing* ring)
{
    uint64_t f = atomic_load_explicit(&ring->frontier, memory_order_acquire);
    uint64_t c = atomic_load_explicit(&ring->consumed, memory_order_acquire);
    return f > c ? (uint32_t)(f - c) : 0;
}

/* Sanity bound for producer positions. A position a full lap behind the
 * frontier is not a timeline any more; writing there would corrupt audio that
 * has not played yet. Nothing legitimate produces it — Core Audio's resync
 * jumps are a few periods at most — so it is dropped and counted. */
static inline bool emu_ring_pos_ok(EmuRing* ring, uint64_t pos)
{
    uint64_t f = atomic_load_explicit(&ring->frontier, memory_order_relaxed);
    if (f > EMU_RING_FRAMES && pos < f - EMU_RING_FRAMES) return false;
    return true;
}

static inline void emu_ring_advance_frontier(EmuRing* ring, uint64_t end)
{
    uint64_t f = atomic_load_explicit(&ring->frontier, memory_order_relaxed);
    if (end > f) {
        atomic_store_explicit(&ring->frontier, end, memory_order_release);
    }
}

/* Producer, 24-bit packed little-endian in: the capture stream, at the
 * engine's capture cursor. */
static inline void emu_ring_write_s24(EmuRing* ring, uint64_t pos,
                                      const uint8_t* src, uint32_t count)
{
    if (!emu_ring_pos_ok(ring, pos)) {
        atomic_fetch_add_explicit(&ring->discarded, count, memory_order_relaxed);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (uint32_t)((pos + i) & EMU_RING_MASK) * EMU_RING_CHANNELS;
        for (uint32_t ch = 0; ch < EMU_RING_CHANNELS; ch++) {
            const uint8_t* p = src + (i * EMU_RING_CHANNELS + ch) * 3;
            /* Sign-extend 24 bits into 32 before scaling. */
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (v & 0x800000) v |= (int32_t)0xff000000;
            ring->data[slot + ch] = (float)v / 8388608.0f;
        }
    }
    emu_ring_advance_frontier(ring, pos + count);
}

/* Producer, silence: a capture interval that brought nothing usable -- an
 * errored or empty packet. Written as zeros rather than skipped so the input
 * keeps its place on the timeline and the slot cannot hand back the previous
 * lap: the erase head only runs while something is reading. */
static inline void emu_ring_write_silence(EmuRing* ring, uint64_t pos, uint32_t count)
{
    if (!emu_ring_pos_ok(ring, pos)) {
        atomic_fetch_add_explicit(&ring->discarded, count, memory_order_relaxed);
        return;
    }
    /* Beyond a lap the zeroing only repeats itself; the frontier still moves
     * by the full count. */
    uint32_t n = count > EMU_RING_FRAMES ? EMU_RING_FRAMES : count;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = (uint32_t)((pos + i) & EMU_RING_MASK) * EMU_RING_CHANNELS;
        ring->data[slot]     = 0.0f;
        ring->data[slot + 1] = 0.0f;
    }
    emu_ring_advance_frontier(ring, pos + count);
}

/* Frames in [pos, pos+count) the producer has not written yet. Not counted
 * before the producer's first write: the engine legitimately reads ahead of
 * Core Audio's first cycle at stream start, and those slots are silence by
 * design, not a glitch. */
static inline void emu_ring_count_missing(EmuRing* ring, uint64_t pos, uint32_t count)
{
    uint64_t f = atomic_load_explicit(&ring->frontier, memory_order_acquire);
    if (f == 0) return;
    if (pos + count > f) {
        uint64_t base = pos > f ? pos : f;
        atomic_fetch_add_explicit(&ring->missing, pos + count - base,
                                  memory_order_relaxed);
    }
}

/* Consumer, Float32 out: Core Audio's ReadInput, at the cycle's sample time.
 * Read slots are zeroed behind the erase head. */
static inline void emu_ring_read_f32(EmuRing* ring, uint64_t pos,
                                     float* dst, uint32_t count)
{
    emu_ring_count_missing(ring, pos, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (uint32_t)((pos + i) & EMU_RING_MASK) * EMU_RING_CHANNELS;
        dst[i * EMU_RING_CHANNELS]     = ring->data[slot];
        dst[i * EMU_RING_CHANNELS + 1] = ring->data[slot + 1];
        ring->data[slot]     = 0.0f;
        ring->data[slot + 1] = 0.0f;
    }
    atomic_store_explicit(&ring->consumed, pos + count, memory_order_release);
}

/* One Float32 sample to 24-bit packed little-endian: the only output format
 * any of this device's alternate settings offer. */
static inline void emu_pack_sample_s24(uint8_t* p, float sample)
{
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    int32_t v = (int32_t)(sample * 8388607.0f);
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
}

/* Interleaved Float32 stereo straight to packed 24-bit, with gain and
 * clipping: the output path, where Core Audio's own mix buffer is the source
 * and a submitted USB request's buffer is the destination. Converted in place
 * rather than through a staging buffer, on Core Audio's IO thread. */
static inline void emu_pack_s24(uint8_t* dst, const float* src, uint32_t count, float gain)
{
    for (uint32_t i = 0; i < count * EMU_RING_CHANNELS; i++) {
        emu_pack_sample_s24(dst + i * 3, src[i] * gain);
    }
}
