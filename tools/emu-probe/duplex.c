/*
 * Milestone 4 -- internally duplex USB engine.
 *
 * Runs capture and playback together. Capture is the clock reference: the
 * sample-frame count observed on each capture service interval sizes the next
 * playback packet, through the feedback queue in the Rust core. That is the
 * known-working E-MU behaviour (guidelines section 17), and it is why capture
 * runs even when only playback is wanted.
 *
 * Everything scheduling-related mirrors capture.c, including the lessons that
 * cost real debugging: underrun is a normal status, one frame-list entry is one
 * service interval rather than one millisecond, and the clock rate must be set
 * and verified before any alternate setting is selected.
 */

#include "duplex.h"
#include "../../shared/usb_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>

#define NUM_REQUESTS        8
/* Requests in flight scale inversely with their length, so that varying
 * request_ms varies only the granularity and leaves the depth of the schedule
 * alone. Eight 8 ms requests plan 64 ms ahead; so do sixty-four 1 ms ones.
 * Without this, req=1 left 8 ms of schedule, which the engine cannot stay
 * ahead of: 48 kHz -- a rate that never crackles -- went to 17 resyncs and
 * +594 ppm, which would have swamped whatever granularity does. */
#define MAX_REQUESTS        64
#define SCHEDULE_DEPTH_MS   (REQUEST_MS * NUM_REQUESTS)
/* Default milliseconds of audio per request; DuplexConfig::request_ms
 * overrides it. Entries per request depend on how often the endpoint is
 * serviced, so the array is sized for the densest case. */
#define REQUEST_MS          8
#define MAX_ENTRIES         (REQUEST_MS * 8)
#define SCHEDULE_MARGIN     16

/* Raw feedback words kept for the report. The value is expected to be nearly
 * constant, so a handful of the first ones plus the extremes says more than a
 * long log would. */
#define FB_SAMPLES          16

/* Skips the device's startup phase-alignment ramp before reporting steady-state
 * numbers. See docs/milestone-3-results.md. */
#define SETTLE_MS           2000.0

typedef struct DuplexCtx DuplexCtx;

typedef struct {
    DuplexCtx*      ctx;
    void*           buffer;
    IOUSBLowLatencyIsocFrame* frames;
    uint64_t        frame_start;
    bool            playback;
} Request;

struct DuplexCtx {
    IOUSBInterfaceInterface500** in_intf;
    IOUSBInterfaceInterface500** out_intf;

    uint8_t  in_pipe,  out_pipe;
    uint16_t in_max,   out_max;

    /* The explicit feedback endpoint, 0x81, on the playback interface. Every
     * playback alt setting advertises it and the vendor's Windows driver sizes
     * playback packets from it; this engine sizes them from capture instead,
     * so reading it here is a measurement of the device's own demand against
     * what capture implies it should be. `fb_pipe` is 0 when it is absent. */
    uint8_t  fb_pipe;
    uint16_t fb_max;
    uint8_t  fb_interval;
    uint32_t fb_entries_per_request;
    uint64_t next_fb_frame;

    /* Playback with interface 2 never opened, and playback with it opened and
     * streaming but never read. Either way no capture measurement arrives, so
     * the feedback endpoint sizes the packets because nothing else can. */
    bool     playback_only;
    bool     capture_idle;
    bool     size_from_device;
    /* Bus frames the playback schedule starts after the capture one. */
    uint32_t sync_delay_ms;
    /* Poll one capture interval in this many; 0 and 1 both mean all of them.
     * Done by covering only the first `capture_entries` intervals of each
     * request window and advancing the schedule over the whole of it, so the
     * rest of the window has no IN transfer against it at all. Asking for a
     * zero-length entry instead does not work -- the stack errors every entry
     * in the request, polled ones included. */
    uint32_t capture_duty;
    uint32_t capture_entries;
    /* The last feedback value that passed the band check, and the Q16.16
     * residue of accumulating it. Only playback_only reads them. */
    uint32_t fb_plan_q16;
    uint32_t fb_residue_q16;
    bool     fb_raw;
    /* What the rate and the service interval say a packet should hold, exact
     * in Q16.16: the fractional rates cannot be judged against a truncated
     * integer. Also the centre of the band a feedback value must land in. */
    uint32_t fb_nominal_q16;

    uint32_t bytes_per_frame;
    double   interval_ms;
    uint32_t nominal_frames;

    /* Service intervals of audio to put in one frame-list entry. At bInterval 3
     * the endpoint is serviced twice per USB frame but the classic isoc API
     * gives one entry per frame, so 1 delivers only half the required audio.
     * Setting 2 tests whether the stack will split a larger entry across both
     * microframe transactions. */
    /* Frame-list entries per millisecond. With the low-latency API an entry is
     * one service interval, so this is 8 / period_microframes: 2 at
     * bInterval 3, 1 at bInterval 4. The classic API gave one entry per USB
     * frame regardless, which silently delivered half the audio at bInterval 3.
     */
    uint32_t entries_per_ms;
    uint32_t entries_per_request;
    /* Milliseconds of audio per request, and so the stride between one
     * request's first bus frame and the next's. */
    uint32_t request_ms;

    uint64_t next_in_frame;
    uint64_t next_out_frame;

    Request in_requests[MAX_REQUESTS];
    Request out_requests[MAX_REQUESTS];
    Request fb_requests[MAX_REQUESTS];
    /* SCHEDULE_DEPTH_MS / request_ms, so the schedule stays 64 ms deep. */
    uint32_t num_requests;

    /* Feedback queue lives in the Rust core; this is caller-provided storage
     * for it, so the engine performs no allocation on the streaming path. */
    _Alignas(16) uint8_t feedback_storage[2048];
    EmuFeedback* feedback;

    /* Sine generator state. Phase is kept in double and wrapped, so it does not
     * lose precision over a long run. */
    double phase;
    double phase_increment;
    double amplitude;

    uint64_t intervals_elapsed;
    uint64_t settle_intervals;

    /* Steady-state statistics, gathered after settling. */
    uint64_t in_packets,  out_packets;
    uint64_t in_frames,   out_frames;
    /* Intervals capture was asked about, and those that answered with nothing.
     * Without these, in_frames / in_packets is frames per *delivered packet*
     * and an interval the device skipped leaves both terms, so the rate it
     * implies reads high by exactly the amount that went missing. */
    uint64_t in_intervals, in_empty;
    uint32_t in_errors,   out_errors;
    uint32_t out_short;         /* device accepted fewer bytes than offered */
    uint32_t max_queue_depth;
    uint32_t resyncs;

    /* Playback diagnostics. Byte counters said the data went out while nothing
     * was audible, so record what the entries actually reported and what was
     * in the buffer we handed over. */
    int32_t  out_status_value[8];
    uint32_t out_status_count[8];
    uint32_t out_status_n;
    uint16_t first_out_req, first_out_act;
    uint8_t  first_out_bytes[12];
    bool     have_first_out;

