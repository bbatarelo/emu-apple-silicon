#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <mach/mach_time.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>

/* Eight requests of 16 frames keeps ~128 ms in flight, which is far more than
 * the host needs and makes the probe insensitive to scheduling hiccups in this
 * userspace process. A real driver would run much shallower. */
#define NUM_REQUESTS        8
#define FRAMES_PER_REQUEST  16
/* Isoc transfers must be scheduled into the future; too close and the
 * controller rejects them as already past. */
#define SCHEDULE_MARGIN     16
/* The device runs a phase-alignment ramp when a stream starts: it inserts one
 * extra sample frame every tenth service interval until phase is recovered,
 * then holds exact nominal. Measured at 48 kHz that ramp ran from interval +157
 * to +987 -- roughly 830 ms -- though a later capture ramped for 1630 ms, so
 * the duration varies and the window has to allow for the longer case.
 * Averaging across the ramp reports the device as hundreds of ppm fast. Discard a settling window measured in real time, not in
 * intervals, so it covers the ramp at every bInterval. The trace file still
 * records every packet, ramp included. */
#define SETTLE_MS           2000.0

typedef struct {
    uint64_t frame;
    uint16_t requested;
    uint16_t actual;
    int32_t  status;
} PacketRecord;

typedef struct CaptureCtx CaptureCtx;

typedef struct {
    CaptureCtx*      ctx;
    void*            buffer;
    IOUSBIsocFrame*  frames;
    uint64_t         frame_start;
    bool             in_flight;
} Request;

struct CaptureCtx {
    IOUSBInterfaceInterface500** intf;
    uint8_t   pipe_ref;
    uint16_t  max_packet_size;
    /* A high-speed endpoint with bInterval < 4 is serviced more than once per
     * 1 ms USB frame, and each frame-list entry holds exactly one of those
     * transactions -- asking for a larger frReqCount does not coalesce them, it
     * just provokes overruns. So an entry measures one *service interval*, not
     * one millisecond, and the rate calculation has to divide by that interval
     * instead. Getting this wrong reads back as exactly half the true rate. */
    uint8_t   transactions_per_frame;
    double    service_interval_ms;
    uint32_t  bytes_per_frame;

    uint64_t  next_frame;
    Request   requests[NUM_REQUESTS];

    PacketRecord* records;
    size_t    record_capacity;
    size_t    record_count;

    /* First non-empty packet, kept so the payload can be inspected. Packet
     * sizes that are not a whole number of sample frames mean the layout is not
     * what the descriptor implies, and only the bytes can settle that. */
    uint8_t   first_packet[96];
    uint8_t   first_packet_tail[24];
    uint16_t  first_packet_tail_len;
    uint16_t  first_packet_len;
    bool      have_first_packet;

    uint32_t  resync_count;
    uint32_t  submit_failures;
    bool      stopping;
};

/* --------------------------------------------------------------- plumbing */

static bool find_capture_interface(IOUSBDeviceInterface500** dev,
                                   uint8_t interface_number,
                                   IOUSBInterfaceInterface500*** out)
{
    IOUSBFindInterfaceRequest request;
    request.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
    request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    request.bAlternateSetting  = kIOUSBFindInterfaceDontCare;

    io_iterator_t iter = IO_OBJECT_NULL;
    if ((*dev)->CreateInterfaceIterator(dev, &request, &iter) != kIOReturnSuccess) {
        fprintf(stderr, "error: CreateInterfaceIterator failed\n");
        return false;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        IOCFPlugInInterface** plugin = NULL;
        SInt32 score = 0;
        kern_return_t kr = IOCreatePlugInInterfaceForService(
            service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        IOObjectRelease(service);
        if (kr != kIOReturnSuccess || !plugin) continue;

        IOUSBInterfaceInterface500** intf = NULL;
        HRESULT hr = (*plugin)->QueryInterface(plugin,
                        CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID*)&intf);
        (*plugin)->Release(plugin);
        if (hr || !intf) continue;

        UInt8 number = 0xff;
        (*intf)->GetInterfaceNumber(intf, &number);
        if (number == interface_number) {
            IOObjectRelease(iter);
            *out = intf;
            return true;
        }
        (*intf)->Release(intf);
    }

    IOObjectRelease(iter);
    fprintf(stderr, "error: interface %u not found\n", interface_number);
    return false;
}

