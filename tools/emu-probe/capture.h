/*
 * Isochronous capture probe -- Milestone 3.
 *
 * Records, for every USB service interval: the frame number, how many bytes
 * were requested, how many actually arrived, and the resulting sample-frame
 * count. Those traces are what the feedback engine and clock estimator get
 * validated against offline, without a driver installed
 * (guidelines sections 10.5 and 29).
 */

#pragma once

#include <stdint.h>
#include <IOKit/usb/IOUSBLib.h>

#include "../../rust/emu-ca0189/include/emu_ca0189.h"

typedef struct {
    uint32_t    sample_rate;
    uint32_t    duration_ms;
    const char* trace_path;   /* NULL to skip writing a trace file */
    int         prefer_interval;  /* 0 = any; else only alts with this bInterval */
} CaptureConfig;

/* Returns 0 on success. */
int emu_capture_run(IOUSBDeviceInterface500** dev,
                    const EmuDeviceModel* model,
                    const CaptureConfig* config);
