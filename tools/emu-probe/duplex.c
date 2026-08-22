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

#define NUM_REQUESTS        8
/* Milliseconds of audio per request. Entries per request depend on how often
 * the endpoint is serviced, so the array is sized for the densest case. */
#define REQUEST_MS          8
#define MAX_ENTRIES         (REQUEST_MS * 8)
#define SCHEDULE_MARGIN     16

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

    uint64_t next_in_frame;
    uint64_t next_out_frame;

    Request in_requests[NUM_REQUESTS];
    Request out_requests[NUM_REQUESTS];

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

static IOReturn submit_capture(Request* req)
{
    DuplexCtx* ctx = req->ctx;

    for (uint32_t i = 0; i < ctx->entries_per_request; i++) {
        /* The API stamps this key and overwrites it as entries land. */
        req->frames[i].frStatus   = kUSBLowLatencyIsochTransferKey;
        req->frames[i].frReqCount = ctx->in_max;
        req->frames[i].frActCount = 0;
    }

    req->frame_start = ctx->next_in_frame;
    ctx->next_in_frame += REQUEST_MS;

    return (*ctx->in_intf)->LowLatencyReadIsochPipeAsync(
        ctx->in_intf, ctx->in_pipe, req->buffer,
        req->frame_start, ctx->entries_per_request, 1, req->frames,
        capture_complete, req);
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
        uint32_t frames = emu_feedback_next(ctx->feedback, ctx->nominal_frames);
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
    ctx->next_out_frame += REQUEST_MS;

    return (*ctx->out_intf)->LowLatencyWriteIsochPipeAsync(
        ctx->out_intf, ctx->out_pipe, req->buffer,
        req->frame_start, ctx->entries_per_request, 1, req->frames,
        playback_complete, req);
}

