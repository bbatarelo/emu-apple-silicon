/*
 * USB engine for the HAL plug-in: the Milestone 4 duplex transport.
 *
 * Capture goes through a timeline-indexed ring Core Audio addresses by sample
 * time; output needs no staging at all, because Core Audio converts its mix
 * straight into the USB request that carries those frames.
 *
 * Runs on its own thread with its own run loop. Core Audio's real-time thread
 * only ever calls emu_engine_write_output / emu_engine_read_input, which are
 * lock-free.
 */

#pragma once

/* Both halves log under one subsystem so a single predicate catches the lot;
 * the category separates them ("plugin" for the Core Audio surface, "engine"
 * for the USB transport). */
#define EMU_LOG_SUBSYSTEM "net.quantum-bit.EMUTrackerPre"

/* One attached device's transport. Opaque: the plug-in holds a handle per
 * device rather than the driver holding one device. */
typedef struct Engine EmuEngine;

#include <stdbool.h>
#include <stdint.h>

/* The zero-timestamp period the plug-in publishes, and the reason it is here
 * rather than private to plugin.c: the HAL derives the largest IO buffer it
 * will grant a client from it, empirically
 *
 *     maxBufferFrames = min(4096, ZeroTimeStampPeriod * 3/8)
 *
 * -- undocumented, but it fits every device on a test machine and three
 * predicted changes to this constant landed exactly (FINDINGS). The direct
 * bind has to schedule past where Core Audio writes, and where Core Audio
 * writes is set by that buffer size, so the two constants are coupled and
 * must not drift apart. The engine sizes its schedule from this.
 */
#define EMU_ZERO_TIMESTAMP_PERIOD 8192u

/* Ceiling on the output safety offset, and not merely a sanity bound: the
 * schedule is sized against *this* rather than the offset in force, because
 * the write lead depends on the offset coreaudiod has cached, which follows
 * a coreaudiod restart and nothing else (FINDINGS). Lower the offset at
 * runtime and the HAL keeps writing at the old, larger one; a schedule sized
 * for the new value would then be too short and drop the difference. Sizing
 * for the ceiling makes any cached value safe, at the price of carrying the
 * ceiling's feedback-servo lag always. */
#define EMU_OUTPUT_SAFETY_MAX_US 20000u

