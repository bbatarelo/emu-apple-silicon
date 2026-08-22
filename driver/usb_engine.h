/*
 * USB engine for the HAL plug-in: the Milestone 4 duplex transport, fed by a
 * ring that Core Audio writes into.
 *
 * Runs on its own thread with its own run loop. Core Audio's real-time thread
 * only ever calls emu_engine_write_output, which is lock-free.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t frames_played;      /* frames the device has actually consumed  */
    uint64_t underruns;          /* frames the ring could not supply         */
    uint64_t overruns;           /* frames the ring could not accept         */
    uint64_t usb_errors;
    uint32_t ring_depth;         /* output frames currently buffered         */
    uint32_t feedback_starved;

    uint64_t frames_captured;
    uint32_t input_depth;
    uint32_t input_underruns;
    uint32_t input_overruns;     /* grows while nothing is recording, by design */
} EmuEngineStats;

/* Name of the attached device, for Core Audio to publish. Resolved on demand,
 * because the plug-in is asked long before anything opens the device and
 * possibly before one is plugged in at all. */
const char* emu_engine_device_name(void);

bool     emu_engine_start(uint32_t sample_rate);
void     emu_engine_stop(void);
bool     emu_engine_running(void);

/* Called from Core Audio's real-time thread. Lock-free, never blocks. */
void     emu_engine_write_output(const float* frames, uint32_t count);

/* Linear amplitude, 0.0 to 1.0. Applied to the output stream, because this
 * device has no hardware master level. */
void     emu_engine_set_output_gain(float gain);

/* Called from Core Audio's real-time thread. Fills silence when the engine is
 * not running or the ring is short. */
void     emu_engine_read_input(float* frames, uint32_t count);

/* Frames the device has consumed. Core Audio's timeline anchors to this, so it
 * follows the device's clock rather than the host's. */
uint64_t emu_engine_frames_played(void);

/* Frames the device has consumed and the host time at which that was true, as a
 * consistent pair. False until the first transfer has completed. */
bool     emu_engine_timeline(uint64_t* frames, uint64_t* host_time);

/* Zeroes read-only counters. Leaves frames_played alone, since the timeline
 * derives from it and must never go backwards. */
void     emu_engine_reset_counters(void);

void     emu_engine_stats(EmuEngineStats* stats);
