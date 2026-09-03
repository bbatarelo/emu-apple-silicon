/*
 * emu-probe -- userspace USB probe for E-MU CA0189 interfaces.
 *
 * Milestones 1 and 2 from EMU_Tracker_Pre_Development_Guidelines.md section 29:
 * descriptor topology, and clock rate control with SET_CUR -> GET_CUR
 * verification.
 *
 * Runs as an ordinary unprivileged process through IOKit's IOUSBLib. It needs
 * no DriverKit entitlement, no signing and no change to system security, which
 * is what makes this work possible while the entitlement request is pending.
 *
 * All descriptor parsing and request encoding lives in the Rust core; this file
 * is only the IOKit transport and the printing. That boundary is deliberate --
 * the parser here is the same one that goes into the dext later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include "../../rust/emu-ca0189/include/emu_ca0189.h"
#include "capture.h"
#include "duplex.h"
#include "lltest.h"

#include "../../shared/device.h"
#include "../../shared/usb_util.h"

#define EMU_VID EMU_VENDOR_ID

#define CONTROL_TIMEOUT_MS 1000

typedef IOUSBDeviceInterface500** UsbDevice;

/* ------------------------------------------------------------------ device */

/* Whichever device open_device found. The probe drives one at a time. */
static const EmuDeviceIdentity* gDevice = NULL;

static UsbDevice open_device(io_service_t* out_service, bool* out_opened)
{
    *out_opened = false;

    io_service_t service = IO_OBJECT_NULL;
    gDevice = emu_find_device(EMU_DEFAULT_PRODUCT_ID, &service);
    if (!gDevice) {
        fprintf(stderr,
                "error: no known E-MU device found on the USB bus.\n"
                "       Connect one and try again.\n");
        return NULL;
    }
    if (!gDevice->verified) {
        fprintf(stderr, "note: %s has not been verified against this driver\n",
                gDevice->name);
    }

    IOCFPlugInInterface** plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) {
        fprintf(stderr, "error: IOCreatePlugInInterfaceForService: 0x%08x\n", kr);
        IOObjectRelease(service);
        return NULL;
    }

    UsbDevice dev = NULL;
    HRESULT hr = (*plugin)->QueryInterface(plugin,
                        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&dev);
    (*plugin)->Release(plugin);
    if (hr || !dev) {
        fprintf(stderr, "error: QueryInterface(IOUSBDeviceInterfaceID500): 0x%lx\n", (long)hr);
        IOObjectRelease(service);
        return NULL;
    }

    *out_service = service;
    return dev;
}

/* Opening is only needed for control transfers, not for reading descriptors. */
static bool device_open(UsbDevice dev)
{
    kern_return_t kr = (*dev)->USBDeviceOpen(dev);
    if (kr == KERN_SUCCESS) {
        return true;
    }

    if (kr == kIOReturnExclusiveAccess) {
        fprintf(stderr, "note: device busy, retrying with USBDeviceOpenSeize\n");
        kr = (*dev)->USBDeviceOpenSeize(dev);
        if (kr == KERN_SUCCESS) {
            return true;
        }
    }

    fprintf(stderr, "error: could not open device: 0x%08x\n", kr);
    return false;
}

static IOReturn control_transfer(UsbDevice dev, const EmuControlSetup* setup, void* data)
{
    IOUSBDevRequestTO req;
    memset(&req, 0, sizeof req);
    req.bmRequestType    = setup->bm_request_type;
    req.bRequest         = setup->b_request;
    req.wValue           = setup->w_value;
    req.wIndex           = setup->w_index;
    req.wLength          = setup->w_length;
    req.pData            = data;
    req.noDataTimeout    = CONTROL_TIMEOUT_MS;
    req.completionTimeout = CONTROL_TIMEOUT_MS;

    return (*dev)->DeviceRequestTO(dev, &req);
}

/* -------------------------------------------------------------- milestone 1 */

