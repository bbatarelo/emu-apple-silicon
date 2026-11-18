/* Issue #7: exercise the production completion/submit handlers against an
 * in-memory USB interface. No device is opened and no driver is installed.
 * Including the implementation keeps the static handlers under test real.
 * A queued request owns its buffers until its simulated completion is delivered.
 */
#include <assert.h>
#include "../../driver/usb_engine.c"

static unsigned submissions, duplicate_submissions;
static Request* owned[2 * MAX_REQUESTS];
static unsigned owned_count;
static unsigned failed;
static CFRunLoopTimerRef abort_timer;
static bool stale_always;
static void complete(Request* req, bool capture, IOReturn result);
static void release_timer(void);

static bool is_capture(Request* r)
{
    for (unsigned i = 0; i < r->engine->num_requests; i++)
        if (r == &r->engine->in_requests[i]) return true;
    return false;
}

static void deliver_aborts(CFRunLoopTimerRef timer, void* context)
{
    (void)timer; (void)context;
    unsigned count = owned_count;
    Request* snapshot[2 * MAX_REQUESTS];
    memcpy(snapshot, owned, count * sizeof(*owned));
    for (unsigned i = 0; i < count; i++)
        complete(snapshot[i], is_capture(snapshot[i]), kIOReturnAborted);
}

static IOReturn fake_abort(void* self, UInt8 pipe)
{
    (void)self; (void)pipe;
    if (abort_timer && !CFRunLoopTimerIsValid(abort_timer)) release_timer();
    if (!abort_timer) {
        /* Longer than the old 50 ms drain, dispatched on the real CF run loop. */
        abort_timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.075,
                           0, 0, 0, deliver_aborts, NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), abort_timer, kCFRunLoopDefaultMode);
    }
    return kIOReturnSuccess;
}

static IOReturn fake_bus_time(void* self, UInt64* frame, AbsoluteTime* at)
{
    (void)self;
    *frame = 1000;
    uint64_t now = mach_absolute_time();
    at->hi = (uint32_t)(now >> 32); at->lo = (uint32_t)now;
    return kIOReturnSuccess;
}

static void release_timer(void)
{
    if (abort_timer) { CFRunLoopTimerInvalidate(abort_timer); CFRelease(abort_timer); }
    abort_timer = NULL;
}


static IOReturn fake_submit(void* self, UInt8 pipe, void* buffer, UInt64 start,
                           UInt32 count, UInt32 update,
                           IOUSBLowLatencyIsocFrame* frames,
                           IOAsyncCallback1 callback, void* refcon)
{
    (void)self; (void)pipe; (void)buffer; (void)start;
    (void)count; (void)update; (void)frames; (void)callback;
    submissions++;
    Request* request = refcon;
    if (stale_always && request == &request->engine->out_requests[1])
        return kIOReturnIsoTooOld;

    for (unsigned i = 0; i < owned_count; i++) {
        if (owned[i] == refcon) {
            duplicate_submissions++;
            return kIOReturnBusy;
        }
    }
    assert(owned_count < sizeof(owned) / sizeof(*owned));
    owned[owned_count++] = refcon;
    return kIOReturnSuccess;
}

static void complete(Request* req, bool capture, IOReturn result)
{
    unsigned i;
    for (i = 0; i < owned_count && owned[i] != req; i++) {}
    assert(i < owned_count);
    owned[i] = owned[--owned_count];
    for (unsigned j = 0; j < req->engine->entries_per_request; j++) {
        req->frames[j].frStatus = result;
        req->frames[j].frActCount = result == kIOReturnSuccess
                                     ? req->frames[j].frReqCount : 0;
    }
    if (req == &req->engine->fb_requests[0]) feedback_complete(req, result, NULL);
    else if (capture) capture_complete(req, result, NULL);
    else playback_complete(req, result, NULL);
}