    /* Explicit feedback, as it arrives. Nothing here steers the stream: the
     * point is to find out whether the endpoint speaks at all, in what units,
     * and whether it agrees with the capture packet lengths the planner uses. */
    uint64_t fb_packets;        /* entries that carried a value              */
    uint64_t fb_empty;          /* good status, no bytes -- device said nothing */
    uint32_t fb_errors;
    uint32_t fb_short;          /* carried something other than 4 bytes      */
    uint64_t fb_raw_sum;        /* sum of the raw Q16.16 words               */
    uint32_t fb_raw_min, fb_raw_max, fb_raw_last;
    uint32_t fb_changes;        /* consecutive words that differed           */
    uint32_t fb_sample[FB_SAMPLES];
    uint32_t fb_sample_n;

    /* Every value with the host time it arrived at, for the trace. Sized for
     * a long run at the fastest cadence and simply stops recording when full,
     * so the streaming path never allocates. */
    const char* fb_trace_path;
    uint64_t*   fb_log_time;
    uint32_t*   fb_log_raw;
    uint32_t    fb_log_n, fb_log_cap;
    uint64_t    fb_log_start;
    double      ticks_per_sec;

    /* The four bytes that lead every capture packet at bInterval 3. E-MU's
     * Windows driver reads the first ULONG of each IN packet as the packet's
     * own total length and steps past it; the driver here infers the same
     * offset from frActCount % bytes_per_frame without ever reading the word.
     * Both readings are recorded so a disagreement -- the case that header
     * exists to survive -- cannot pass unnoticed. */
    uint64_t hdr_packets;       /* capture packets that carried a lead word  */
    uint64_t hdr_agree;         /* ...whose word equalled frActCount         */
    uint32_t hdr_sample_hdr[FB_SAMPLES];
    uint32_t hdr_sample_act[FB_SAMPLES];
    uint32_t hdr_sample_n;

    bool stopping;
};

/* ------------------------------------------------------------------ audio */

static void write_sample(uint8_t* p, double value, double amplitude)
{
    int32_t v = (int32_t)(value * amplitude * 8388607.0); /* 2^23 - 1 */
    if (v > 8388607) v = 8388607;
    if (v < -8388608) v = -8388608;
    /* 24-bit signed little-endian, matching every alt setting on this device. */
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
}

static void fill_tone(DuplexCtx* ctx, uint8_t* dst, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        double s = sin(ctx->phase);
        ctx->phase += ctx->phase_increment;
        if (ctx->phase >= 2.0 * M_PI) ctx->phase -= 2.0 * M_PI;

        write_sample(dst + (size_t)i * 6 + 0, s, ctx->amplitude); /* left  */
        write_sample(dst + (size_t)i * 6 + 3, s, ctx->amplitude); /* right */
    }
}

/* ------------------------------------------------------------- scheduling */

static void capture_complete(void* refcon, IOReturn result, void* arg0);
static void playback_complete(void* refcon, IOReturn result, void* arg0);
static void feedback_complete(void* refcon, IOReturn result, void* arg0);

/* Queues one request on the explicit feedback endpoint. It is polled on its
 * own bInterval -- 4 on this device, so one entry per millisecond even at the
 * rates whose data endpoint is serviced twice as often -- and carries four
 * bytes each time. */
static IOReturn submit_feedback(Request* req)
{
    DuplexCtx* ctx = req->ctx;

    for (uint32_t i = 0; i < ctx->fb_entries_per_request; i++) {
        req->frames[i].frStatus   = kUSBLowLatencyIsochTransferKey;
        req->frames[i].frReqCount = ctx->fb_max;
        req->frames[i].frActCount = 0;
    }

    req->frame_start = ctx->next_fb_frame;
    ctx->next_fb_frame += ctx->request_ms;

    return (*ctx->out_intf)->LowLatencyReadIsochPipeAsync(
        ctx->out_intf, ctx->fb_pipe, req->buffer,
        req->frame_start, ctx->fb_entries_per_request, 1, req->frames,
        feedback_complete, req);
}

static IOReturn submit_capture(Request* req)
{
    DuplexCtx* ctx = req->ctx;

    for (uint32_t i = 0; i < ctx->capture_entries; i++) {
        /* The API stamps this key and overwrites it as entries land. */
        req->frames[i].frStatus   = kUSBLowLatencyIsochTransferKey;
        req->frames[i].frReqCount = ctx->in_max;
        req->frames[i].frActCount = 0;
    }

    /* The transfer covers the first capture_entries intervals of the window
     * and the schedule steps over the whole window regardless, so the
     * remainder carries no IN transfer and no token goes out for it. */
    req->frame_start = ctx->next_in_frame;
    ctx->next_in_frame += ctx->request_ms;

    return (*ctx->in_intf)->LowLatencyReadIsochPipeAsync(
        ctx->in_intf, ctx->in_pipe, req->buffer,
        req->frame_start, ctx->capture_entries, 1, req->frames,
        capture_complete, req);
}

/*
 * How many frames the next playback entry carries, with no capture to ask.
 *
 * The device states its demand as a fraction, and a stream of whole packets
 * has to average out to it, so the remainder is carried between entries --
 * the same running-fraction arithmetic E-MU's Windows driver does. Until the
 * first value arrives the exact fractional nominal stands in, which is what
 * an empty feedback queue already draws.
 */
static uint32_t playback_only_frames(DuplexCtx* ctx)
{
    if (ctx->fb_plan_q16 == 0) {
        return emu_feedback_next(ctx->feedback, ctx->nominal_frames);
    }
    /* The device's fixed-point scaling, corrected at the point of use. `fbraw`
     * turns it off so the two can be compared by ear: uncorrected, the 44.1
     * family under-delivers 53.1 ppm. */
    uint32_t plan = ctx->fb_raw ? ctx->fb_plan_q16
                                : emu_feedback_true_q16(ctx->fb_plan_q16);
    uint32_t acc = ctx->fb_residue_q16 + plan;
    ctx->fb_residue_q16 = acc & 0xffffu;
    return acc >> 16;
}