static const char* endpoint_sync_type(uint8_t attributes)
{
    switch ((attributes >> 2) & 0x03) {
        case 0: return "no-sync";
        case 1: return "asynchronous";
        case 2: return "adaptive";
        default: return "synchronous";
    }
}

static const char* extension_code_name(uint16_t code)
{
    switch (code) {
        case EMU_XU_CLOCK_RATE:        return "clock rate";
        case EMU_XU_CLOCK_SOURCE:      return "clock source";
        case EMU_XU_DIGITAL_IO_STATUS: return "digital I/O status";
        case EMU_XU_DEVICE_OPTIONS:    return "device options";
        case EMU_XU_DIRECT_MONITORING: return "direct monitoring";
        case EMU_XU_METERING:          return "metering";
        default:                       return "unknown";
    }
}

/* The raw configuration descriptor, before anyone tries to make sense of it.
 * Kept separate from parsing so an unfamiliar device can still have its bytes
 * saved and sent on when the parser rejects them. */
static bool fetch_config(UsbDevice dev, const uint8_t** raw_out, uint16_t* raw_len_out)
{
    IOUSBConfigurationDescriptorPtr cfg = NULL;
    kern_return_t kr = (*dev)->GetConfigurationDescriptorPtr(dev, 0, &cfg);
    if (kr != KERN_SUCCESS || !cfg) {
        fprintf(stderr, "error: GetConfigurationDescriptorPtr: 0x%08x\n", kr);
        return false;
    }

    *raw_out = (const uint8_t*)cfg;
    *raw_len_out = OSSwapLittleToHostInt16(cfg->wTotalLength);
    return true;
}

static bool read_model(UsbDevice dev, EmuDeviceModel* model,
                       const uint8_t** raw_out, uint16_t* raw_len_out)
{
    const uint8_t* bytes = NULL;
    uint16_t total = 0;
    if (!fetch_config(dev, &bytes, &total)) return false;

    int32_t rc = emu_parse_config_descriptor(bytes, total, model);
    if (rc != 0) {
        fprintf(stderr, "error: descriptor parse failed with code %d\n", rc);
        return false;
    }

    if (raw_out) *raw_out = bytes;
    if (raw_len_out) *raw_len_out = total;
    return true;
}

static const EmuExtensionUnit* find_clock_unit(const EmuDeviceModel* m)
{
    for (uint8_t i = 0; i < m->num_extension_units; i++) {
        if (m->extension_units[i].extension_code == EMU_XU_CLOCK_RATE) {
            return &m->extension_units[i];
        }
    }
    fprintf(stderr, "error: device exposes no clock rate extension unit\n");
    return NULL;
}

