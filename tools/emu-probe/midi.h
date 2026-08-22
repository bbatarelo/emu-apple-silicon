/*
 * MIDI transport probe.
 *
 * The 0404 USB carries an ordinary USB-MIDI 1.0 interface on two bulk
 * endpoints. These commands prove the transport works before a CoreMIDI
 * driver is built on it: dump what arrives, send arbitrary bytes, and -- with
 * a DIN cable from MIDI OUT to MIDI IN -- verify a loopback byte-for-byte.
 */

#pragma once

#include <stdint.h>
#include <IOKit/usb/IOUSBLib.h>

#include "../../rust/emu-ca0189/include/emu_ca0189.h"

/* Prints every MIDI message that arrives for `duration_ms`. */
int emu_midi_dump(IOUSBDeviceInterface500** dev,
                  const EmuDeviceModel* model,
                  uint32_t duration_ms);

/* Encodes `len` MIDI bytes into event packets and sends them. */
int emu_midi_send(IOUSBDeviceInterface500** dev,
                  const EmuDeviceModel* model,
                  const uint8_t* bytes, uint32_t len);

/* Sends a test sequence and expects it back byte-for-byte. Needs a DIN cable
 * connecting the device's MIDI OUT to its MIDI IN. */
int emu_midi_loopback(IOUSBDeviceInterface500** dev,
                      const EmuDeviceModel* model);