static IOReturn submit_playback(Request* req)
{
    DuplexCtx* ctx = req->ctx;

    /* The isochronous data buffer is contiguous: frame i's payload begins where
     * frame i-1's frReqCount ended, not at a fixed wMaxPacketSize stride. Using
     * a stride leaves a gap of (wMaxPacketSize - packet bytes) before every
     * frame after the first, which the device reads as audio -- 10 bytes per
     * millisecond at 48 kHz. It still sounds tonal, because most of each packet
     * is correct, but every frame boundary is shifted. */
    size_t offset = 0;

    for (uint32_t i = 0; i < ctx->entries_per_request; i++) {
        /* The planner: capture's measurement of the device's clock decides how
         * much audio this interval carries. */
        /* Pop once per entry and scale, rather than once per packed interval.
         * Capture observes one transaction per frame, so it pushes one
         * measurement per entry; popping several would drain the queue faster
         * than it fills and fall back to the truncated nominal. At 176.4 kHz
         * that fallback is 88 where the true average is 88.2, which measured as
         * -1131 ppm. One measurement already describes the device's rate; it
         * only needs scaling to the entry's duration. */
        /* One entry is one service interval, so one capture measurement sizes
         * exactly one playback entry. Supply and demand stay balanced. */
        uint32_t frames = ctx->size_from_device
                              ? playback_only_frames(ctx)
                              : emu_feedback_next(ctx->feedback, ctx->nominal_frames);
        uint32_t bytes = emu_output_packet_bytes(frames, ctx->bytes_per_frame);

        while (bytes > ctx->out_max && frames > 0) {
            frames--;
            bytes = emu_output_packet_bytes(frames, ctx->bytes_per_frame);
        }

        /* Exactly frames * bytes_per_frame. The 4-byte excess that capture
         * reports at bInterval 3 is not part of the playback format: sending
         * frames*6 + 4 produces no audio at all, which is how that hypothesis
         * was disproven. */
        fill_tone(ctx, (uint8_t*)req->buffer + offset, frames);
        offset += bytes;

        req->frames[i].frStatus   = kUSBLowLatencyIsochTransferKey;
        req->frames[i].frReqCount = (UInt16)bytes;
        req->frames[i].frActCount = 0;
    }

    req->frame_start = ctx->next_out_frame;
    ctx->next_out_frame += ctx->request_ms;

    return (*ctx->out_intf)->LowLatencyWriteIsochPipeAsync(
        ctx->out_intf, ctx->out_pipe, req->buffer,
        req->frame_start, ctx->entries_per_request, 1, req->frames,
        playback_complete, req);
}

static void resync(DuplexCtx* ctx)
{
    UInt64 now = 0;
    AbsoluteTime at;
    /* Through the playback interface: it is the one that is always open, and
     * the frame number belongs to the bus rather than to either of them. */
    if ((*ctx->out_intf)->GetBusFrameNumber(ctx->out_intf, &now, &at) == kIOReturnSuccess) {
        ctx->next_in_frame  = now + SCHEDULE_MARGIN;
        ctx->next_out_frame = now + SCHEDULE_MARGIN + ctx->sync_delay_ms;
        ctx->next_fb_frame  = now + SCHEDULE_MARGIN;
        ctx->resyncs++;
    }
}

static void capture_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    Request* req = (Request*)refcon;
    DuplexCtx* ctx = req->ctx;

    if (result == kIOReturnIsoTooOld) resync(ctx);

    bool settled = ctx->intervals_elapsed >= ctx->settle_intervals;

    for (uint32_t i = 0; i < ctx->capture_entries; i++) {
        const IOUSBLowLatencyIsocFrame* f = &req->frames[i];
        /* Playback counts the intervals when the device is sizing the packets,
         * so capture must not count them a second time. */
        if (!ctx->size_from_device) ctx->intervals_elapsed++;
        if (settled && !ctx->stopping) ctx->in_intervals++;

        if (!emu_frame_ok(f->frStatus)) {
            if (settled && !ctx->stopping) ctx->in_errors++;
            continue;
        }
        if (f->frActCount == 0) {
            /* An interval that answered with nothing still passed on the
             * device's clock. Counted, not skipped. */
            if (settled && !ctx->stopping) ctx->in_empty++;
            continue;
        }

        uint32_t frames = emu_frames_in_packet(f->frActCount, ctx->bytes_per_frame);

        /* Where the frames begin is already settled by the remainder; what the
         * bytes in front of them say has never been read. Windows treats the
         * word as this packet's own total length, so it should equal
         * frActCount -- and where it does not, the host's byte count and the
         * device's disagree, which is worth knowing whichever one is right. */
        if (f->frActCount % ctx->bytes_per_frame != 0) {
            const uint8_t* p = (const uint8_t*)req->buffer + (size_t)i * ctx->in_max;
            uint32_t hdr = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            ctx->hdr_packets++;
            if (hdr == f->frActCount) ctx->hdr_agree++;
            else if (ctx->hdr_sample_n < FB_SAMPLES) {
                ctx->hdr_sample_hdr[ctx->hdr_sample_n] = hdr;
                ctx->hdr_sample_act[ctx->hdr_sample_n] = f->frActCount;
                ctx->hdr_sample_n++;
            }
        }

        /* Feed the planner even while settling: playback needs it from the
         * first interval, and only the statistics wait. Not when the device is
         * sizing the packets: nothing pops the queue then, and a thinned
         * capture stream would fill it and report an overflow that means
         * nothing. */
        if (!ctx->size_from_device) emu_feedback_push(ctx->feedback, frames);

        /* `!stopping` as well, and not only `settled`: the interval, empty and
         * error counters beside this one already carry it, so without it the
         * requests still in flight when AbortPipe drains them contribute their
         * frames to the numerator and nothing to the denominator. That reads
         * as the device running fast -- +1337 ppm on a 48 kHz run whose every
         * packet was exactly 48 frames, which is the same size of error a rate
         * measurement here exists to resolve. */
        if (settled && !ctx->stopping) {
            ctx->in_packets++;
            ctx->in_frames += frames;
        }
    }

    uint32_t depth = emu_feedback_depth(ctx->feedback);
    if (settled && depth > ctx->max_queue_depth) ctx->max_queue_depth = depth;

    if (ctx->stopping) {
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    if (submit_capture(req) != kIOReturnSuccess) {
        resync(ctx);
        if (submit_capture(req) != kIOReturnSuccess) {
            ctx->stopping = true;
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
    }
}

static void playback_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    Request* req = (Request*)refcon;
    DuplexCtx* ctx = req->ctx;

    if (result == kIOReturnIsoTooOld) resync(ctx);

    bool settled = ctx->intervals_elapsed >= ctx->settle_intervals;

    for (uint32_t i = 0; i < ctx->entries_per_request; i++) {
        const IOUSBLowLatencyIsocFrame* f = &req->frames[i];

        /* Capture counts the service intervals that have passed, because it is
         * the side that sees them all. With no capture, playback is the only
         * side there is -- and without this the settle window never elapses
         * and the run reports nothing at all. */
        if (ctx->size_from_device) ctx->intervals_elapsed++;

        /* Record every distinct status, whether or not it counts as an error,
         * so a status that is silently "fine" cannot hide. */
        bool seen = false;
        for (uint32_t k = 0; k < ctx->out_status_n; k++) {
            if (ctx->out_status_value[k] == f->frStatus) { ctx->out_status_count[k]++; seen = true; break; }
        }
        if (!seen && ctx->out_status_n < 8) {
            ctx->out_status_value[ctx->out_status_n] = f->frStatus;
            ctx->out_status_count[ctx->out_status_n] = 1;
            ctx->out_status_n++;
        }

        if (!ctx->have_first_out && f->frReqCount > 0) {
            ctx->first_out_req = f->frReqCount;
            ctx->first_out_act = f->frActCount;
            memcpy(ctx->first_out_bytes, req->buffer, sizeof ctx->first_out_bytes);
            ctx->have_first_out = true;
        }

        if (!emu_frame_ok(f->frStatus)) {
            if (settled && !ctx->stopping) ctx->out_errors++;
            continue;
        }
        if (!settled || ctx->stopping) continue;

        ctx->out_packets++;
        ctx->out_frames += emu_frames_in_packet(f->frActCount, ctx->bytes_per_frame);

        /* The device taking fewer bytes than offered means playback is
         * outrunning it -- the audible failure this engine exists to avoid. */
        if (f->frActCount < f->frReqCount) ctx->out_short++;
    }

    if (ctx->stopping) return;

    if (submit_playback(req) != kIOReturnSuccess) {
        resync(ctx);
        if (submit_playback(req) != kIOReturnSuccess) {
            ctx->stopping = true;
        }
    }
}