static void print_model(const EmuDeviceModel* m)
{
    printf("Configuration\n");
    printf("  value              %u\n", m->configuration_value);
    printf("  interfaces         %u\n", m->num_interfaces);
    printf("  max power          %u mA\n", m->max_power_ma);
    printf("  control interface  %u\n", m->control_interface);
    if (m->status_endpoint) {
        printf("  status endpoint    0x%02x (interrupt IN)\n", m->status_endpoint);
    }

    printf("\nExtension units (%u)\n", m->num_extension_units);
    for (uint8_t i = 0; i < m->num_extension_units; i++) {
        const EmuExtensionUnit* xu = &m->extension_units[i];
        printf("  unit %-3u code 0x%04x  %-18s src=%u ch=%u controls=0x%02x\n",
               xu->unit_id, xu->extension_code, extension_code_name(xu->extension_code),
               xu->source_id, xu->channels, xu->controls);
    }

    printf("\nTerminals (%u)\n", m->num_terminals);
    for (uint8_t i = 0; i < m->num_terminals; i++) {
        const EmuTerminal* t = &m->terminals[i];
        printf("  id %-3u type 0x%04x  %-7s", t->terminal_id, t->terminal_type,
               t->is_input ? "input" : "output");
        if (t->is_input) printf(" channels=%u", t->channels);
        else             printf(" source=%u", t->source_id);
        printf("\n");
    }

    printf("\nStreaming alternate settings (%u)\n", m->num_alt_settings);
    printf("  iface alt  ep    dir  rate     ch bits bpf  maxpkt ival sync          feedback\n");
    for (uint16_t i = 0; i < m->num_alt_settings; i++) {
        const EmuAltSetting* a = &m->alt_settings[i];

        if (a->data_endpoint == 0) {
            printf("  %-5u %-4u --    --   zero-bandwidth\n",
                   a->interface_number, a->alternate_setting);
            continue;
        }

        char feedback[16];
        if (a->feedback_endpoint) snprintf(feedback, sizeof feedback, "0x%02x", a->feedback_endpoint);
        else                      snprintf(feedback, sizeof feedback, "--");

        printf("  %-5u %-4u 0x%02x  %-4s %-8u %-2u %-4u %-4u %-6u %-4u %-13s %s\n",
               a->interface_number, a->alternate_setting, a->data_endpoint,
               (a->data_endpoint & 0x80) ? "IN" : "OUT",
               a->sample_rate, a->channels, a->bit_resolution,
               a->channels * a->subframe_size,
               a->max_packet_size, a->interval,
               endpoint_sync_type(a->data_endpoint_attributes),
               feedback);
    }
}

static bool save_raw(const uint8_t* raw, uint16_t raw_len, const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) { perror("fopen"); return false; }
    fwrite(raw, 1, raw_len, f);
    fclose(f);
    printf("wrote %u raw bytes to %s\n", raw_len, path);
    return true;
}

static int cmd_descriptors(UsbDevice dev, const char* save_path)
{
    const uint8_t* raw = NULL;
    uint16_t raw_len = 0;
    if (!fetch_config(dev, &raw, &raw_len)) return 1;

    printf("%s  %04x:%04x\n", gDevice->name, EMU_VID, gDevice->product_id);
    printf("configuration descriptor: %u bytes\n\n", raw_len);

    EmuDeviceModel model;
    int32_t rc = emu_parse_config_descriptor(raw, raw_len, &model);
    if (rc == 0) {
        print_model(&model);
        if (save_path) {
            printf("\n");
            if (!save_raw(raw, raw_len, save_path)) return 1;
        }
        return 0;
    }

    /* The bytes are worth keeping even when we cannot read them: an
     * unrecognised descriptor is exactly what someone else needs to see. */
    fprintf(stderr, "error: descriptor parse failed with code %d\n", rc);
    if (save_path && !save_raw(raw, raw_len, save_path)) return 1;
    return 1;
}

/* -------------------------------------------------------------- milestone 2 */

/* Reads the active rate code. Returns -1 on transport failure. */
static int read_rate_code(UsbDevice dev, uint8_t unit_id, uint8_t interface)
{
    EmuControlSetup setup;
    if (emu_setup_get_clock_rate(unit_id, interface, &setup) != 0) return -1;

    uint8_t value = 0xff;
    IOReturn kr = control_transfer(dev, &setup, &value);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: GET_CUR clock rate failed: 0x%08x\n", kr);
        return -1;
    }
    return value;
}

static void describe_rate_code(int code)
{
    uint32_t hz = (code >= 0) ? emu_rate_code_to_hz((uint8_t)code) : 0;
    if (hz) printf("code %d  = %u Hz\n", code, hz);
    else    printf("code %d  = unrecognised\n", code);
}

static int cmd_clock_get(UsbDevice dev)
{
    EmuDeviceModel model;
    if (!read_model(dev, &model, NULL, NULL)) return 1;

    const EmuExtensionUnit* xu = find_clock_unit(&model);
    if (!xu) return 1;

    printf("clock rate extension unit: id %u on interface %u\n",
           xu->unit_id, model.control_interface);

    if (!device_open(dev)) return 1;

    int code = read_rate_code(dev, xu->unit_id, model.control_interface);
    if (code < 0) { (*dev)->USBDeviceClose(dev); return 1; }

    printf("current rate: ");
    describe_rate_code(code);

    (*dev)->USBDeviceClose(dev);
    return 0;
}