static bool find_isoc_in_pipe(IOUSBInterfaceInterface500** intf,
                              uint8_t* out_pipe, uint16_t* out_max_packet)
{
    UInt8 num_endpoints = 0;
    if ((*intf)->GetNumEndpoints(intf, &num_endpoints) != kIOReturnSuccess) {
        return false;
    }

    /* Pipe 0 is the default control pipe, so endpoints start at 1. */
    for (UInt8 i = 1; i <= num_endpoints; i++) {
        UInt8 direction, number, transfer_type, interval;
        UInt16 max_packet = 0;
        if ((*intf)->GetPipeProperties(intf, i, &direction, &number,
                                       &transfer_type, &max_packet, &interval)
            != kIOReturnSuccess) {
            continue;
        }
        if (transfer_type == kUSBIsoc && direction == kUSBIn) {
            *out_pipe = i;
            *out_max_packet = max_packet;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------- submission */

static void request_complete(void* refcon, IOReturn result, void* arg0);

static IOReturn submit(Request* req)
{
    CaptureCtx* ctx = req->ctx;

    for (uint32_t i = 0; i < FRAMES_PER_REQUEST; i++) {
        req->frames[i].frStatus   = 0;
        req->frames[i].frReqCount = ctx->max_packet_size;
        req->frames[i].frActCount = 0;
    }

    req->frame_start = ctx->next_frame;
    ctx->next_frame += FRAMES_PER_REQUEST;

    IOReturn kr = (*ctx->intf)->ReadIsochPipeAsync(
        ctx->intf, ctx->pipe_ref, req->buffer,
        req->frame_start, FRAMES_PER_REQUEST, req->frames,
        request_complete, req);

    req->in_flight = (kr == kIOReturnSuccess);
    return kr;
}

/* If the host controller has moved past the frame we asked for, jump forward
 * rather than falling further behind on every retry. */
static void resync(CaptureCtx* ctx)
{
    UInt64 now = 0;
    AbsoluteTime at;
    if ((*ctx->intf)->GetBusFrameNumber(ctx->intf, &now, &at) == kIOReturnSuccess) {
        ctx->next_frame = now + SCHEDULE_MARGIN;
        ctx->resync_count++;
    }
}

static void request_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    Request* req = (Request*)refcon;
    CaptureCtx* ctx = req->ctx;

    req->in_flight = false;

    if (result != kIOReturnSuccess && result != kIOReturnUnderrun) {
        if (result == kIOReturnIsoTooOld) {
            resync(ctx);
        } else if (!ctx->stopping) {
            fprintf(stderr, "warning: isoc request failed: 0x%08x\n", result);
        }
    }

    /* The isochronous read buffer is strided by frReqCount, which this probe
     * sets to wMaxPacketSize for every entry -- not packed by what actually
     * arrived. A short packet leaves a gap. */
    size_t offset = 0;

    for (uint32_t i = 0; i < FRAMES_PER_REQUEST; i++) {
        if (ctx->record_count >= ctx->record_capacity) break;

        if (!ctx->have_first_packet && req->frames[i].frActCount > 0) {
            uint16_t len = req->frames[i].frActCount;
            if (len > sizeof ctx->first_packet) len = sizeof ctx->first_packet;
            memcpy(ctx->first_packet, (const uint8_t*)req->buffer + offset, len);
            ctx->first_packet_len = len;

            uint16_t act = req->frames[i].frActCount;
            uint16_t tail = act > sizeof ctx->first_packet_tail
                          ? (uint16_t)sizeof ctx->first_packet_tail : act;
            memcpy(ctx->first_packet_tail,
                   (const uint8_t*)req->buffer + offset + (act - tail),
                   tail);
            ctx->first_packet_tail_len = tail;
            ctx->have_first_packet = true;
        }

        offset += ctx->max_packet_size;

        ctx->records[ctx->record_count++] = (PacketRecord){
            .frame     = req->frame_start + i,
            .requested = req->frames[i].frReqCount,
            .actual    = req->frames[i].frActCount,
            .status    = req->frames[i].frStatus,
        };
    }

    if (ctx->stopping || ctx->record_count >= ctx->record_capacity) {
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }

    if (submit(req) != kIOReturnSuccess) {
        ctx->submit_failures++;
        resync(ctx);
        if (submit(req) != kIOReturnSuccess) {
            ctx->stopping = true;
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
    }
}

/* --------------------------------------------------------------- analysis */

static const char* isoc_status_name(int32_t status)
{
    switch ((uint32_t)status) {
        case 0:          return "success";
        case 0xe0004001: return "kIOUSBNotSent1Err";
        case 0xe0004002: return "kIOUSBNotSent2Err";
        case 0xe0004003: return "kIOUSBBufferUnderrunErr";
        case 0xe0004004: return "kIOUSBBufferOverrunErr";
        case 0xe000400f: return "kIOUSBWrongPIDErr";
        case 0xe0004010: return "kIOUSBPIDCheckErr";
        case 0xe0004011: return "kIOUSBDataToggleErr";
        case 0xe0004050: return "kIOUSBTransactionReturned";
        case 0xe0004051: return "kIOUSBTransactionTimeout";
        case 0xe00002e7: return "kIOReturnUnderrun";
        case 0xe00002e8: return "kIOReturnOverrun";
        case 0xe00002eb: return "kIOReturnAborted";
        case 0xe00002ec: return "kIOReturnNoBandwidth";
        case 0xe00002ed: return "kIOReturnNotResponding";
        case 0xe00002ee: return "kIOReturnIsoTooOld";
        case 0xe00002ef: return "kIOReturnIsoTooNew";
        default:         return "?";
    }
}

/* Histogram of per-frame status codes. When a capture goes wrong this is the
 * single most informative thing to look at, so it prints unconditionally. */
static void report_statuses(const CaptureCtx* ctx)
{
    struct { int32_t status; size_t count; } seen[12];
    size_t distinct = 0;

    for (size_t i = 0; i < ctx->record_count; i++) {
        int32_t st = ctx->records[i].status;
        size_t j = 0;
        for (; j < distinct; j++) {
            if (seen[j].status == st) { seen[j].count++; break; }
        }
        if (j == distinct && distinct < 12) {
            seen[distinct].status = st;
            seen[distinct].count = 1;
            distinct++;
        }
    }

    printf("\nFrame status\n");
    for (size_t i = 0; i < distinct; i++) {
        printf("  0x%08x  %-26s %6zu  %5.1f%%\n",
               (uint32_t)seen[i].status, isoc_status_name(seen[i].status),
               seen[i].count,
               100.0 * (double)seen[i].count / (double)ctx->record_count);
    }

    if (ctx->have_first_packet) {
        printf("\nFirst packet payload (%u bytes shown)\n  ", ctx->first_packet_len);
        for (uint16_t i = 0; i < ctx->first_packet_len; i++) {
            printf("%02x ", ctx->first_packet[i]);
            if ((i + 1) % 24 == 0) printf("\n  ");
        }
        printf("\n");
        printf("  ...tail: ");
        for (uint16_t i = 0; i < ctx->first_packet_tail_len; i++) {
            printf("%02x ", ctx->first_packet_tail[i]);
        }
        printf("\n");
    }

    printf("\nFirst 8 records (frame, requested, actual, status)\n");
    for (size_t i = 0; i < ctx->record_count && i < 8; i++) {
        const PacketRecord* r = &ctx->records[i];
        printf("  %llu  req=%-4u act=%-4u 0x%08x %s\n",
               (unsigned long long)r->frame, r->requested, r->actual,
               (uint32_t)r->status, isoc_status_name(r->status));
    }
}

/* A short isoc IN frame is normal, not a failure. The device sends what its
 * clock produced; wMaxPacketSize is only the ceiling we reserved. Treating
 * underrun as an error would discard every useful packet. */
static bool frame_ok(int32_t status)
{
    return status == 0 || (uint32_t)status == 0xe00002e7;
}

static int compare_u32(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

static void report(const CaptureCtx* ctx, const CaptureConfig* cfg,
                   uint8_t alt, uint8_t interval)
{
    size_t n = ctx->record_count;
    if (n == 0) {
        printf("\nno packets captured\n");
        return;
    }

    report_statuses(ctx);

    /* Ignore leading empty packets: the device needs a moment to start
     * producing after the alternate setting is selected. */
    size_t first = 0;
    while (first < n && ctx->records[first].actual == 0) first++;

    /* Trim trailing aborted frames. Those are the teardown of the requests that
     * were still in flight when the run loop stopped, not lost audio. Counting
     * them in the frame span would understate the measured rate -- six aborted
     * frames in 2496 is 2400 ppm of pure artefact. */
    size_t last = n;
    while (last > first && !frame_ok(ctx->records[last - 1].status)) last--;
    if (last > first) n = last;

    size_t settle_intervals = (size_t)(SETTLE_MS / ctx->service_interval_ms);
    size_t settled = first + settle_intervals;
    if (settled < n) {
        first = settled;
    } else {
        printf("\nwarning: capture too short to settle; statistics may be skewed\n");
    }

    size_t counted = 0, errors = 0, empty = 0;
    uint64_t total_bytes = 0;
    /* Sample frames must be accumulated per packet, not by dividing the byte
     * total at the end. A partial sample frame cannot exist, so any remainder
     * belongs to that packet alone; summing first would let remainders pile up
     * into phantom frames. */
    uint64_t audio_frames = 0;
    uint64_t leftover_bytes = 0;
    uint32_t min_bytes = UINT32_MAX, max_bytes = 0;

    for (size_t i = first; i < n; i++) {
        const PacketRecord* r = &ctx->records[i];
        if (!frame_ok(r->status)) { errors++; continue; }
        if (r->actual == 0) { empty++; }
        total_bytes    += r->actual;
        audio_frames   += r->actual / ctx->bytes_per_frame;
        leftover_bytes += r->actual % ctx->bytes_per_frame;
        if (r->actual < min_bytes) min_bytes = r->actual;
        if (r->actual > max_bytes) max_bytes = r->actual;
        counted++;
    }

    if (counted == 0) {
        printf("\nno usable packets (errors=%zu)\n", errors);
        return;
    }

    /* Every USB frame is 1 ms of the host controller's clock, whether or not a
     * packet arrived in it, so the span is the correct time base. */
    uint64_t frame_span = ctx->records[n - 1].frame - ctx->records[first].frame + 1;
    /* Each observed entry covers one service interval of known duration, so
     * sample frames per interval is a direct measure of the device's clock
     * against the host controller's. That ratio is what the clock estimator
     * ultimately consumes. */
    double frames_per_interval = (double)audio_frames / (double)counted;
    double measured_hz = frames_per_interval / ctx->service_interval_ms * 1000.0;
    double frames_per_ms = measured_hz / 1000.0;
    double drift_ppm = (measured_hz - (double)cfg->sample_rate)
                     / (double)cfg->sample_rate * 1e6;

    printf("\nCapture results\n");
    printf("  settling discarded  %zu intervals (%.0f ms)\n",
           settle_intervals, SETTLE_MS);
    printf("  alt setting        %u (bInterval %u, %u transaction%s per frame)\n",
           alt, interval, ctx->transactions_per_frame,
           ctx->transactions_per_frame == 1 ? "" : "s");
    printf("  packets recorded   %zu (%zu counted, %zu errors, %zu empty)\n",
           n, counted, errors, empty);
    printf("  USB frame span     %llu\n", (unsigned long long)frame_span);
    printf("  bytes received     %llu\n", (unsigned long long)total_bytes);
    printf("  sample frames      %llu\n", (unsigned long long)audio_frames);
    printf("  packet bytes       min %u, max %u\n", min_bytes, max_bytes);
    printf("  bytes per frame    %u\n", ctx->bytes_per_frame);
    if (leftover_bytes) {
        printf("  UNEXPLAINED        %llu bytes beyond whole sample frames"
               " (%.2f per packet)\n",
               (unsigned long long)leftover_bytes,
               (double)leftover_bytes / (double)counted);
    }

    printf("\nClock\n");
    printf("  nominal rate       %u Hz\n", cfg->sample_rate);
    printf("  measured rate      %.2f Hz\n", measured_hz);
    printf("  deviation          %+.1f ppm\n", drift_ppm);
    printf("  sample frames/ms   %.4f\n", frames_per_ms);
    printf("  frames per interval %.4f over %.2f ms\n",
           frames_per_interval, ctx->service_interval_ms);

    if (ctx->resync_count || ctx->submit_failures) {
        printf("\n  resyncs %u, submit failures %u\n",
               ctx->resync_count, ctx->submit_failures);
    }

    /* The distribution is the interesting part: an asynchronous endpoint on a
     * non-integer rate must alternate packet sizes, and that pattern is the raw
     * material the feedback planner consumes. */
    uint32_t* sizes = malloc(counted * sizeof(uint32_t));
    if (sizes) {
        size_t k = 0;
        for (size_t i = first; i < n && k < counted; i++) {
            if (frame_ok(ctx->records[i].status)) sizes[k++] = ctx->records[i].actual;
        }
        qsort(sizes, k, sizeof(uint32_t), compare_u32);

        printf("\nPacket size distribution\n");
        size_t i = 0;
        while (i < k) {
            size_t j = i;
            while (j < k && sizes[j] == sizes[i]) j++;
            size_t count = j - i;
            printf("  %4u bytes = %3u sample frames  %6zu packets  %5.1f%%\n",
                   sizes[i], sizes[i] / ctx->bytes_per_frame, count,
                   100.0 * (double)count / (double)k);
            i = j;
        }
        free(sizes);
    }

    if (cfg->trace_path) {
        FILE* f = fopen(cfg->trace_path, "w");
        if (!f) { perror("fopen"); return; }
        fprintf(f, "# E-MU Tracker Pre isochronous capture trace\n");
        fprintf(f, "# nominal_rate_hz=%u alt=%u bInterval=%u bytes_per_frame=%u\n",
                cfg->sample_rate, alt, interval, ctx->bytes_per_frame);
        fprintf(f, "usb_frame,requested_bytes,actual_bytes,sample_frames,status\n");
        for (size_t r = 0; r < n; r++) {
            const PacketRecord* p = &ctx->records[r];
            fprintf(f, "%llu,%u,%u,%u,%d\n",
                    (unsigned long long)p->frame, p->requested, p->actual,
                    p->actual / ctx->bytes_per_frame, p->status);
        }
        fclose(f);
        printf("\nwrote %zu packet records to %s\n", n, cfg->trace_path);
    }
}

/* ------------------------------------------------------------------- run */

int emu_capture_run(IOUSBDeviceInterface500** dev,
                    const EmuDeviceModel* model,
                    const CaptureConfig* cfg)
{
    /* Pick the capture alt setting that matches the requested rate. */
    const EmuAltSetting* chosen = NULL;
    for (uint16_t i = 0; i < model->num_alt_settings; i++) {
        const EmuAltSetting* a = &model->alt_settings[i];
        if (a->data_endpoint == 0) continue;
        if ((a->data_endpoint & 0x80) == 0) continue;      /* capture only */
        if (a->sample_rate != cfg->sample_rate) continue;
        if (cfg->prefer_interval && a->interval != cfg->prefer_interval) continue;
        /* Prefer the longest service interval available: fewer, larger packets
         * are easier to keep up with from userspace. */
        if (!chosen || a->interval > chosen->interval) chosen = a;
    }
    if (!chosen) {
        fprintf(stderr, "error: no capture alt setting for %u Hz", cfg->sample_rate);
        if (cfg->prefer_interval) fprintf(stderr, " at bInterval %d", cfg->prefer_interval);
        fprintf(stderr, "\n");
        return 1;
    }

    printf("capture: %u Hz, interface %u alt %u, bInterval %u, wMaxPacketSize %u\n",
           cfg->sample_rate, chosen->interface_number, chosen->alternate_setting,
           chosen->interval, chosen->max_packet_size);

    IOUSBInterfaceInterface500** intf = NULL;
    if (!find_capture_interface(dev, chosen->interface_number, &intf)) return 1;

    int rc = 1;
    bool interface_open = false;
    /* Declared before the first `goto cleanup` so the cleanup path never reads
     * an uninitialised value. */
    CFRunLoopSourceRef source = NULL;
    size_t allocated = 0;
    CaptureCtx ctx;
    memset(&ctx, 0, sizeof ctx);

    if ((*intf)->USBInterfaceOpen(intf) != kIOReturnSuccess) {
        fprintf(stderr, "error: could not open interface %u\n", chosen->interface_number);
        goto cleanup;
    }
    interface_open = true;

    /* Return to the zero-bandwidth setting first so the transition always
     * starts from a known state rather than from whatever was left selected. */
    (*intf)->SetAlternateInterface(intf, 0);

    IOReturn kr = (*intf)->SetAlternateInterface(intf, chosen->alternate_setting);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: SetAlternateInterface(%u) failed: 0x%08x\n",
                chosen->alternate_setting, kr);
        if (kr == kIOReturnTimeout) {
            fprintf(stderr,
                "       The device did not answer SET_INTERFACE. This usually means the\n"
                "       active clock rate disagrees with this alt setting's tSamFreq.\n"
                "       A device left in this state stops answering control transfers\n"
                "       entirely and needs a physical replug.\n");
        }
        goto cleanup;
    }

    ctx.intf = intf;
    ctx.bytes_per_frame = chosen->channels * chosen->subframe_size;
    if (ctx.bytes_per_frame == 0) ctx.bytes_per_frame = 6;

    if (!find_isoc_in_pipe(intf, &ctx.pipe_ref, &ctx.max_packet_size)) {
        fprintf(stderr, "error: no isochronous IN pipe on alt %u\n",
                chosen->alternate_setting);
        goto cleanup;
    }
    /* period = 2^(bInterval-1) microframes; 8 microframes per 1 ms frame. */
    uint32_t period_microframes = 1u << (chosen->interval - 1);
    ctx.transactions_per_frame = (uint8_t)(period_microframes >= 8
                                           ? 1 : (8 / period_microframes));
    ctx.service_interval_ms = (double)period_microframes / 8.0;

    printf("        isoc IN pipe %u, wMaxPacketSize %u, service interval %.2f ms\n",
           ctx.pipe_ref, ctx.max_packet_size, ctx.service_interval_ms);
    if (ctx.transactions_per_frame > 1) {
        printf("        note: %u transactions per frame; this probe observes one of\n"
               "        them, so the trace is a sampled subset of the audio stream\n",
               ctx.transactions_per_frame);
    }

    if ((*intf)->CreateInterfaceAsyncEventSource(intf, &source) != kIOReturnSuccess) {
        fprintf(stderr, "error: CreateInterfaceAsyncEventSource failed\n");
        goto cleanup;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);

    ctx.record_capacity = (size_t)cfg->duration_ms * 2 + FRAMES_PER_REQUEST * NUM_REQUESTS;
    ctx.records = calloc(ctx.record_capacity, sizeof(PacketRecord));
    if (!ctx.records) { fprintf(stderr, "error: out of memory\n"); goto cleanup; }

    UInt64 current = 0;
    AbsoluteTime at;
    if ((*intf)->GetBusFrameNumber(intf, &current, &at) != kIOReturnSuccess) {
        fprintf(stderr, "error: GetBusFrameNumber failed\n");
        goto cleanup;
    }
    ctx.next_frame = current + SCHEDULE_MARGIN;

    for (int i = 0; i < NUM_REQUESTS; i++) {
        Request* req = &ctx.requests[i];
        req->ctx = &ctx;
        req->buffer = calloc(FRAMES_PER_REQUEST, ctx.max_packet_size);
        req->frames = calloc(FRAMES_PER_REQUEST, sizeof(IOUSBIsocFrame));
        if (!req->buffer || !req->frames) {
            fprintf(stderr, "error: out of memory\n");
            goto cleanup;
        }
        allocated++;
    }

    for (int i = 0; i < NUM_REQUESTS; i++) {
        IOReturn sk = submit(&ctx.requests[i]);
        if (sk != kIOReturnSuccess) {
            fprintf(stderr, "error: initial submit %d failed: 0x%08x\n", i, sk);
            goto cleanup;
        }
    }

    printf("        streaming for %u ms...\n", cfg->duration_ms);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, cfg->duration_ms / 1000.0 + 0.5, false);

    /* Let outstanding requests drain rather than tearing down underneath them. */
    ctx.stopping = true;
    (*intf)->AbortPipe(intf, ctx.pipe_ref);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);

    report(&ctx, cfg, chosen->alternate_setting, chosen->interval);
    rc = 0;

cleanup:
    if (source) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
        CFRelease(source);
    }
    for (size_t i = 0; i < allocated; i++) {
        free(ctx.requests[i].buffer);
        free(ctx.requests[i].frames);
    }
    free(ctx.records);
    if (interface_open) {
        /* Alt 0 is the zero-bandwidth setting; leaving a streaming alt selected
         * would keep the device's isochronous bandwidth reserved. */
        (*intf)->SetAlternateInterface(intf, 0);
        (*intf)->USBInterfaceClose(intf);
    }
    (*intf)->Release(intf);
    return rc;
}
