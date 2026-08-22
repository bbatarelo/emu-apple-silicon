/*
 * MIDI transport probe. See midi.h.
 *
 * Everything here is synchronous: bulk transfers with timeouts, polled in a
 * loop. MIDI's data rate (31.25 kbaud on the DIN side) is glacial by USB
 * standards, so there is nothing to schedule -- the interesting questions are
 * only whether the pipes move bytes and whether the packet framing is right,
 * and the Rust codec owns the framing.
 */

#include "midi.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include "../../shared/usb_util.h"

typedef struct {
    IOUSBInterfaceInterface500** intf;
    bool     open;
    uint8_t  in_pipe, out_pipe;
    uint16_t in_max_packet, out_max_packet;
} MidiPipes;

static void midi_release(MidiPipes* p)
{
    if (p->intf) {
        if (p->open) (*p->intf)->USBInterfaceClose(p->intf);
        (*p->intf)->Release(p->intf);
    }
    memset(p, 0, sizeof *p);
}

static bool midi_claim(IOUSBDeviceInterface500** dev,
                       const EmuDeviceModel* model, MidiPipes* p)
{
    memset(p, 0, sizeof *p);

    if (model->midi_interface == 0xff) {
        fprintf(stderr, "error: this device advertises no MIDI interface\n");
        return false;
    }
    if (!emu_find_interface(dev, model->midi_interface, &p->intf)) return false;

    IOReturn kr = (*p->intf)->USBInterfaceOpen(p->intf);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: USBInterfaceOpen(midi): 0x%08x\n", kr);
        if (kr == kIOReturnExclusiveAccess) {
            fprintf(stderr,
                    "       Something else holds the MIDI interface -- probably the\n"
                    "       installed CoreMIDI driver. Use midi-check instead, or\n"
                    "       'make uninstall-midi' to probe the raw interface.\n");
        }
        midi_release(p);
        return false;
    }
    p->open = true;

    /* The MIDI interface has a single alternate setting, so its pipes exist
     * as soon as it is open -- no SetAlternateInterface dance. */
    if (!emu_find_bulk_pipe(p->intf, kUSBIn, &p->in_pipe, &p->in_max_packet) ||
        !emu_find_bulk_pipe(p->intf, kUSBOut, &p->out_pipe, &p->out_max_packet)) {
        fprintf(stderr, "error: MIDI bulk pipes not found\n");
        midi_release(p);
        return false;
    }
    return true;
}

static uint64_t now_ms(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000000ull;
}

static const char* message_name(uint8_t status)
{
    switch (status & 0xf0) {
        case 0x80: return "note off";
        case 0x90: return "note on";
        case 0xa0: return "poly aftertouch";
        case 0xb0: return "control change";
        case 0xc0: return "program change";
        case 0xd0: return "channel pressure";
        case 0xe0: return "pitch bend";
    }
    switch (status) {
        case 0xf0: return "sysex start";
        case 0xf1: return "mtc quarter frame";
        case 0xf2: return "song position";
        case 0xf3: return "song select";
        case 0xf6: return "tune request";
        case 0xf7: return "sysex end";
        case 0xf8: return "clock";
        case 0xfa: return "start";
        case 0xfb: return "continue";
        case 0xfc: return "stop";
        case 0xfe: return "active sensing";
        case 0xff: return "reset";
    }
    return "?";
}

static void print_bytes(const uint8_t* bytes, uint32_t n)
{
    printf("  ");
    for (uint32_t i = 0; i < n; i++) printf("%02x ", bytes[i]);
    if (n > 0) printf("  %s", message_name(bytes[0]));
    printf("\n");
}

/* Reads whatever is pending, decodes it, optionally copies the MIDI bytes to
 * `sink` (up to sink_cap), and returns how many bytes arrived. Returns -1 on a
 * transport error. A timeout is not an error; MIDI is idle almost always. */
static int drain_input(MidiPipes* p, uint32_t wait_ms,
                       uint8_t* sink, uint32_t sink_cap, uint32_t* sink_len,
                       bool print)
{
    uint8_t buffer[512];
    UInt32 size = sizeof buffer;

    IOReturn kr = (*p->intf)->ReadPipeTO(p->intf, p->in_pipe, buffer, &size,
                                         wait_ms, wait_ms);
    if (kr == kIOUSBTransactionTimeout || kr == kIOReturnAborted) return 0;
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: ReadPipeTO: 0x%08x (%s)\n",
                kr, emu_isoc_status_name((int32_t)kr));
        return -1;
    }

    int total = 0;
    for (UInt32 off = 0; off + 4 <= size; off += 4) {
        uint8_t decoded[3];
        uint32_t n = emu_midi_decode(buffer + off, decoded);
        if (n == 0) continue;
        if (print) print_bytes(decoded, n);
        if (sink && sink_len) {
            for (uint32_t i = 0; i < n && *sink_len < sink_cap; i++) {
                sink[(*sink_len)++] = decoded[i];
            }
        }
        total += (int)n;
    }
    return total;
}

