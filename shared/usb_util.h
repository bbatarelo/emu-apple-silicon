/*
 * IOKit helpers shared between the capture probe and the duplex engine.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <IOKit/usb/IOUSBLib.h>

/* Claims the interface nub with the given bInterfaceNumber. The caller owns the
 * returned interface and must Release it. */
bool emu_find_interface(IOUSBDeviceInterface500** dev,
                        uint8_t interface_number,
                        IOUSBInterfaceInterface500*** out);

/* Locates the isochronous pipe in `direction` (kUSBIn / kUSBOut) on the
 * currently selected alternate setting. Must be called after
 * SetAlternateInterface, since pipes only exist for the active alt. */
bool emu_find_isoc_pipe(IOUSBInterfaceInterface500** intf,
                        uint8_t direction,
                        uint8_t* out_pipe,
                        uint16_t* out_max_packet);

const char* emu_isoc_status_name(int32_t status);

/* An isochronous frame that moved fewer bytes than requested reports underrun.
 * On an asynchronous endpoint that is the normal case, not a failure. */
bool emu_frame_ok(int32_t status);