/*
 * What the device asks for, as opposed to what capture says it took.
 *
 * The word is little-endian Q16.16. E-MU's Windows driver reads it as sample
 * frames per *packet interval* of the data endpoint -- Whole = value >> 16,
 * assigned straight to the per-interval packet size -- while the same vendor's
 * macOS kext reads the identical endpoint as frames per *millisecond* and
 * multiplies by 1000 to get a sample rate. At bInterval 4 the two agree; at
 * bInterval 3, where the crackle lives, they differ by a factor of two. Rather
 * than pick one, both readings are reported and the rate they imply is
 * compared with the rate the device was set to, which settles it from the
 * hardware instead of from the documentation.
 */
static void feedback_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    Request* req = (Request*)refcon;
    DuplexCtx* ctx = req->ctx;

    if (result == kIOReturnIsoTooOld) resync(ctx);

    /* The device's output FIFO starts empty and is filled by the first
     * requests, so its servo spends the opening of every stream asking for
     * less than the true rate while it drains back to target. Averaging
     * through that reports a demand far below the clock, which is the priming
     * transient rather than the device's steady request -- the same reason the
     * rate statistics wait. */
    bool settled = ctx->intervals_elapsed >= ctx->settle_intervals;

    for (uint32_t i = 0; i < ctx->fb_entries_per_request; i++) {
        const IOUSBLowLatencyIsocFrame* f = &req->frames[i];

        if (!emu_frame_ok(f->frStatus)) {
            if (!ctx->stopping) ctx->fb_errors++;
            continue;
        }
        /* Silence from an asynchronous feedback endpoint is legal and common:
         * the device answers when it has something to say. Counted separately
         * so "the endpoint is dead" and "the endpoint is quiet" stay apart. */
        if (f->frActCount == 0) { if (settled) ctx->fb_empty++; continue; }
        if (f->frActCount != 4) { if (settled) ctx->fb_short++; continue; }

        /* Entries are laid out by frReqCount, exactly as on the capture pipe. */
        const uint8_t* p = (const uint8_t*)req->buffer + (size_t)i * ctx->fb_max;
        uint32_t raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);

        if (ctx->fb_packets == 0) {
            ctx->fb_raw_min = ctx->fb_raw_max = raw;
        } else {
            if (raw < ctx->fb_raw_min) ctx->fb_raw_min = raw;
            if (raw > ctx->fb_raw_max) ctx->fb_raw_max = raw;
            if (raw != ctx->fb_raw_last) ctx->fb_changes++;
        }
        /* Out-of-band values are refused rather than clamped: this endpoint
         * returns nonsense through a poisoned stream like everything else,
         * and in playback_only it is sizing real packets.
         *
         * The planner is fed whether or not the statistics have settled --
         * playback needs a size from its first interval, and a settle window's
         * worth of nominal fallback is a settle window of not doing what this
         * mode exists to do. Only the numbers below wait. */
        uint32_t lo = ctx->fb_nominal_q16 > 65536u ? ctx->fb_nominal_q16 - 65536u : 0;
        if (raw >= lo && raw <= ctx->fb_nominal_q16 + 65536u) ctx->fb_plan_q16 = raw;

        /* Traced from the first value, ahead of the settle gate: the onset
         * this exists to locate can fall inside the settle window, and a
         * trace that starts two seconds in cannot say that it did not. */
        if (ctx->fb_log_n < ctx->fb_log_cap) {
            ctx->fb_log_time[ctx->fb_log_n] = mach_absolute_time();
            ctx->fb_log_raw[ctx->fb_log_n] = raw;
            ctx->fb_log_n++;
        }

        if (!settled) continue;

        ctx->fb_raw_last = raw;
        ctx->fb_raw_sum += raw;
        ctx->fb_packets++;
        if (ctx->fb_sample_n < FB_SAMPLES) ctx->fb_sample[ctx->fb_sample_n++] = raw;
    }

    if (ctx->stopping) return;

    if (submit_feedback(req) != kIOReturnSuccess) {
        resync(ctx);
        if (submit_feedback(req) != kIOReturnSuccess) ctx->stopping = true;
    }
}

/* ---------------------------------------------------------------- helpers */

/* Selects the alternate setting for `rate` on `interface`, going through alt 0
 * first so the transition always starts from a known state. */
static bool select_alt(IOUSBInterfaceInterface500** intf,
                       const EmuDeviceModel* model,
                       uint8_t interface_number,
                       uint32_t rate,
                       bool short_interval,
                       const EmuAltSetting** chosen_out)
{
    const EmuAltSetting* chosen = NULL;
    for (uint16_t i = 0; i < model->num_alt_settings; i++) {
        const EmuAltSetting* a = &model->alt_settings[i];
        if (a->interface_number != interface_number) continue;
        if (a->data_endpoint == 0) continue;
        if (a->sample_rate != rate) continue;
        /* fill_tone writes a stereo frame, so the rate alone is not enough to
         * pick an alt setting on a device that also offers four channels at
         * the same rate. */
        if (a->channels != 2) continue;
        if (!chosen) chosen = a;
        else if (short_interval ? (a->interval < chosen->interval)
                                : (a->interval > chosen->interval)) chosen = a;
    }
    if (!chosen) {
        fprintf(stderr, "error: interface %u has no stereo alt setting for %u Hz\n",
                interface_number, rate);
        return false;
    }

    (*intf)->SetAlternateInterface(intf, 0);

    IOReturn kr = (*intf)->SetAlternateInterface(intf, chosen->alternate_setting);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: interface %u SetAlternateInterface(%u): 0x%08x\n",
                interface_number, chosen->alternate_setting, kr);
        if (kr == kIOReturnTimeout) {
            fprintf(stderr,
                "       The device did not answer SET_INTERFACE. The active clock\n"
                "       rate probably disagrees with this alt setting. A device left\n"
                "       in this state stops answering control transfers entirely and\n"
                "       needs a physical replug.\n");
        } else if (kr == kIOReturnNoBandwidth) {
            fprintf(stderr,
                "       Not enough isochronous bandwidth for both directions. Try a\n"
                "       lower rate, or connect the device directly rather than\n"
                "       through a hub.\n");
        }
        return false;
    }

    *chosen_out = chosen;
    return true;
}