int emu_midi_dump(IOUSBDeviceInterface500** dev,
                  const EmuDeviceModel* model, uint32_t duration_ms)
{
    MidiPipes pipes;
    if (!midi_claim(dev, model, &pipes)) return 1;

    printf("MIDI interface %u: IN 0x%02x, OUT 0x%02x, %u ms. Play something\n"
           "into the DIN MIDI IN port...\n\n",
           model->midi_interface, model->midi_in_endpoint,
           model->midi_out_endpoint, duration_ms);

    int rc = 0;
    uint64_t deadline = now_ms() + duration_ms;
    uint32_t bytes_seen = 0;
    while (now_ms() < deadline) {
        int n = drain_input(&pipes, 250, NULL, 0, NULL, true);
        if (n < 0) { rc = 1; break; }
        bytes_seen += (uint32_t)n;
    }
    printf("\n%u MIDI bytes received\n", bytes_seen);

    midi_release(&pipes);
    return rc;
}

/* Encodes a byte stream into event packets and writes them in one transfer.
 * Returns the packet count, or -1. */
static int send_stream(MidiPipes* p, const uint8_t* bytes, uint32_t len)
{
    /* Storage for the Rust encoder, aligned generously. */
    static uint8_t enc_storage[256] __attribute__((aligned(16)));
    if (emu_midi_encoder_size() > sizeof enc_storage) {
        fprintf(stderr, "error: encoder storage too small\n");
        return -1;
    }
    EmuMidiEncoder* enc = emu_midi_encoder_init(enc_storage, 0);
    if (!enc) return -1;

    uint8_t out[512];
    uint32_t out_len = 0;
    int packets = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (emu_midi_encode(enc, bytes[i], out + out_len)) {
            out_len += 4;
            packets++;
            if (out_len + 4 > sizeof out) break;
        }
    }
    if (out_len == 0) {
        fprintf(stderr, "error: nothing to send (incomplete MIDI message?)\n");
        return -1;
    }

    IOReturn kr = (*p->intf)->WritePipeTO(p->intf, p->out_pipe, out, out_len,
                                          1000, 1000);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: WritePipeTO: 0x%08x\n", kr);
        return -1;
    }
    return packets;
}

int emu_midi_send(IOUSBDeviceInterface500** dev,
                  const EmuDeviceModel* model,
                  const uint8_t* bytes, uint32_t len)
{
    MidiPipes pipes;
    if (!midi_claim(dev, model, &pipes)) return 1;

    int packets = send_stream(&pipes, bytes, len);
    if (packets > 0) {
        printf("sent %u MIDI bytes as %d event packets to the DIN MIDI OUT\n",
               len, packets);
    }

    midi_release(&pipes);
    return packets > 0 ? 0 : 1;
}

int emu_midi_loopback(IOUSBDeviceInterface500** dev, const EmuDeviceModel* model)
{
    /* One of everything the encoder distinguishes: channel messages of both
     * data lengths, running status, system common, real-time, and a SysEx
     * long enough to span packets. */
    static const uint8_t test[] = {
        0x90, 0x3c, 0x40,             /* note on */
        0x3e, 0x40,                   /* ...running status */
        0x80, 0x3c, 0x00,             /* note off */
        0xb0, 0x07, 0x64,             /* control change */
        0xc2, 0x14,                   /* program change */
        0xe0, 0x00, 0x40,             /* pitch bend */
        0xf8,                         /* clock */
        0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7,  /* sysex: identity request */
        0x90, 0x40, 0x40,             /* note on */
        0x80, 0x40, 0x00,             /* note off */
    };
    /* What must come back: the same stream with running status expanded,
     * because every event packet carries its status byte. */
    static const uint8_t expect[] = {
        0x90, 0x3c, 0x40,
        0x90, 0x3e, 0x40,
        0x80, 0x3c, 0x00,
        0xb0, 0x07, 0x64,
        0xc2, 0x14,
        0xe0, 0x00, 0x40,
        0xf8,
        0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7,
        0x90, 0x40, 0x40,
        0x80, 0x40, 0x00,
    };

    MidiPipes pipes;
    if (!midi_claim(dev, model, &pipes)) return 1;

    printf("loopback: sending %zu bytes out the DIN MIDI OUT...\n", sizeof test);

    int rc = 1;
    if (send_stream(&pipes, test, sizeof test) > 0) {
        /* 28 bytes at 31.25 kbaud (10 bits per byte on the wire) is ~9 ms;
         * two seconds is generous even with slow device buffering. */
        uint8_t back[256];
        uint32_t back_len = 0;
        uint64_t deadline = now_ms() + 2000;
        while (now_ms() < deadline && back_len < sizeof expect) {
            if (drain_input(&pipes, 250, back, sizeof back, &back_len, false) < 0) {
                break;
            }
        }

        if (back_len == sizeof expect && memcmp(back, expect, back_len) == 0) {
            printf("PASS: all %u bytes returned intact (%zu sent; the USB framing\n"
                   "      legitimately expands running status)\n",
                   back_len, sizeof test);
            rc = 0;
        } else if (back_len == 0) {
            fprintf(stderr,
                    "FAIL: nothing came back. Is a DIN cable connecting the\n"
                    "      device's MIDI OUT to its MIDI IN?\n");
        } else {
            fprintf(stderr, "FAIL: got %u bytes, expected %zu:\n",
                    back_len, sizeof expect);
            for (uint32_t i = 0; i < back_len; i++) {
                fprintf(stderr, "%02x ", back[i]);
            }
            fprintf(stderr, "\n");
        }
    }

    midi_release(&pipes);
    return rc;
}