/*
 * Software equivalent of unplugging and reconnecting the device.
 *
 * A failed SetAlternateInterface can leave the Tracker Pre refusing control
 * transfers entirely (kIOUSBTransactionTimeout on everything). Re-enumerating
 * clears that without physical access to the hardware.
 */
static int cmd_reset(UsbDevice dev)
{
    if (!device_open(dev)) return 1;

    printf("re-enumerating device...\n");
    printf("note: this has been observed to remove the device without it\n");
    printf("      returning. Be ready to replug it physically.\n");
    IOReturn kr = (*dev)->USBDeviceReEnumerate(dev, 0);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "error: USBDeviceReEnumerate failed: 0x%08x\n", kr);
        (*dev)->USBDeviceClose(dev);
        return 1;
    }

    /* The device object is invalid from here on: the old one is terminated and
     * a fresh one appears with a new sessionID. Deliberately no close. */
    printf("done -- the device will reappear shortly with a new session\n");
    return 0;
}

/*
 * Reads the clock rate unit's rate-support control (selector 0x02).
 *
 * The descriptor advertises this control via bmControls bit 1, but E-MU's
 * source never documents the reply format, so print the raw bytes alongside a
 * bitmap reading rather than asserting one interpretation.
 */
static int cmd_clock_support(UsbDevice dev)
{
    EmuDeviceModel model;
    if (!read_model(dev, &model, NULL, NULL)) return 1;

    const EmuExtensionUnit* xu = find_clock_unit(&model);
    if (!xu) return 1;

    if ((xu->controls & 0x02) == 0) {
        fprintf(stderr, "error: unit %u does not advertise the rate-support control\n",
                xu->unit_id);
        return 1;
    }

    if (!device_open(dev)) return 1;

    int rc = 1;
    for (uint16_t len = 1; len <= 4; len *= 2) {
        EmuControlSetup setup;
        if (emu_setup_get_clock_rate_support(xu->unit_id, model.control_interface,
                                             len, &setup) != 0) {
            break;
        }

        uint8_t buf[4] = {0};
        IOReturn kr = control_transfer(dev, &setup, buf);
        if (kr != kIOReturnSuccess) {
            printf("  wLength=%u  failed: 0x%08x\n", len, kr);
            continue;
        }

        printf("  wLength=%u  raw:", len);
        for (uint16_t i = 0; i < len; i++) printf(" %02x", buf[i]);

        printf("   as bitmap:");
        for (uint8_t code = 0; code <= 5; code++) {
            if (buf[0] & (1u << code)) printf(" %u", emu_rate_code_to_hz(code));
        }
        printf("\n");
        rc = 0;
    }

    (*dev)->USBDeviceClose(dev);
    return rc;
}

/*
 * SET_CUR -> GET_CUR -> compare, exactly as guidelines section 18 requires.
 *
 * A failed SET_CUR must never be reported as success. The original E-MU driver
 * could do that, and the guidelines call it out specifically as behaviour not
 * to reproduce.
 */
static bool set_and_verify(UsbDevice dev, uint8_t unit_id, uint8_t interface,
                           uint8_t want_code, bool quiet)
{
    EmuControlSetup setup;
    if (emu_setup_set_clock_rate(unit_id, interface, &setup) != 0) return false;

    uint8_t value = want_code;
    IOReturn kr = control_transfer(dev, &setup, &value);
    if (kr != kIOReturnSuccess) {
        printf("    SET_CUR failed: 0x%08x\n", kr);
        return false;
    }

    int got = read_rate_code(dev, unit_id, interface);
    if (got < 0) return false;

    bool ok = (got == want_code);
    if (!quiet) {
        printf("    SET_CUR %u (%u Hz) -> GET_CUR %d (%u Hz)  %s\n",
               want_code, emu_rate_code_to_hz(want_code),
               got, emu_rate_code_to_hz((uint8_t)got),
               ok ? "verified" : "MISMATCH");
    }
    return ok;
}