static void report(const DuplexCtx* ctx, const DuplexConfig* cfg)
{
    printf("\n%s results (after %.0f ms settling)\n",
           ctx->playback_only ? "Playback-only"
                              : ctx->capture_idle ? "Capture-idle" : "Duplex",
           SETTLE_MS);

    if (ctx->out_packets == 0) {
        printf("  no playback packets completed\n");
        return;
    }

    /* With the low-latency API an entry is one service interval on both sides,
     * so the same conversion applies to each. */
    /* A thinned capture stream is a sample of the device's clock, not a
     * measurement of it: the FIFO backs up between polls and the packets that
     * are read come back long, so its rate reads high by however much was
     * skipped. Reported, never judged. */
    bool have_capture = ctx->in_packets > 0;
    bool capture_is_clock = have_capture && !ctx->size_from_device;
    /* Per elapsed interval, not per delivered packet: an interval that
     * brought nothing is still an interval of the device's clock, and
     * dividing it out is how a slow stream reads as a fast one. */
    double in_per_packet = have_capture
        ? (double)ctx->in_frames / (double)ctx->in_packets : 0.0;
    double in_per_interval = ctx->in_intervals
        ? (double)ctx->in_frames / (double)ctx->in_intervals : 0.0;
    double out_per_entry    = (double)ctx->out_frames / (double)ctx->out_packets;
    double out_entry_ms     = ctx->interval_ms;

    double in_hz  = in_per_interval / ctx->interval_ms * 1000.0;
    double out_hz = out_per_entry   / out_entry_ms     * 1000.0;
    double tracking_ppm = have_capture ? (out_hz - in_hz) / in_hz * 1e6 : 0.0;

    if (have_capture) {
        printf("  capture   %llu packets over %llu intervals, %llu frames\n",
               (unsigned long long)ctx->in_packets,
               (unsigned long long)ctx->in_intervals,
               (unsigned long long)ctx->in_frames);
        printf("            %.4f frames/interval, %.4f per delivered packet, "
               "%llu empty\n", in_per_interval, in_per_packet,
               (unsigned long long)ctx->in_empty);
    }
    printf("  playback  %llu packets, %llu frames, %.4f frames/entry (%.2f ms)\n",
           (unsigned long long)ctx->out_packets,
           (unsigned long long)ctx->out_frames, out_per_entry, out_entry_ms);

    if (have_capture) {
        printf("\n  capture rate    %.2f Hz (%+.1f ppm)\n", in_hz,
               (in_hz - cfg->sample_rate) / cfg->sample_rate * 1e6);
    } else {
        printf("\n");
    }
    printf("  playback rate   %.2f Hz (%+.1f ppm)\n", out_hz,
           (out_hz - cfg->sample_rate) / cfg->sample_rate * 1e6);

    /* The property that matters in duplex: playback must track capture,
     * because capture is the device's own clock. Drift between them is
     * unbounded error. With no capture there is nothing to track and the
     * playback rate above is the whole of what the host can say -- the
     * verdict on this mode is audible, not numeric. */
    if (capture_is_clock) {
        printf("  tracking error  %+.1f ppm (playback vs capture)\n", tracking_ppm);
    } else if (have_capture) {
        printf("  capture rate above is a thinned sample, not the device's clock\n");
    }

    printf("\n  playback entry status\n");
    for (uint32_t k = 0; k < ctx->out_status_n; k++) {
        printf("    0x%08x  %-24s %u\n", (uint32_t)ctx->out_status_value[k],
               emu_isoc_status_name(ctx->out_status_value[k]),
               ctx->out_status_count[k]);
    }
    if (ctx->have_first_out) {
        printf("  first playback entry: req=%u act=%u, buffer starts:",
               ctx->first_out_req, ctx->first_out_act);
        for (size_t b = 0; b < sizeof ctx->first_out_bytes; b++) {
            printf(" %02x", ctx->first_out_bytes[b]);
        }
        printf("\n");
    }

    if (ctx->hdr_packets) {
        printf("\n  capture lead word  %llu packets carried one, %llu matched frActCount\n",
               (unsigned long long)ctx->hdr_packets,
               (unsigned long long)ctx->hdr_agree);
        for (uint32_t k = 0; k < ctx->hdr_sample_n; k++) {
            printf("    word %u (0x%08x) against frActCount %u\n",
                   ctx->hdr_sample_hdr[k], ctx->hdr_sample_hdr[k],
                   ctx->hdr_sample_act[k]);
        }
    }

    printf("\n  explicit feedback endpoint\n");
    if (!ctx->fb_pipe) {
        printf("    not read\n");
    } else if (ctx->fb_packets == 0) {
        printf("    %llu intervals, none carried a value (%u errors, %u odd length)\n",
               (unsigned long long)ctx->fb_empty, ctx->fb_errors, ctx->fb_short);
    } else {
        double mean = (double)ctx->fb_raw_sum / (double)ctx->fb_packets / 65536.0;
        printf("    %llu values, %llu silent intervals, %u errors, %u odd length\n",
               (unsigned long long)ctx->fb_packets,
               (unsigned long long)ctx->fb_empty, ctx->fb_errors, ctx->fb_short);
        printf("    raw  min 0x%08x  max 0x%08x  last 0x%08x  %u changes\n",
               ctx->fb_raw_min, ctx->fb_raw_max, ctx->fb_raw_last, ctx->fb_changes);
        printf("    mean %.4f in Q16.16 units\n", mean);

        /* Two published readings of the same four bytes, and the rate each
         * implies. Whichever lands on the rate the device was just set to is
         * the one this hardware means; at bInterval 4 they coincide and the
         * question stays open until a top rate is run. */
        printf("    as frames per %.2f ms service interval -> %.2f Hz (%+.1f ppm)\n",
               ctx->interval_ms, mean / ctx->interval_ms * 1000.0,
               (mean / ctx->interval_ms * 1000.0 - cfg->sample_rate)
                   / cfg->sample_rate * 1e6);
        printf("    as frames per millisecond              -> %.2f Hz (%+.1f ppm)\n",
               mean * 1000.0,
               (mean * 1000.0 - cfg->sample_rate) / cfg->sample_rate * 1e6);
        /* The same word with the firmware's fixed-point scaling undone, which
         * is what the planner follows unless `fbraw` says otherwise. At the
         * 44.1 family the two differ by 53.1 ppm; elsewhere they are equal. */
        double corrected = (double)emu_feedback_true_q16(ctx->fb_raw_last) / 65536.0;
        printf("    corrected for the 64000 scaling -> %.4f frames, %.2f Hz (%+.1f ppm)%s\n",
               corrected, corrected / ctx->interval_ms * 1000.0,
               (corrected / ctx->interval_ms * 1000.0 - cfg->sample_rate)
                   / cfg->sample_rate * 1e6,
               ctx->fb_raw ? "  [NOT followed: fbraw]" : "");
        if (have_capture) {
            printf("    capture measured %.4f frames per interval\n", in_per_interval);
        }
        if (ctx->size_from_device) {
            printf("    sizing playback from this endpoint (0x%08x in force)\n",
                   ctx->fb_plan_q16);
        }

        printf("    first values:");
        for (uint32_t k = 0; k < ctx->fb_sample_n; k++) printf(" 0x%08x", ctx->fb_sample[k]);
        printf("\n");
    }

    printf("\n  feedback starved   %u\n", emu_feedback_starved(ctx->feedback));
    printf("  feedback overflow  %u\n", emu_feedback_overflows(ctx->feedback));
    printf("  max queue depth    %u\n", ctx->max_queue_depth);
    printf("  capture errors     %u\n", ctx->in_errors);
    printf("  playback errors    %u\n", ctx->out_errors);
    printf("  short writes       %u\n", ctx->out_short);
    printf("  resyncs            %u\n", ctx->resyncs);

    bool stable = ctx->out_errors == 0
               && ctx->out_short == 0
               && ctx->resyncs == 0
               && emu_feedback_overflows(ctx->feedback) == 0
               && (!capture_is_clock || fabs(tracking_ppm) < 100.0);

    printf("\n  %s\n", stable
        ? (capture_is_clock ? "STABLE: playback tracked capture with no errors"
                            : "STABLE: playback ran with no errors -- listen for the verdict")
        : "UNSTABLE: see counters above");
}

