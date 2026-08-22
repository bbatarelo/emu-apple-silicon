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
    /* Believed to share the CA0189 protocol. Neither has been tested; they are
     * listed so the shape of the work is obvious, not because they are known to
     * function. Remove the entry or set verified once someone checks. */
    { 0x3f02, "E-MU 0202 USB",     false },
    { 0x3f04, "E-MU 0404 USB",     false },
};

#define EMU_DEVICE_COUNT (sizeof(kEmuDevices) / sizeof(kEmuDevices[0]))

/* The device this build drives. Until multi-device support exists in the HAL
 * plug-in, this is the one it opens and the one it advertises to Core Audio. */
#define EMU_DEFAULT_PRODUCT_ID 0x3f0a

static inline const EmuDeviceIdentity* emu_device_for_product(uint16_t product_id)
{
    for (unsigned i = 0; i < EMU_DEVICE_COUNT; i++) {
        if (kEmuDevices[i].product_id == product_id) return &kEmuDevices[i];
    }
    return 0;
}