/*
 * Repeats one rate transition to characterise the intermittent SET_CUR failure
 * documented in docs/milestone-1-2-results.md.
 *
 * Reports the value actually read back on failure. That distinction matters: a
 * read-too-soon race would return the *previous* rate, whereas the device
 * returning its 44100 default instead suggests it abandoned the change.
 */
static int cmd_clock_stress(UsbDevice dev, uint8_t from_code, uint8_t to_code,
                            int iterations, unsigned delay_ms)
{
    EmuDeviceModel model;
    if (!read_model(dev, &model, NULL, NULL)) return 1;

    const EmuExtensionUnit* xu = find_clock_unit(&model);
    if (!xu) return 1;

    if (!device_open(dev)) return 1;

    int original = read_rate_code(dev, xu->unit_id, model.control_interface);
    if (original < 0) { (*dev)->USBDeviceClose(dev); return 1; }

    printf("stressing %u Hz -> %u Hz, %d iterations",
           emu_rate_code_to_hz(from_code), emu_rate_code_to_hz(to_code), iterations);
    if (delay_ms) printf(", %u ms settle before GET_CUR", delay_ms);
    printf("\n\n");

    EmuControlSetup set_setup;
    emu_setup_set_clock_rate(xu->unit_id, model.control_interface, &set_setup);

    int from_failures = 0, to_failures = 0;
    int fallback_to_default = 0, stuck_on_previous = 0, other = 0;

    for (int i = 0; i < iterations; i++) {
        uint8_t value = from_code;
        if (control_transfer(dev, &set_setup, &value) != kIOReturnSuccess) {
            from_failures++;
            continue;
        }
        if (delay_ms) usleep(delay_ms * 1000);
        int got_from = read_rate_code(dev, xu->unit_id, model.control_interface);
        if (got_from != from_code) {
            from_failures++;
            printf("  iter %-3d setup leg failed: wanted %u, got %d\n",
                   i, from_code, got_from);
            continue;
        }

        value = to_code;
        if (control_transfer(dev, &set_setup, &value) != kIOReturnSuccess) {
            to_failures++;
            continue;
        }
        if (delay_ms) usleep(delay_ms * 1000);
        int got_to = read_rate_code(dev, xu->unit_id, model.control_interface);

        if (got_to != to_code) {
            to_failures++;
            if (got_to == 0)                    fallback_to_default++;
            else if (got_to == (int)from_code)  stuck_on_previous++;
            else                                other++;
            printf("  iter %-3d MISMATCH: wanted %u (%u Hz), got %d (%u Hz)%s\n",
                   i, to_code, emu_rate_code_to_hz(to_code),
                   got_to, got_to >= 0 ? emu_rate_code_to_hz((uint8_t)got_to) : 0,
                   got_to == 0 ? "  [fell back to default]"
                               : (got_to == (int)from_code ? "  [stayed on previous]" : ""));
        }
    }

    printf("\nresults over %d iterations\n", iterations);
    printf("  setup leg (-> %u Hz) failures:  %d\n",
           emu_rate_code_to_hz(from_code), from_failures);
    printf("  target leg (-> %u Hz) failures: %d\n",
           emu_rate_code_to_hz(to_code), to_failures);
    if (to_failures) {
        printf("    fell back to 44100 default: %d\n", fallback_to_default);
        printf("    stayed on previous rate:    %d\n", stuck_on_previous);
        printf("    some other rate:            %d\n", other);
    }

    printf("\nrestoring original rate (code %d)\n", original);
    set_and_verify(dev, xu->unit_id, model.control_interface, (uint8_t)original, false);

    (*dev)->USBDeviceClose(dev);
    return (from_failures || to_failures) ? 1 : 0;
}

