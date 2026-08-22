/*
 * Milestone 4 -- internally duplex USB engine.
 *
 * Capture and playback run together, with capture acting as the clock
 * reference that sizes playback packets.
 */

#pragma once

#include <stdint.h>
#include <IOKit/usb/IOUSBLib.h>

#include "../../rust/emu-ca0189/include/emu_ca0189.h"

typedef struct {
    uint32_t sample_rate;
    uint32_t duration_ms;
    /// 0 plays silence.
    uint32_t tone_hz;
    /// 0.0 to 1.0 of full scale.
    double   amplitude;
} DuplexConfig;

int emu_duplex_run(IOUSBDeviceInterface500** dev,
                   const EmuDeviceModel* model,
                   const DuplexConfig* config);