typedef struct {
    uint64_t frames_played;      /* frames the device has actually consumed  */
    uint64_t usb_errors;
    uint64_t timestamp_fallbacks;/* completions without a usable hardware timestamp */
    uint64_t timestamp_resets;   /* timeline discontinuities the filter snapped to */
    uint64_t resyncs;            /* bus-schedule rebuilds after the queue went stale */
    uint64_t dead_frames;        /* playback frames of bus time no packet covered: the
                                    summed length of every rebuild's dropout */
    uint64_t short_playback;     /* playback entries the bus called good and did not
                                    carry in full: frActCount short of frReqCount */
    uint32_t feedback_starved;
    uint32_t feedback_overflows; /* capture measurements the queue could not hold:
                                    playback and capture have decoupled */

    /* The explicit feedback endpoint, 0x81. The device states its own demand
     * there in Q16.16 sample frames per playback service interval, about every
     * 32 ms. It sizes playback only while capture is off; otherwise these are
     * a second, device-side reading of the same clock to check the first
     * against. Raw words as sent -- the scaling correction is applied where
     * the value is used, not where it is reported. */
    uint64_t feedback_packets;   /* values received                            */
    uint64_t feedback_silent;    /* intervals the endpoint had nothing to say  */
    uint64_t feedback_errors;
    uint64_t feedback_rejected;  /* values outside one frame of nominal, ignored */
    uint64_t feedback_changes;   /* consecutive values that differed           */
    uint32_t feedback_q16;       /* the last value                             */
    uint32_t feedback_min_q16;
    uint32_t feedback_max_q16;
    uint32_t feedback_nominal_q16; /* what the rate and interval say it should be */

    /* The output path. Core Audio writes into the submitted USB buffers
     * itself (emu_engine_write_output), so what these measure is that
     * thread's progress against the bus, not a staging buffer's occupancy. */
    uint64_t frames_bound;       /* frames converted straight into a USB buffer */
    uint64_t underruns;          /* frames that transmitted before Core Audio wrote them */
    uint64_t unfilled_playback;  /* requests that transmitted with any such frame */
    uint32_t output_lead;        /* how far Core Audio's writes lead the play head */
    uint64_t unmapped_frames;    /* frames Core Audio wrote that no queued request covered:
                                    a rebuild's dead interval, or a cycle past the
                                    schedule -- the latter means the schedule is too short */
    uint64_t unmapped_ahead;     /* of those, the ones past the end of the schedule */
    uint64_t write_lead_max;     /* high-water mark of how far past the play head Core
                                    Audio writes: what the schedule depth must exceed */
    uint64_t bind_races;         /* writes into a request recycled underneath them */
    uint32_t schedule_requests;  /* the schedule depth this session settled on */
    bool     schedule_clamped;   /* ...and whether MAX_REQUESTS truncated it, which
                                    means the schedule may be short of the write lead */

    bool     input_enabled;      /* whether capture was opened at all this session */
    uint64_t frames_captured;    /* frames written to the input ring, silence included */
    uint32_t input_depth;
    uint32_t input_underruns;
    uint32_t input_overruns;

    /* Health. The first is the counter whose absence cost a day and a half:
     * with the engine dead, Core Audio keeps calling DoIOOperation and every
     * other figure here keeps advancing, so nothing else distinguishes
     * "playing" from "playing into a void". */
    uint32_t engine_streaming;   /* 1 while transfers are on the bus */
    uint64_t recoveries;         /* stream rebuilds after a transport fault */
    uint64_t recovery_failures;  /* rebuilds that themselves failed */
    uint32_t fault_mode;         /* injected fault, 0 when none (testing) */
    uint64_t empty_capture;      /* capture intervals with nothing usable, written as silence;
                                    a couple at every start is the ADC spinning up */
} EmuEngineStats;

/* Whether a supported device is attached right now. Cheap and safe to call
 * from the property thread: presence is kept current by hot-plug notification,
 * not looked up per call. The plug-in publishes its Core Audio device only
 * while this is true, so an absent device is absent from the system's device
 * list rather than listed and unable to start. */
bool emu_engine_device_attached(void);

/* Name of the attached device, for Core Audio to publish. Same source as
 * emu_engine_device_attached. Only meaningful while a device is attached; the
 * answer with none is a placeholder, since the plug-in has nothing published
 * to name. */
const char* emu_engine_device_name(void);

/* The attached device's name, for log prefixes. Reads only what is already
 * known -- it never goes looking, so logging cannot have side effects -- and
 * answers "E-MU device" when nothing is attached. */
const char* emu_engine_log_name(void);

/* Registers a callback for the attached device changing -- arriving, leaving,
 * or being swapped for a sibling -- so the plug-in can tell Core Audio that
 * its device list, or the name of the device on it, is no longer right:
 * nothing re-queries a device on its own. Called on the engine's hot-plug
 * queue, not on any Core Audio thread, and never from inside a property
 * call.
 *
 * Also arms the hot-plug watch, and returns whether that worked. If not --
 * which takes the process being out of Mach ports or memory -- no device is
 * published and none will be: there is deliberately no look-up-once fallback,
 * because a device that appears but cannot be followed is the stale-device
 * problem over again. The plug-in should refuse to initialize, and say why. */
bool emu_engine_set_identity_observer(void (*observer)(void));

/* Brings the device up and starts both streams. Blocks until the streams are
 * scheduled on the bus and the timeline anchor is published — tens of
 * milliseconds — so that the first GetZeroTimeStamp already has the device's
 * clock and never has to guess from the host's. Returns false if the hardware
 * did not come up.
 *
 * `output_safety_us` is the output safety offset the plug-in publishes for
 * this session. The engine binds playback data exactly that far ahead of the
 * play head -- Core Audio's definition of the offset is how far ahead of the
 * hardware position it is safe to do IO, and the fill is this driver's
 * hardware position -- so the same number sets how late the engine thread may
 * run before a packet transmits silence: the offset less one request period.
 *
 * `with_input` false leaves the capture interface unclaimed at alternate
 * setting 0, so no IN transaction reaches the bus, and playback is sized from
 * the explicit feedback endpoint instead of from capture (planner_next in
 * usb_engine.c). */
