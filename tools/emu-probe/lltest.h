/* Diagnostic probe for low-latency isochronous frame-list granularity. */
#pragma once

#include <stdint.h>
#include <IOKit/usb/IOUSBLib.h>
#include "../../rust/emu-ca0189/include/emu_ca0189.h"

int emu_lowlatency_probe(IOUSBDeviceInterface500** dev,
                         const EmuDeviceModel* model,
                         uint32_t rate);