static Engine* fixture(bool capture)
{
    static IOUSBInterfaceInterface500 vtable;
    static IOUSBInterfaceInterface500* interface = &vtable;
    vtable.LowLatencyReadIsochPipeAsync = fake_submit;
    vtable.LowLatencyWriteIsochPipeAsync = fake_submit;
    vtable.AbortPipe = fake_abort;
    vtable.GetBusFrameNumber = fake_bus_time;
    Engine* e = emu_engine_create(0x3f04, 0);
    assert(e);
    submissions = duplicate_submissions = owned_count = 0;
    stale_always = false;
    release_timer();
    e->out_intf = &interface;
    e->in_intf = capture ? &interface : NULL;
    e->num_requests = 2;
    e->out_pipe = 1; e->in_pipe = 1;
    e->entries_per_ms = 1;
    e->entries_per_request = 2;
    e->nominal_frames = 48;
    e->sample_rate = 48000;
    e->bytes_per_frame = 6;
    e->out_max = e->in_max = 288;
    e->next_in_frame = e->next_out_frame = 1000;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    e->ticks_per_ms = 1000000ull * tb.denom / tb.numer;
    e->feedback = emu_feedback_init(e->feedback_storage);
    e->ts_filter = emu_ts_filter_init(e->ts_filter_storage,
                                      mach_absolute_time(), REQUEST_MS * e->ticks_per_ms);
    assert(e->feedback && e->ts_filter);
    emu_feedback_set_nominal(e->feedback, 48000, 1000000);
    for (unsigned i = 0; i < e->num_requests; i++) {
        Request* requests[] = { &e->out_requests[i], &e->in_requests[i] };
        for (unsigned j = 0; j < 2; j++) {
            requests[j]->engine = e;
            requests[j]->buffer = calloc(2, 288);
            requests[j]->frames = calloc(2, sizeof(IOUSBLowLatencyIsocFrame));
            assert(requests[j]->buffer && requests[j]->frames);
        }
    }
    return e;
}

static void dispose(Engine* e)
{
    /* No real controller owns these allocations. Discard the fake queue; do
     * not call hardware teardown on the deliberately incomplete vtable. */
    owned_count = 0;
    release_timer();
    for (unsigned i = 0; i < e->num_requests; i++) {
        free(e->out_requests[i].buffer); free(e->out_requests[i].frames);
        free(e->in_requests[i].buffer); free(e->in_requests[i].frames);
    }
    free(e->fb_requests[0].buffer); free(e->fb_requests[0].frames);
    e->out_intf = e->in_intf = NULL;
    emu_engine_destroy(e);
}

static void check(bool ok, const char* description)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) failed++;
}

static void abort_case(bool capture, bool stopping, bool faulted)
{
    Engine* e = fixture(capture);
    Request* r = capture ? &e->in_requests[0] : &e->out_requests[0];
    IOReturn kr = capture ? submit_capture(r) : submit_playback(r);
    assert(kr == kIOReturnSuccess);
    atomic_store(&e->stopping, stopping);
    atomic_store(&e->faulted, faulted);
    uint64_t before = capture ? atomic_load(&e->frames_captured)
                              : atomic_load(&e->frames_played);
    printf("\n%s abort: stopping=%d, faulted=%d\n",
           capture ? "Capture" : "Playback", stopping, faulted);
    /* The controller returns an entirely cancelled, never-played request. */
    complete(r, capture, kIOReturnAborted);
    uint64_t after = capture ? atomic_load(&e->frames_captured)
                             : atomic_load(&e->frames_played);
    printf("  observed: frame count +%llu, callback submissions %u, pending %u\n",
           (unsigned long long)(after - before), submissions - 1, owned_count);
    check(after == before, "cancelled future intervals do not advance the audio timeline");
    check(submissions == 1, "abort completion does not restart the cancelled request");
    if (!stopping && !faulted) {
        /* This is the next action of the startup retry's queue loop: submit
         * slot zero again. The mock detects reuse while the callback's new
         * transfer still owns the same Request and frame-list buffer. */
        (void)(capture ? submit_capture(r) : submit_playback(r));
        check(duplicate_submissions == 0, "next startup submission does not reuse an owned request");
    }
    dispose(e);
}