/* One handle per attached device. Stage 1 permits exactly one; create returns
 * NULL if one is already outstanding. */
EmuEngine* emu_engine_create(uint16_t product_id, uint64_t location_id);
void       emu_engine_destroy(EmuEngine* engine);

bool     emu_engine_start(EmuEngine* engine, uint32_t sample_rate, uint32_t output_safety_us,
                          bool with_input);

void     emu_engine_stop(EmuEngine* engine);
bool     emu_engine_running(EmuEngine* engine);

/* Called from Core Audio's real-time thread. Lock-free, never blocks.
 * `sample_pos` is the IO cycle's sample time: frames on the same timeline that
 * GetZeroTimeStamp publishes, which is the timeline the rings are indexed by. */
void     emu_engine_write_output(EmuEngine* engine, const float* frames, uint32_t count, uint64_t sample_pos);
void     emu_engine_read_input(EmuEngine* engine, float* frames, uint32_t count, uint64_t sample_pos);

/* Linear amplitude, 0.0 to 1.0. Applied to the output stream, because this
 * device has no hardware master level. */
void     emu_engine_set_output_gain(EmuEngine* engine, float gain);

/* Frames the device has consumed. Core Audio's timeline anchors to this, so it
 * follows the device's clock rather than the host's. */
uint64_t emu_engine_frames_played(EmuEngine* engine);

/* Frames the device has consumed and the host time at which that was true, as a
 * consistent pair. Published before emu_engine_start returns — initially the
 * scheduled bus time of the first packet, then refreshed by every completed
 * request. The pair stays on one straight line for the life of the session:
 * a stall that forces the bus schedule to be rebuilt is accounted as frames
 * the device consumed while no packet reached it, not as a pause of its
 * clock, so the plug-in never has to declare a new timeline. False only if the
 * engine is not running. */
bool     emu_engine_timeline(EmuEngine* engine, uint64_t* frames, uint64_t* host_time);

/* Zeroes read-only counters. Leaves frames_played alone, since the timeline
 * derives from it and must never go backwards. */
void     emu_engine_reset_counters(EmuEngine* engine);

/* While the engine runs: live counters. After it stops: the final counters of
 * the last session, kept so a post-mortem `make check` still has evidence. */
void     emu_engine_stats(EmuEngine* engine, EmuEngineStats* stats);

/* True while transfers are actually on the bus. Distinct from
 * emu_engine_running, which only says a start was requested and no stop has
 * arrived: between a transport fault and a successful rebuild the engine is
 * running but not streaming, and that is exactly the state that used to be
 * invisible from outside. */
bool     emu_engine_streaming(EmuEngine* engine);

/*
 * Called on the engine thread when the engine has exhausted its rebuild
 * attempts. The plug-in answers by marking the device not alive, which is the
 * only way to tell Core Audio to stop handing audio to a transport that is no
 * longer there. Runs with no locks held and must not call emu_engine_stop,
 * which would join the thread it is running on.
 */
void     emu_engine_set_failure_handler(EmuEngine* engine,
                                        void (*handler)(void* context),
                                        void* context);

/*
 * Fault injection, so the recovery path can be exercised without unplugging
 * anything. TRANSIENT fails the next few submissions and the engine should
 * rebuild through it; PERSISTENT fails every submission until cleared and
 * should drive it out to the failure handler. It lives in the engine because
 * the fault it simulates is a submission the USB stack refuses, and that is
 * the only place it can be observed.
 */
typedef enum {
    EMU_FAULT_NONE       = 0,
    EMU_FAULT_TRANSIENT  = 1,
    EMU_FAULT_PERSISTENT = 2,
} EmuFaultMode;

void     emu_engine_inject_fault(EmuEngine* engine, EmuFaultMode mode);
