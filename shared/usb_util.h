/*
 * IOKit helpers shared between the capture probe and the duplex engine.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <IOKit/usb/IOUSBLib.h>

#include "device.h"

/* Finds an attached device with the given product ID (and the E-MU vendor
 * ID), or IO_OBJECT_NULL. The caller owns the service and must release it.
 * Which one, if several of that product are attached, is up to IOKit. */
io_service_t emu_find_product(uint16_t product_id);

/* Finds an attached device this driver knows about, preferring
 * `preferred_product_id` when more than one is plugged in. Returns its identity
 * and, in *out_service, the matching IOKit service, which the caller owns and
 * must release. Returns NULL when no known E-MU device is present.
 *
 * Every product in the table is looked for, so one build serves the whole
 * family rather than the product ID being fixed at compile time. */
const EmuDeviceIdentity* emu_find_device(uint16_t preferred_product_id,
                                         io_service_t* out_service);

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