static void startup_retry(bool capture)
{
    Engine* e = fixture(capture);
    const uint64_t resume = 123456;
    atomic_store(&e->frames_played, resume);
    emu_engine_inject_fault(e, EMU_FAULT_STARTUP_STALE);
    CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
    IOReturn kr = queue_initial_requests(e, resume);
    printf("\nDelayed startup retry (%s)\n", capture ? "duplex" : "playback only");
    check(kr == kIOReturnSuccess, "one-shot stale startup recovers");
    check(CFAbsoluteTimeGetCurrent() - start >= 0.070,
          "retry waits for callbacks delayed beyond the old 50 ms timeout");
    check(e->schedule_lead_ms == 16, "adaptive startup lead is preserved");
    check(e->out_requests[0].data_frame_start == resume &&
          e->out_cursor == resume + 192 && atomic_load(&e->frames_played) == resume,
          "rebuilt map and frame anchor share the original resume point");
    check(atomic_load(&e->timeline_frames) == resume && e->in_cursor == resume,
          "cancelled callbacks did not advance either cursor or the published timeline");
    check(duplicate_submissions == 0 && e->pending_requests == owned_count &&
          owned_count == (capture ? 4u : 2u), "rebuilt queue owns each request exactly once");
    check(atomic_load(&e->fault_mode) == EMU_FAULT_NONE,
          "startup fault is consumed once");
    release_timer();
    quiesce(e);
    check(owned_count == 0 && e->pending_requests == 0,
          "final drain returns every outstanding request");
    dispose(e);
}

static void ownership_guard(void)
{
    Engine* e = fixture(false);
    Request* r = &e->out_requests[0];
    assert(submit_playback(r) == kIOReturnSuccess);
    memset(r->buffer, 0x5a, 576);
    uint64_t cursor = e->out_cursor;
    puts("\nOutstanding request guard");
    check(submit_playback(r) == kIOReturnBusy && submissions == 1 &&
          e->out_cursor == cursor && ((uint8_t*)r->buffer)[0] == 0x5a,
          "duplicate submit is refused before buffer writes or timeline changes");
    quiesce(e);
    dispose(e);
}

static void exhausted_retries(void)
{
    Engine* e = fixture(true);
    stale_always = true;
    puts("\nExhausted setup retries");
    check(queue_initial_requests(e, 0) == kIOReturnIsoTooOld,
          "persistent stale startup returns failure after bounded retries");
    check(e->schedule_lead_ms == 256 && duplicate_submissions == 0,
          "all four attempts preserve exclusive request ownership");
    quiesce(e);
    check(e->pending_requests == 0 && owned_count == 0,
          "partial queue on final failure drains before cleanup");
    dispose(e);
}

static void feedback_drain(void)
{
    Engine* e = fixture(false);
    e->fb_pipe = e->fb_abort_pipe = 2;
    e->fb_entries_per_request = 2;
    e->fb_max = 4;
    Request* r = &e->fb_requests[0];
    r->engine = e;
    r->buffer = calloc(2, 4);
    r->frames = calloc(2, sizeof(*r->frames));
    puts("\nFeedback drain");
    check(submit_feedback(r) == kIOReturnSuccess, "feedback request is tracked");
    e->fb_pipe = 0; /* disabled after another feedback submission failed */
    quiesce(e);
    check(owned_count == 0 && e->pending_requests == 0 && submissions == 1,
          "disabled feedback endpoint still returns outstanding ownership");
    dispose(e);
}

static void partial_setup_cleanup(void)
{
    Engine* e = emu_engine_create(0x3f04, 0);
    CFRunLoopSourceContext context = {0};
    e->out_source = CFRunLoopSourceCreate(NULL, 0, &context);
    assert(e->out_source && !e->run_loop);
    teardown(e); /* capture source creation failed before run_loop was set */
    check(!e->out_source, "partial setup releases source without a published run loop");
    emu_engine_inject_fault(e, EMU_FAULT_STARTUP_STALE);
    EmuEngineStats stats;
    emu_engine_stats(e, &stats);
    check(stats.fault_mode == EMU_FAULT_STARTUP_STALE,
          "idle diagnostics report the armed startup fault");
    emu_engine_destroy(e);
}

int main(void)
{
    Engine* e = fixture(false);
    assert(submit_playback(&e->out_requests[0]) == kIOReturnSuccess);
    complete(&e->out_requests[0], false, kIOReturnSuccess);
    puts("Normal completion control");
    check(atomic_load(&e->frames_played) == 96 && submissions == 2,
          "successful playback accounts frames and replenishes the queue");
    dispose(e);
    for (unsigned capture = 0; capture < 2; capture++) {
        abort_case(capture, false, false); /* startup retry */
        abort_case(capture, false, true);  /* recovery drain */
        abort_case(capture, true, false);  /* ordinary StopIO */
    }
    partial_setup_cleanup();
    ownership_guard();
    startup_retry(false);
    startup_retry(true);
    exhausted_retries();
    feedback_drain();
    printf("\n%u invariant failures.\n", failed);
    return failed ? 1 : 0;
}