static void resync(DuplexCtx* ctx)
{
    UInt64 now = 0;
    AbsoluteTime at;
    if ((*ctx->in_intf)->GetBusFrameNumber(ctx->in_intf, &now, &at) == kIOReturnSuccess) {
        ctx->next_in_frame  = now + SCHEDULE_MARGIN;
        ctx->next_out_frame = now + SCHEDULE_MARGIN;
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

    for (uint32_t i = 0; i < ctx->entries_per_request; i++) {
        const IOUSBLowLatencyIsocFrame* f = &req->frames[i];
        ctx->intervals_elapsed++;

        if (!emu_frame_ok(f->frStatus)) {
            if (settled && !ctx->stopping) ctx->in_errors++;
            continue;
        }
        if (f->frActCount == 0) continue;

        uint32_t frames = emu_frames_in_packet(f->frActCount, ctx->bytes_per_frame);

        /* Feed the planner even while settling: playback needs it from the
         * first interval, and only the statistics wait. */
        emu_feedback_push(ctx->feedback, frames);

        if (settled) {
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
        if (!settled) continue;

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

/* ---------------------------------------------------------------- helpers */

/* Selects the alternate setting for `rate` on `interface`, going through alt 0
 * first so the transition always starts from a known state. */
static bool select_alt(IOUSBInterfaceInterface500** intf,
                       const EmuDeviceModel* model,
                       uint8_t interface_number,
                       uint32_t rate,
                       const EmuAltSetting** chosen_out)
{
    const EmuAltSetting* chosen = NULL;
    for (uint16_t i = 0; i < model->num_alt_settings; i++) {
        const EmuAltSetting* a = &model->alt_settings[i];
        if (a->interface_number != interface_number) continue;
        if (a->data_endpoint == 0) continue;
        if (a->sample_rate != rate) continue;
        if (!chosen || a->interval > chosen->interval) chosen = a;
    }
    if (!chosen) {
        fprintf(stderr, "error: interface %u has no alt setting for %u Hz\n",
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
    printf("\nDuplex results (after %.0f ms settling)\n", SETTLE_MS);

    if (ctx->out_packets == 0) {
        printf("  no playback packets completed\n");
        return;
    }

    /* With the low-latency API an entry is one service interval on both sides,
     * so the same conversion applies to each. */
    double in_per_interval  = (double)ctx->in_frames  / (double)ctx->in_packets;
    double out_per_entry    = (double)ctx->out_frames / (double)ctx->out_packets;
    double out_entry_ms     = ctx->interval_ms;

    double in_hz  = in_per_interval / ctx->interval_ms * 1000.0;
    double out_hz = out_per_entry   / out_entry_ms     * 1000.0;

    printf("  capture   %llu packets, %llu frames, %.4f frames/interval\n",
           (unsigned long long)ctx->in_packets,
           (unsigned long long)ctx->in_frames, in_per_interval);
    printf("  playback  %llu packets, %llu frames, %.4f frames/entry (%.2f ms)\n",
           (unsigned long long)ctx->out_packets,
           (unsigned long long)ctx->out_frames, out_per_entry, out_entry_ms);

    printf("\n  capture rate    %.2f Hz (%+.1f ppm)\n", in_hz,
           (in_hz - cfg->sample_rate) / cfg->sample_rate * 1e6);
    printf("  playback rate   %.2f Hz (%+.1f ppm)\n", out_hz,
           (out_hz - cfg->sample_rate) / cfg->sample_rate * 1e6);

    /* The property that matters: playback must track capture, because capture
     * is the device's own clock. Drift between them is unbounded error. */
    double tracking_ppm = (out_hz - in_hz) / in_hz * 1e6;
    printf("  tracking error  %+.1f ppm (playback vs capture)\n", tracking_ppm);

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
               && fabs(tracking_ppm) < 100.0;

    printf("\n  %s\n", stable
        ? "STABLE: playback tracked capture with no errors"
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

    /* Playback is interface 1, capture interface 2. */
    if (!emu_find_interface(dev, 1, &out_intf)) goto cleanup;
    if (!emu_find_interface(dev, 2, &in_intf))  goto cleanup;

    if ((*out_intf)->USBInterfaceOpen(out_intf) != kIOReturnSuccess) {
        fprintf(stderr, "error: could not open playback interface\n");
        goto cleanup;
    }
    out_open = true;
    if ((*in_intf)->USBInterfaceOpen(in_intf) != kIOReturnSuccess) {
        fprintf(stderr, "error: could not open capture interface\n");
        goto cleanup;
    }
    in_open = true;

    const EmuAltSetting *out_alt = NULL, *in_alt = NULL;
    if (!select_alt(out_intf, model, 1, cfg->sample_rate, &out_alt)) goto cleanup;
    if (!select_alt(in_intf,  model, 2, cfg->sample_rate, &in_alt))  goto cleanup;

    /* The submit paths reach the interfaces through the context, so these must
     * be stored before anything is queued. */
    ctx->in_intf  = in_intf;
    ctx->out_intf = out_intf;

    if (!emu_find_isoc_pipe(out_intf, kUSBOut, &ctx->out_pipe, &ctx->out_max)) {
        fprintf(stderr, "error: no isochronous OUT pipe\n");
        goto cleanup;
    }
    if (!emu_find_isoc_pipe(in_intf, kUSBIn, &ctx->in_pipe, &ctx->in_max)) {
        fprintf(stderr, "error: no isochronous IN pipe\n");
        goto cleanup;
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
    ctx->settle_intervals = (uint64_t)(SETTLE_MS * ctx->entries_per_ms);
    ctx->amplitude = cfg->amplitude;

    /* Measured with `emu-probe lltest`: the low-latency API delivers one entry
     * per service interval, so 2 per millisecond at bInterval 3 and 1 at
     * bInterval 4. The classic API delivered one per USB frame regardless,
     * which is why bInterval 3 only ever received half its audio. */
    uint32_t period_microframes = 1u << (out_alt->interval - 1);
    ctx->entries_per_ms = period_microframes >= 8 ? 1 : (8 / period_microframes);
    ctx->entries_per_request = REQUEST_MS * ctx->entries_per_ms;
    if (ctx->entries_per_request > MAX_ENTRIES) ctx->entries_per_request = MAX_ENTRIES;

    ctx->phase_increment = 2.0 * M_PI * (double)cfg->tone_hz / (double)cfg->sample_rate;

    /* 176.4 kHz averages 88.2 frames per 0.5 ms interval, so a truncated
     * fallback of 88 is slow every time starvation hits. */
    emu_feedback_set_nominal(ctx->feedback, cfg->sample_rate,
                             (uint64_t)(ctx->interval_ms * 1e6));

    printf("duplex: %u Hz, %.2f ms interval, %u frames nominal\n",
           cfg->sample_rate, ctx->interval_ms, ctx->nominal_frames);
    printf("        playback iface 1 alt %u, pipe %u, wMaxPacketSize %u\n",
           out_alt->alternate_setting, ctx->out_pipe, ctx->out_max);
    printf("        capture  iface 2 alt %u, pipe %u, wMaxPacketSize %u\n",
           in_alt->alternate_setting, ctx->in_pipe, ctx->in_max);
    printf("        %u entries/ms, %u entries per %u ms request\n",
           ctx->entries_per_ms, ctx->entries_per_request, REQUEST_MS);
    if (cfg->tone_hz) {
        printf("        generating %u Hz sine at %.0f%% amplitude\n",
               cfg->tone_hz, cfg->amplitude * 100.0);
    } else {
        printf("        playing silence\n");
    }

    if ((*in_intf)->CreateInterfaceAsyncEventSource(in_intf, &in_source) != kIOReturnSuccess ||
        (*out_intf)->CreateInterfaceAsyncEventSource(out_intf, &out_source) != kIOReturnSuccess) {
        fprintf(stderr, "error: could not create async event sources\n");
        goto cleanup;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), in_source, kCFRunLoopDefaultMode);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), out_source, kCFRunLoopDefaultMode);

    for (int i = 0; i < NUM_REQUESTS; i++) {
        /* Low-latency buffers must come from the interface so they can be
         * shared with the kernel without copying; plain malloc is rejected. */
        UInt32 list_bytes = ctx->entries_per_request * sizeof(IOUSBLowLatencyIsocFrame);

        ctx->in_requests[i].ctx = ctx;
        ctx->out_requests[i].ctx = ctx;
        ctx->out_requests[i].playback = true;

        if ((*in_intf)->LowLatencyCreateBuffer(in_intf, &ctx->in_requests[i].buffer,
                (UInt32)ctx->entries_per_request * ctx->in_max,
                kUSBLowLatencyReadBuffer) != kIOReturnSuccess ||
            (*in_intf)->LowLatencyCreateBuffer(in_intf, (void**)&ctx->in_requests[i].frames,
                list_bytes, kUSBLowLatencyFrameListBuffer) != kIOReturnSuccess ||
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
    if ((*in_intf)->GetBusFrameNumber(in_intf, &now, &at) != kIOReturnSuccess) {
        fprintf(stderr, "error: GetBusFrameNumber failed\n");
        goto cleanup;
    }
    ctx->next_in_frame  = now + SCHEDULE_MARGIN;
    ctx->next_out_frame = now + SCHEDULE_MARGIN;

    /* Capture first: playback should have measurements waiting rather than
     * starving through its whole first request. */
    for (int i = 0; i < NUM_REQUESTS; i++) {
        IOReturn kr = submit_capture(&ctx->in_requests[i]);
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "error: capture submit %d: 0x%08x\n", i, kr);
            goto cleanup;
        }
    }
    for (int i = 0; i < NUM_REQUESTS; i++) {
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
    (*in_intf)->AbortPipe(in_intf, ctx->in_pipe);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);

    report(ctx, cfg);
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
        if (ctx->in_requests[i].buffer)  (*in_intf)->LowLatencyDestroyBuffer(in_intf, ctx->in_requests[i].buffer);
        if (ctx->in_requests[i].frames)  (*in_intf)->LowLatencyDestroyBuffer(in_intf, ctx->in_requests[i].frames);
        if (ctx->out_requests[i].buffer) (*out_intf)->LowLatencyDestroyBuffer(out_intf, ctx->out_requests[i].buffer);
        if (ctx->out_requests[i].frames) (*out_intf)->LowLatencyDestroyBuffer(out_intf, ctx->out_requests[i].frames);
    }
    /* Alt 0 releases the reserved isochronous bandwidth. */
    if (out_open) { (*out_intf)->SetAlternateInterface(out_intf, 0); (*out_intf)->USBInterfaceClose(out_intf); }
    if (in_open)  { (*in_intf)->SetAlternateInterface(in_intf, 0);   (*in_intf)->USBInterfaceClose(in_intf); }
    if (out_intf) (*out_intf)->Release(out_intf);
    if (in_intf)  (*in_intf)->Release(in_intf);
    free(ctx);
    return rc;
}