/* -------------------------------------------------------------------- run */

int emu_duplex_run(IOUSBDeviceInterface500** dev,
                   const EmuDeviceModel* model,
                   const DuplexConfig* cfg)
{
    int rc = 1;
    DuplexCtx* ctx = calloc(1, sizeof(DuplexCtx));
    if (!ctx) { fprintf(stderr, "error: out of memory\n"); return 1; }

    IOUSBInterfaceInterface500** in_intf = NULL;
    IOUSBInterfaceInterface500** out_intf = NULL;
    CFRunLoopSourceRef in_source = NULL, out_source = NULL;
    bool in_open = false, out_open = false;
    size_t allocated = 0;

    if (emu_feedback_size() > sizeof ctx->feedback_storage) {
        fprintf(stderr, "fatal: feedback storage too small (%u needed)\n",
                emu_feedback_size());
        goto cleanup;
    }
    ctx->feedback = emu_feedback_init(ctx->feedback_storage);
    if (!ctx->feedback) { fprintf(stderr, "fatal: feedback init failed\n"); goto cleanup; }

    if (cfg->feedback_trace) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        ctx->ticks_per_sec = 1e9 * (double)tb.denom / (double)tb.numer;
        /* One value per 32 service intervals, so 4000 a minute at worst.
         * Allocated once, before anything streams. */
        ctx->fb_log_cap  = 1u << 17;
        ctx->fb_log_time = calloc(ctx->fb_log_cap, sizeof *ctx->fb_log_time);
        ctx->fb_log_raw  = calloc(ctx->fb_log_cap, sizeof *ctx->fb_log_raw);
        if (!ctx->fb_log_time || !ctx->fb_log_raw) {
            fprintf(stderr, "error: out of memory for the feedback trace\n");
            goto cleanup;
        }
        ctx->fb_trace_path = cfg->feedback_trace;
    }

    ctx->playback_only    = cfg->playback_only;
    ctx->capture_idle     = cfg->capture_idle;
    ctx->sync_delay_ms    = cfg->sync_delay_ms;
    ctx->capture_duty     = cfg->capture_duty;
    ctx->fb_raw           = cfg->feedback_raw;
    ctx->request_ms       = cfg->request_ms ? cfg->request_ms : REQUEST_MS;
    if (ctx->request_ms > REQUEST_MS) ctx->request_ms = REQUEST_MS;
    ctx->num_requests     = SCHEDULE_DEPTH_MS / ctx->request_ms;
    if (ctx->num_requests > MAX_REQUESTS) ctx->num_requests = MAX_REQUESTS;
    /* A thinned capture stream cannot size playback either: the measurements
     * arrive for a fraction of the intervals and the queue would starve for
     * the rest. */
    ctx->size_from_device = cfg->playback_only || cfg->capture_idle
                         || cfg->capture_duty > 1 || cfg->plan_from_device;

    /* Playback is interface 1, capture interface 2. In playback_only the
     * capture interface is not opened, not claimed and not moved off alt 0 --
     * the idle state it is already in -- so there is no alt-setting
     * transition to get wrong and nothing of interface 2 on the bus. */
    if (!emu_find_interface(dev, 1, &out_intf)) goto cleanup;
    if (!ctx->playback_only && !emu_find_interface(dev, 2, &in_intf)) goto cleanup;

    if ((*out_intf)->USBInterfaceOpen(out_intf) != kIOReturnSuccess) {
        fprintf(stderr, "error: could not open playback interface\n");
        goto cleanup;
    }
    out_open = true;
    if (in_intf) {
        if ((*in_intf)->USBInterfaceOpen(in_intf) != kIOReturnSuccess) {
            fprintf(stderr, "error: could not open capture interface\n");
            goto cleanup;
        }
        in_open = true;
    }

    const EmuAltSetting *out_alt = NULL, *in_alt = NULL;
    if (!select_alt(out_intf, model, 1, cfg->sample_rate, cfg->short_interval, &out_alt)) goto cleanup;
    if (in_intf && !select_alt(in_intf, model, 2, cfg->sample_rate, cfg->short_interval, &in_alt))
        goto cleanup;

    /* The submit paths reach the interfaces through the context, so these must
     * be stored before anything is queued. */
    ctx->in_intf  = in_intf;
    ctx->out_intf = out_intf;

    if (!emu_find_isoc_pipe(out_intf, kUSBOut, &ctx->out_pipe, &ctx->out_max)) {
        fprintf(stderr, "error: no isochronous OUT pipe\n");
        goto cleanup;
    }
    if (in_intf && !emu_find_isoc_pipe(in_intf, kUSBIn, &ctx->in_pipe, &ctx->in_max)) {
        fprintf(stderr, "error: no isochronous IN pipe\n");
        goto cleanup;
    }

    /* The only isochronous IN pipe on the playback interface is the explicit
     * feedback endpoint. Its absence is not fatal -- the planner runs off
     * capture either way -- so a device without one simply reports nothing. */
    if (!emu_find_isoc_pipe_full(out_intf, kUSBIn, &ctx->fb_pipe,
                                 &ctx->fb_max, &ctx->fb_interval)) {
        ctx->fb_pipe = 0;
    }

    ctx->bytes_per_frame = out_alt->channels * out_alt->subframe_size;
    if (ctx->bytes_per_frame == 0) ctx->bytes_per_frame = 6;
    ctx->interval_ms = (double)(1u << (out_alt->interval - 1)) * 0.125;
    ctx->nominal_frames = (uint32_t)(cfg->sample_rate * ctx->interval_ms / 1000.0);
    /* Frame-list entries arrive one per USB frame -- one per millisecond --
     * whatever the bInterval, because that is how the requests are scheduled.
     * How much *audio* an entry carries is a separate question (interval_ms),
     * and only the rate calculation uses that. Dividing the settle window by
     * interval_ms here asked for 4000 entries at bInterval 3, which a 3-second
     * run never reaches, so no statistics were ever gathered. */
    ctx->amplitude = cfg->amplitude;

    /* Measured with `emu-probe lltest`: the low-latency API delivers one entry
     * per service interval, so 2 per millisecond at bInterval 3 and 1 at
     * bInterval 4. The classic API delivered one per USB frame regardless,
     * which is why bInterval 3 only ever received half its audio. */
    uint32_t period_microframes = 1u << (out_alt->interval - 1);
    ctx->entries_per_ms = period_microframes >= 8 ? 1 : (8 / period_microframes);
    ctx->entries_per_request = ctx->request_ms * ctx->entries_per_ms;
    if (ctx->entries_per_request > MAX_ENTRIES) ctx->entries_per_request = MAX_ENTRIES;

    ctx->capture_entries = ctx->capture_duty > 1
        ? ctx->entries_per_request / ctx->capture_duty
        : ctx->entries_per_request;
    if (ctx->capture_entries == 0) ctx->capture_entries = 1;

    /* After entries_per_ms, not before: computed above it this multiplied by a
     * still-zeroed field, so the settle window was zero intervals and every
     * run reported the device's startup phase-alignment ramp as steady state.
     * That ramp reads hundreds of ppm fast (FINDINGS), which is exactly the
     * size of error a rate measurement here is meant to resolve. */
    ctx->settle_intervals = (uint64_t)(SETTLE_MS * ctx->entries_per_ms);

    /* The feedback endpoint keeps its own bInterval, which is 4 on this device
     * at every rate -- one entry per millisecond even where the data endpoint
     * is serviced twice as often. */
    if (ctx->fb_pipe) {
        uint32_t fb_period = 1u << (ctx->fb_interval - 1);
        uint32_t fb_per_ms = fb_period >= 8 ? 1 : (8 / fb_period);
        ctx->fb_entries_per_request = ctx->request_ms * fb_per_ms;
        if (ctx->fb_entries_per_request > MAX_ENTRIES)
            ctx->fb_entries_per_request = MAX_ENTRIES;
    }

    /* Exact, unlike nominal_frames: at 176.4 kHz a 0.5 ms interval holds 88.2
     * frames, and a feedback value has to be judged against that rather than
     * against the 88 the truncation leaves. */
    ctx->fb_nominal_q16 = (uint32_t)(((uint64_t)cfg->sample_rate << 16)
                                     * period_microframes / 8000u);

    ctx->phase_increment = 2.0 * M_PI * (double)cfg->tone_hz / (double)cfg->sample_rate;

    /* 176.4 kHz averages 88.2 frames per 0.5 ms interval, so a truncated
     * fallback of 88 is slow every time starvation hits. */
    emu_feedback_set_nominal(ctx->feedback, cfg->sample_rate,
                             (uint64_t)(ctx->interval_ms * 1e6));

    printf("%s: %u Hz, %.2f ms interval, %u frames nominal\n",
           ctx->playback_only ? "playback-only" : "duplex",
           cfg->sample_rate, ctx->interval_ms, ctx->nominal_frames);
    printf("        playback iface 1 alt %u, pipe %u, wMaxPacketSize %u\n",
           out_alt->alternate_setting, ctx->out_pipe, ctx->out_max);
    if (in_alt && ctx->capture_idle) {
        printf("        capture  iface 2 alt %u streaming, never read\n",
               in_alt->alternate_setting);
    } else if (in_alt) {
        printf("        capture  iface 2 alt %u, pipe %u, wMaxPacketSize %u\n",
               in_alt->alternate_setting, ctx->in_pipe, ctx->in_max);
    } else {
        printf("        capture  iface 2 not opened; packets sized from 0x81\n");
    }
    printf("        %u entries/ms, %u entries per %u ms request, "
           "%u ms of schedule in flight\n",
           ctx->entries_per_ms, ctx->entries_per_request, ctx->request_ms,
           ctx->request_ms * ctx->num_requests);
    if (ctx->sync_delay_ms) {
        printf("        playback schedule starts %u ms after capture\n",
               ctx->sync_delay_ms);
    }
    if (cfg->plan_from_device && !ctx->playback_only && ctx->capture_duty < 2) {
        printf("        capture runs in full; packets sized from 0x81 anyway\n");
    }
    if (ctx->capture_duty > 1) {
        printf("        capture polled %u of every %u intervals (1 in %u); "
               "packets sized from 0x81\n",
               ctx->capture_entries, ctx->entries_per_request, ctx->capture_duty);
    }
    if (ctx->fb_pipe) {
        printf("        feedback iface 1 pipe %u, wMaxPacketSize %u, bInterval %u\n",
               ctx->fb_pipe, ctx->fb_max, ctx->fb_interval);
    } else {
        printf("        no explicit feedback endpoint\n");
    }
    if (cfg->tone_hz) {
        printf("        generating %u Hz sine at %.0f%% amplitude\n",
               cfg->tone_hz, cfg->amplitude * 100.0);
    } else {
        printf("        playing silence\n");
    }

    if ((*out_intf)->CreateInterfaceAsyncEventSource(out_intf, &out_source) != kIOReturnSuccess ||
        (in_intf && (*in_intf)->CreateInterfaceAsyncEventSource(in_intf, &in_source)
                        != kIOReturnSuccess)) {
        fprintf(stderr, "error: could not create async event sources\n");
        goto cleanup;
    }
    if (in_source) CFRunLoopAddSource(CFRunLoopGetCurrent(), in_source, kCFRunLoopDefaultMode);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), out_source, kCFRunLoopDefaultMode);

    for (uint32_t i = 0; i < ctx->num_requests; i++) {
        /* Low-latency buffers must come from the interface so they can be
         * shared with the kernel without copying; plain malloc is rejected. */
        UInt32 list_bytes = ctx->entries_per_request * sizeof(IOUSBLowLatencyIsocFrame);

        ctx->in_requests[i].ctx = ctx;
        ctx->out_requests[i].ctx = ctx;
        ctx->out_requests[i].playback = true;
        ctx->fb_requests[i].ctx = ctx;

        if (ctx->fb_pipe) {
            UInt32 fb_list_bytes =
                ctx->fb_entries_per_request * sizeof(IOUSBLowLatencyIsocFrame);
            if ((*out_intf)->LowLatencyCreateBuffer(out_intf, &ctx->fb_requests[i].buffer,
                    (UInt32)ctx->fb_entries_per_request * ctx->fb_max,
                    kUSBLowLatencyReadBuffer) != kIOReturnSuccess ||
                (*out_intf)->LowLatencyCreateBuffer(out_intf, (void**)&ctx->fb_requests[i].frames,
                    fb_list_bytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess) {
                fprintf(stderr, "error: LowLatencyCreateBuffer (feedback) failed\n");
                goto cleanup;
            }
        }

        if ((in_intf &&
             ((*in_intf)->LowLatencyCreateBuffer(in_intf, &ctx->in_requests[i].buffer,
                 (UInt32)ctx->entries_per_request * ctx->in_max,
                 kUSBLowLatencyReadBuffer) != kIOReturnSuccess ||
              (*in_intf)->LowLatencyCreateBuffer(in_intf, (void**)&ctx->in_requests[i].frames,
                 list_bytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess)) ||
            (*out_intf)->LowLatencyCreateBuffer(out_intf, &ctx->out_requests[i].buffer,
                (UInt32)ctx->entries_per_request * ctx->out_max,
                kUSBLowLatencyWriteBuffer) != kIOReturnSuccess ||
            (*out_intf)->LowLatencyCreateBuffer(out_intf, (void**)&ctx->out_requests[i].frames,
                list_bytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess) {
            fprintf(stderr, "error: LowLatencyCreateBuffer failed\n");
            goto cleanup;
        }
        allocated++;
    }

    UInt64 now = 0;
    AbsoluteTime at;
    if ((*out_intf)->GetBusFrameNumber(out_intf, &now, &at) != kIOReturnSuccess) {
        fprintf(stderr, "error: GetBusFrameNumber failed\n");
        goto cleanup;
    }
    ctx->next_in_frame  = now + SCHEDULE_MARGIN;
    ctx->next_out_frame = now + SCHEDULE_MARGIN + ctx->sync_delay_ms;
    ctx->next_fb_frame  = now + SCHEDULE_MARGIN;
    ctx->fb_log_start   = mach_absolute_time();

    /* Capture first: playback should have measurements waiting rather than
     * starving through its whole first request. */
    for (uint32_t i = 0; i < ctx->num_requests && in_intf && !ctx->capture_idle; i++) {
        IOReturn kr = submit_capture(&ctx->in_requests[i]);
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "error: capture submit %d: 0x%08x\n", i, kr);
            goto cleanup;
        }
    }
    /* Reading feedback changes nothing about the stream: the planner still
     * sizes packets from capture. A device that refuses the pipe is reported
     * and stepped over rather than taken as a failure of the run. */
    if (ctx->fb_pipe) {
        for (uint32_t i = 0; i < ctx->num_requests; i++) {
            IOReturn kr = submit_feedback(&ctx->fb_requests[i]);
            if (kr != kIOReturnSuccess) {
                fprintf(stderr, "warning: feedback submit %d: 0x%08x (%s)\n",
                        i, kr, emu_isoc_status_name((int32_t)kr));
                ctx->fb_pipe = 0;
                break;
            }
        }
    }
    for (uint32_t i = 0; i < ctx->num_requests; i++) {
        IOReturn kr = submit_playback(&ctx->out_requests[i]);
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "error: playback submit %d: 0x%08x\n", i, kr);
            goto cleanup;
        }
    }

    printf("        streaming for %u ms...\n", cfg->duration_ms);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, cfg->duration_ms / 1000.0, false);

    ctx->stopping = true;
    (*out_intf)->AbortPipe(out_intf, ctx->out_pipe);
    if (in_intf) (*in_intf)->AbortPipe(in_intf, ctx->in_pipe);
    if (ctx->fb_pipe) (*out_intf)->AbortPipe(out_intf, ctx->fb_pipe);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);

    report(ctx, cfg);

    /* Written after the stream has stopped: the streaming path only ever
     * appends to memory it already owns. */
    if (ctx->fb_trace_path && ctx->fb_log_n) {
        FILE* f = fopen(ctx->fb_trace_path, "w");
        if (!f) {
            fprintf(stderr, "error: could not write %s\n", ctx->fb_trace_path);
        } else {
            fprintf(f, "seconds,raw,frames\n");
            for (uint32_t i = 0; i < ctx->fb_log_n; i++) {
                double t = (double)(ctx->fb_log_time[i] - ctx->fb_log_start)
                         / ctx->ticks_per_sec;
                fprintf(f, "%.6f,0x%08x,%.6f\n", t, ctx->fb_log_raw[i],
                        (double)ctx->fb_log_raw[i] / 65536.0);
            }
            fclose(f);
            printf("\n  feedback trace: %u values -> %s\n",
                   ctx->fb_log_n, ctx->fb_trace_path);
        }
    }
    rc = 0;