static int cmd_clock_sweep(UsbDevice dev, int only_code)
{
    EmuDeviceModel model;
    if (!read_model(dev, &model, NULL, NULL)) return 1;

    const EmuExtensionUnit* xu = find_clock_unit(&model);
    if (!xu) return 1;

    if (!device_open(dev)) return 1;

    int original = read_rate_code(dev, xu->unit_id, model.control_interface);
    if (original < 0) { (*dev)->USBDeviceClose(dev); return 1; }

    printf("clock rate unit %u on interface %u\n", xu->unit_id, model.control_interface);
    printf("original rate: ");
    describe_rate_code(original);
    printf("\n");

    int failures = 0, attempted = 0;

    for (uint8_t code = 0; code <= 5; code++) {
        if (only_code >= 0 && code != (uint8_t)only_code) continue;

        uint32_t hz = emu_rate_code_to_hz(code);
        printf("  %u Hz (code %u)\n", hz, code);
        attempted++;
        if (!set_and_verify(dev, xu->unit_id, model.control_interface, code, false)) {
            failures++;
        }
    }

    /* Always put the device back the way it was found. */
    printf("\nrestoring original rate (code %d)\n", original);
    if (!set_and_verify(dev, xu->unit_id, model.control_interface,
                        (uint8_t)original, false)) {
        fprintf(stderr, "warning: could not restore the original rate\n");
        failures++;
    }

    (*dev)->USBDeviceClose(dev);

    printf("\n%d/%d rate changes verified\n", attempted - failures, attempted);
    return failures ? 1 : 0;
}

/*
 * The device's clock rate must be set before an alternate setting matching that
 * rate is selected. E-MU's own driver does this -- EMUUSBAudioEngine::SetSampleRate
 * runs before SetAlternateInterface in startUSBStream -- and selecting an alt
 * whose tSamFreq disagrees with the active clock appears to be what wedges the
 * device into refusing all control transfers.
 */
static int prepare_clock_for_capture(UsbDevice dev, const EmuDeviceModel* model,
                                     uint32_t hz)
{
    const EmuExtensionUnit* xu = find_clock_unit(model);
    if (!xu) return 1;

    uint8_t code = emu_hz_to_rate_code(hz);
    if (code == 0xff) {
        fprintf(stderr, "error: %u Hz is not a rate this device supports\n", hz);
        return 1;
    }

    if (!device_open(dev)) return 1;

    int current = read_rate_code(dev, xu->unit_id, model->control_interface);
    if (current < 0) { (*dev)->USBDeviceClose(dev); return 1; }

    if (current == (int)code) {
        printf("clock already at %u Hz (code %u)\n", hz, code);
    } else {
        printf("setting clock %u Hz -> %u Hz before selecting an alt setting\n",
               emu_rate_code_to_hz((uint8_t)current), hz);
        if (!set_and_verify(dev, xu->unit_id, model->control_interface, code, false)) {
            fprintf(stderr,
                    "error: clock rate did not take effect; refusing to select a\n"
                    "       mismatched alternate setting\n");
            (*dev)->USBDeviceClose(dev);
            return 1;
        }
    }

    (*dev)->USBDeviceClose(dev);
    return 0;
}

