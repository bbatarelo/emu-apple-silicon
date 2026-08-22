/*
 * Which devices this driver claims.
 *
 * Everything E-MU built on the CA0189 speaks the same protocol, so adding a
 * sibling should mostly be a matter of adding a row here and confirming the
 * descriptors match what the parser expects. See docs/ADDING-A-DEVICE.md.
 *
 * The descriptors remain authoritative for topology, rates and endpoints. Only
 * the identity is fixed here.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Creative Labs, who owned E-MU. Shared by the whole family. */
#define EMU_VENDOR_ID 0x041e

typedef struct {
    uint16_t    product_id;
    const char* name;
    /* Whether anyone has actually run this driver against the hardware. An
     * untested entry is a hypothesis, and saying so is more useful than a
     * confident list that turns out to be wrong. */
    bool        verified;
} EmuDeviceIdentity;

static const EmuDeviceIdentity kEmuDevices[] = {
    { 0x3f0a, "E-MU Tracker Pre",  true  },
    /* Believed to share the CA0189 protocol, but untested. Listed so the shape
     * of the work is obvious, not because it is known to function. Remove the
     * entry or set verified once someone checks. */
    { 0x3f02, "E-MU 0202 USB",     false },
    /* Descriptors read, all six rates set and verified by read-back, and duplex
     * streaming ran clean at 48 kHz. See docs/FINDINGS.md. */
    { 0x3f04, "E-MU 0404 USB",     true  },
};

#define EMU_DEVICE_COUNT (sizeof(kEmuDevices) / sizeof(kEmuDevices[0]))

/* Which device to prefer when more than one from the table is plugged in. The
 * driver and the tools look for every device in this table and take whichever
 * is attached, so a single build serves any of them; this only breaks the tie.
 * Until multi-device support exists in the HAL plug-in, exactly one device is
 * published to Core Audio. */
#define EMU_DEFAULT_PRODUCT_ID 0x3f0a

static inline const EmuDeviceIdentity* emu_device_for_product(uint16_t product_id)
{
    for (unsigned i = 0; i < EMU_DEVICE_COUNT; i++) {
        if (kEmuDevices[i].product_id == product_id) return &kEmuDevices[i];
    }
    return 0;
}