cleanup:
    if (in_source) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), in_source, kCFRunLoopDefaultMode);
        CFRelease(in_source);
    }
    if (out_source) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), out_source, kCFRunLoopDefaultMode);
        CFRelease(out_source);
    }
    for (size_t i = 0; i < allocated; i++) {
        if (ctx->in_requests[i].buffer && in_intf)  (*in_intf)->LowLatencyDestroyBuffer(in_intf, ctx->in_requests[i].buffer);
        if (ctx->in_requests[i].frames && in_intf)  (*in_intf)->LowLatencyDestroyBuffer(in_intf, ctx->in_requests[i].frames);
        if (ctx->out_requests[i].buffer) (*out_intf)->LowLatencyDestroyBuffer(out_intf, ctx->out_requests[i].buffer);
        if (ctx->out_requests[i].frames) (*out_intf)->LowLatencyDestroyBuffer(out_intf, ctx->out_requests[i].frames);
        if (ctx->fb_requests[i].buffer)  (*out_intf)->LowLatencyDestroyBuffer(out_intf, ctx->fb_requests[i].buffer);
        if (ctx->fb_requests[i].frames)  (*out_intf)->LowLatencyDestroyBuffer(out_intf, ctx->fb_requests[i].frames);
    }
    /* Alt 0 releases the reserved isochronous bandwidth. */
    if (out_open) { (*out_intf)->SetAlternateInterface(out_intf, 0); (*out_intf)->USBInterfaceClose(out_intf); }
    if (in_open)  { (*in_intf)->SetAlternateInterface(in_intf, 0);   (*in_intf)->USBInterfaceClose(in_intf); }
    if (out_intf) (*out_intf)->Release(out_intf);
    if (in_intf)  (*in_intf)->Release(in_intf);
    free(ctx->fb_log_time);
    free(ctx->fb_log_raw);
    free(ctx);
    return rc;
}
