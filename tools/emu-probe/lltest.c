/*
 * Diagnostic: how does the low-latency isochronous API distribute data across
 * frame-list entries?
 *
 * The classic ReadIsochPipeAsync gives one entry per USB frame, which cannot
 * express a bInterval 3 endpoint being serviced twice per frame. E-MU's driver
 * uses the low-latency API with kNumberOfFramesPerMillisecond of 8, implying
 * entries are microframes. This measures what actually arrives rather than
 * assuming either model.
 *
 * Read-only: it captures, prints, and restores alt 0.
 */

#include "lltest.h"
#include "../../shared/usb_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

/* One millisecond is 8 microframes. If entries are microframes, 64 entries is
 * 8 ms; if they are frames, 64 entries is 64 ms. The arrival rate tells us
 * which, without having to trust either interpretation. */
#define ENTRIES  64

typedef struct {
    IOUSBInterfaceInterface500** intf;
    IOUSBLowLatencyIsocFrame*    list;
    void*                        data;
    bool                         done;
    IOReturn                     result;
} LLProbe;

static void ll_complete(void* refcon, IOReturn result, void* arg0)
{
    (void)arg0;
    LLProbe* probe = (LLProbe*)refcon;
    probe->result = result;
    probe->done = true;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

int emu_lowlatency_probe(IOUSBDeviceInterface500** dev,
                         const EmuDeviceModel* model,
                         uint32_t rate)
{
    const EmuAltSetting* alt = NULL;
    for (uint16_t i = 0; i < model->num_alt_settings; i++) {
        const EmuAltSetting* a = &model->alt_settings[i];
        if (a->interface_number != 2 || a->data_endpoint == 0) continue;
        if (a->sample_rate != rate) continue;
        if (!alt || a->interval > alt->interval) alt = a;
    }
    if (!alt) { fprintf(stderr, "error: no capture alt for %u Hz\n", rate); return 1; }

    uint32_t period = 1u << (alt->interval - 1);
    printf("low-latency probe: %u Hz, alt %u, bInterval %u (period %u microframes)\n",
           rate, alt->alternate_setting, alt->interval, period);
    printf("  expected per microframe: %.2f frames = %.1f bytes\n",
           rate / 8000.0, rate / 8000.0 * 6.0);
    printf("  expected per %u-microframe service interval: %.2f frames = %.1f bytes\n",
           period, rate * period / 8000.0, rate * period / 8000.0 * 6.0);

    IOUSBInterfaceInterface500** intf = NULL;
    if (!emu_find_interface(dev, 2, &intf)) return 1;

    int rc = 1;
    bool opened = false;
    LLProbe probe;
    memset(&probe, 0, sizeof probe);
    CFRunLoopSourceRef source = NULL;

    if ((*intf)->USBInterfaceOpen(intf) != kIOReturnSuccess) {
        fprintf(stderr, "error: could not open capture interface\n");
        goto cleanup;
    }
    opened = true;

    (*intf)->SetAlternateInterface(intf, 0);
    IOReturn kr = (*intf)->SetAlternateInterface(intf, alt->alternate_setting);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: SetAlternateInterface(%u): 0x%08x\n",
                alt->alternate_setting, kr);
        goto cleanup;
    }

    uint8_t pipe = 0;
    uint16_t max_packet = 0;
    if (!emu_find_isoc_pipe(intf, kUSBIn, &pipe, &max_packet)) {
        fprintf(stderr, "error: no isoc IN pipe\n");
        goto cleanup;
    }
    printf("  pipe %u, wMaxPacketSize %u\n\n", pipe, max_packet);

    if ((*intf)->CreateInterfaceAsyncEventSource(intf, &source) != kIOReturnSuccess) {
        fprintf(stderr, "error: async event source\n");
        goto cleanup;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);

    /* Low-latency transfers require buffers allocated through the interface, so
     * they can be shared with the kernel without copying. */
    kr = (*intf)->LowLatencyCreateBuffer(intf, &probe.data,
                                         (UInt32)ENTRIES * max_packet,
                                         kUSBLowLatencyReadBuffer);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: LowLatencyCreateBuffer(data): 0x%08x\n", kr);
        goto cleanup;
    }
    kr = (*intf)->LowLatencyCreateBuffer(intf, (void**)&probe.list,
                                         ENTRIES * sizeof(IOUSBLowLatencyIsocFrame),
                                         kUSBLowLatencyFrameListBuffer);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: LowLatencyCreateBuffer(list): 0x%08x\n", kr);
        goto cleanup;
    }

    for (int i = 0; i < ENTRIES; i++) {
        /* The API marks entries with this key and overwrites it on completion,
         * which is how E-MU's driver detects which entries have landed. */
        probe.list[i].frStatus   = kUSBLowLatencyIsochTransferKey;
        probe.list[i].frReqCount = max_packet;
        probe.list[i].frActCount = 0;
    }
    probe.intf = intf;

    UInt64 frame = 0;
    AbsoluteTime at;
    if ((*intf)->GetBusFrameNumber(intf, &frame, &at) != kIOReturnSuccess) {
        fprintf(stderr, "error: GetBusFrameNumber\n");
        goto cleanup;
    }

    uint64_t start_ms = (uint64_t)(CFAbsoluteTimeGetCurrent() * 1000.0);

    kr = (*intf)->LowLatencyReadIsochPipeAsync(
        intf, pipe, probe.data, frame + 16, ENTRIES,
        1 /* updateFrequency, ms */, probe.list, ll_complete, &probe);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: LowLatencyReadIsochPipeAsync: 0x%08x\n", kr);
        goto cleanup;
    }

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2.0, false);
    uint64_t elapsed_ms = (uint64_t)(CFAbsoluteTimeGetCurrent() * 1000.0) - start_ms;

    if (!probe.done) {
        fprintf(stderr, "error: request never completed\n");
        goto cleanup;
    }

    printf("request of %d entries completed in ~%llu ms (result 0x%08x)\n",
           ENTRIES, (unsigned long long)elapsed_ms, probe.result);
    printf("  -> %.2f entries per millisecond\n\n",
           elapsed_ms ? (double)ENTRIES / (double)elapsed_ms : 0.0);

    printf("entry  actual  frames  status\n");
    uint32_t nonempty = 0, total_bytes = 0;
    for (int i = 0; i < ENTRIES; i++) {
        uint32_t act = probe.list[i].frActCount;
        if (act) { nonempty++; total_bytes += act; }
        if (i < 24) {
            printf("  %2d   %5u   %5.2f  0x%08x %s\n", i, act, act / 6.0,
                   (uint32_t)probe.list[i].frStatus,
                   emu_isoc_status_name(probe.list[i].frStatus));
        }
    }
    printf("  ...\n");
    printf("\n%u of %d entries carried data, %u bytes total (%.1f frames)\n",
           nonempty, ENTRIES, total_bytes, total_bytes / 6.0);
    if (nonempty) {
        printf("mean %.1f bytes per non-empty entry\n",
               (double)total_bytes / (double)nonempty);
    }
    rc = 0;

cleanup:
    if (probe.data) (*intf)->LowLatencyDestroyBuffer(intf, probe.data);
    if (probe.list) (*intf)->LowLatencyDestroyBuffer(intf, probe.list);
    if (source) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
        CFRelease(source);
    }
    if (opened) {
        (*intf)->SetAlternateInterface(intf, 0);
        (*intf)->USBInterfaceClose(intf);
    }
    (*intf)->Release(intf);
    return rc;
}