/* ------------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
        "usage: emu-probe <command>\n"
        "\n"
        "  descriptors [file]   parse and print the topology; optionally save raw bytes\n"
        "  clock                read the active sample rate         (read-only)\n"
        "  reset                re-enumerate the device (software replug)\n"
        "  clock-support        query which rates the device claims  (read-only)\n"
        "  clock-sweep          set and verify every supported rate (WRITES to device)\n"
        "  clock-set <hz>       set one rate and verify it          (WRITES to device)\n"
        "  capture <hz> [ms] [trace.csv]\n"
        "                       record isochronous capture packets   (streams audio in)\n"
        "  play <hz> [ms] [tone_hz] [amplitude_pct] [opts]\n"
        "                       duplex engine: capture clocks playback of a sine\n"
        "                       tone. Connect headphones to hear it. 'short' picks\n"
        "                       the 0.5 ms endpoint where the rate offers both,\n"
        "                       'delay=<ms>' starts playback that far after capture,\n"
        "                       'duty=<n>' polls capture on 1 interval in n,\n"
        "                       'fbtrace=<f.csv>' logs 0x81 against host time,\n"
        "                       'plan=device' sizes packets from 0x81 in full duplex,\n"
        "                       'req=<ms>' audio queued per request (1 to 8, default 8),\n"
        "                       'fbraw' follows 0x81 without correcting its scaling.\n"
        "  play-only <hz> [ms] [tone_hz] [amplitude_pct] [short]\n"
        "                       the same with the capture interface never opened:\n"
        "                       the feedback endpoint sizes the packets and half\n"
        "                       the traffic leaves the bus. Judge it by ear.\n"
        "  play-idle <hz> [ms] [tone_hz] [amplitude_pct] [short]\n"
        "                       capture claimed and streaming but never read: the\n"
        "                       device's converter runs, nothing of it reaches the\n"
        "                       bus. Splits 'the traffic' from 'the ADC'.\n"
        "  lltest [hz]          diagnostic: low-latency isoc frame granularity\n"
        "  clock-stress <from_hz> <to_hz> [iters] [delay_ms]\n"
        "                       repeat one transition to characterise\n"
        "                       intermittent failures               (WRITES to device)\n"
        "\n"
        "Read-only by default. The writing commands restore the original rate\n"
        "when they finish.\n");
}

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 2; }

    /* Catch a Rust/C struct layout disagreement before it corrupts output. */
    if (emu_device_model_size() != sizeof(EmuDeviceModel)) {
        fprintf(stderr,
                "fatal: EmuDeviceModel layout mismatch (rust %u, c %zu).\n"
                "       emu_ca0189.h is out of sync with descriptor.rs.\n",
                emu_device_model_size(), sizeof(EmuDeviceModel));
        return 3;
    }

    io_service_t service = IO_OBJECT_NULL;
    bool opened = false;
    UsbDevice dev = open_device(&service, &opened);
    if (!dev) return 1;

    int rc;
    const char* cmd = argv[1];

    if (strcmp(cmd, "descriptors") == 0) {
        rc = cmd_descriptors(dev, argc > 2 ? argv[2] : NULL);
    } else if (strcmp(cmd, "clock") == 0) {
        rc = cmd_clock_get(dev);
    } else if (strcmp(cmd, "reset") == 0) {
        rc = cmd_reset(dev);
    } else if (strcmp(cmd, "clock-support") == 0) {
        rc = cmd_clock_support(dev);
    } else if (strcmp(cmd, "clock-sweep") == 0) {
        rc = cmd_clock_sweep(dev, -1);
    } else if (strcmp(cmd, "capture") == 0) {
        if (argc < 3) { usage(); rc = 2; }
        else {
            EmuDeviceModel model;
            if (!read_model(dev, &model, NULL, NULL)) {
                rc = 1;
            } else if (prepare_clock_for_capture(
                           dev, &model, (uint32_t)strtoul(argv[2], NULL, 10)) != 0) {
                rc = 1;
            } else {
                CaptureConfig cfg = {
                    .sample_rate = (uint32_t)strtoul(argv[2], NULL, 10),
                    .duration_ms = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 10) : 2000,
                    .trace_path  = (argc > 4) ? argv[4] : NULL,
                    .prefer_interval = 0,
                };
                rc = emu_capture_run(dev, &model, &cfg);
            }
        }
    } else if (strcmp(cmd, "play") == 0 || strcmp(cmd, "play-only") == 0
               || strcmp(cmd, "play-idle") == 0) {
        if (argc < 3) { usage(); rc = 2; }
        else {
            uint32_t hz = (uint32_t)strtoul(argv[2], NULL, 10);
            EmuDeviceModel model;
            if (!read_model(dev, &model, NULL, NULL)) {
                rc = 1;
            } else if (prepare_clock_for_capture(dev, &model, hz) != 0) {
                rc = 1;
            } else {
                DuplexConfig cfg = {
                    .sample_rate = hz,
                    .duration_ms = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 10) : 5000,
                    .tone_hz     = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 10) : 440,
                    .amplitude   = (argc > 5) ? strtod(argv[5], NULL) / 100.0 : 0.15,
                    .playback_only  = strcmp(cmd, "play-only") == 0,
                    .capture_idle   = strcmp(cmd, "play-idle") == 0,
                };
                /* Trailing options, in any order: 'short' and 'delay=<ms>'. */
                for (int i = 6; i < argc; i++) {
                    if (strcmp(argv[i], "short") == 0) cfg.short_interval = true;
                    else if (strcmp(argv[i], "plan=device") == 0)
                        cfg.plan_from_device = true;
                    else if (strncmp(argv[i], "fbtrace=", 8) == 0)
                        cfg.feedback_trace = argv[i] + 8;
                    else if (strncmp(argv[i], "duty=", 5) == 0)
                        cfg.capture_duty = (uint32_t)strtoul(argv[i] + 5, NULL, 10);
                    else if (strncmp(argv[i], "delay=", 6) == 0)
                        cfg.sync_delay_ms = (uint32_t)strtoul(argv[i] + 6, NULL, 10);
                    else if (strncmp(argv[i], "req=", 4) == 0)
                        cfg.request_ms = (uint32_t)strtoul(argv[i] + 4, NULL, 10);
                    else if (strcmp(argv[i], "fbraw") == 0)
                        cfg.feedback_raw = true;
                    else fprintf(stderr, "warning: ignoring '%s'\n", argv[i]);
                }
                rc = emu_duplex_run(dev, &model, &cfg);
            }
        }
    } else if (strcmp(cmd, "lltest") == 0) {
        uint32_t hz = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 176400;
        EmuDeviceModel model;
        if (!read_model(dev, &model, NULL, NULL)) rc = 1;
        else if (prepare_clock_for_capture(dev, &model, hz) != 0) rc = 1;
        else rc = emu_lowlatency_probe(dev, &model, hz);
    } else if (strcmp(cmd, "clock-stress") == 0) {
        if (argc < 4) { usage(); rc = 2; }
        else {
            uint8_t from = emu_hz_to_rate_code((uint32_t)strtoul(argv[2], NULL, 10));
            uint8_t to   = emu_hz_to_rate_code((uint32_t)strtoul(argv[3], NULL, 10));
            if (from == 0xff || to == 0xff) {
                fprintf(stderr, "error: unsupported rate in '%s -> %s'\n", argv[2], argv[3]);
                rc = 2;
            } else {
                int iters = (argc > 4) ? atoi(argv[4]) : 20;
                unsigned delay = (argc > 5) ? (unsigned)atoi(argv[5]) : 0;
                rc = cmd_clock_stress(dev, from, to, iters, delay);
            }
        }
    } else if (strcmp(cmd, "clock-set") == 0) {
        if (argc < 3) { usage(); rc = 2; }
        else {
            uint8_t code = emu_hz_to_rate_code((uint32_t)strtoul(argv[2], NULL, 10));
            if (code == 0xff) {
                fprintf(stderr, "error: %s Hz is not a rate this device supports\n", argv[2]);
                rc = 2;
            } else {
                rc = cmd_clock_sweep(dev, code);
            }
        }
    } else {
        usage();
        rc = 2;
    }

    (*dev)->Release(dev);
    IOObjectRelease(service);
    return rc;
}
