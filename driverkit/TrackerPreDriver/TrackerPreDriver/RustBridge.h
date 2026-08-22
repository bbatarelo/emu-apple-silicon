//
//  RustBridge.h
//  TrackerPreDriver
//
//  C ABI boundary to the freestanding Rust core.
//  See EMU_Tracker_Pre_Development_Guidelines.md sections 13 and 22.
//
//  Everything here must stay a small C ABI. No C++ ABI calls from Rust, no
//  Rust references or slices exposed to C++, no panics or exceptions crossing
//  the boundary.
//

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Probe: pure function -------------------------------------------------
// Returns a + b + 0x454d5500. The marker makes it obvious the value really
// came through Rust object code.
uint32_t emu_rust_probe_add(uint32_t a, uint32_t b);

// --- Probe: opaque object over caller-provided storage --------------------
// C++ owns the memory; Rust only initializes and mutates it in place. This
// keeps the probe allocation-free, matching the real-time rules in section 16.
typedef struct ProbeCore ProbeCore;

uint32_t   probe_core_size(void);
uint32_t   probe_core_align(void);
ProbeCore* probe_core_init(void* storage, uint32_t initial);
uint32_t   probe_core_increment(ProbeCore* core);  // u32 max on invalid handle
void       probe_core_destroy(ProbeCore* core);

// --- Probe: atomics -------------------------------------------------------
// Exercises the subset of core:: the future statistics/packet engine needs.
uint64_t probe_counter_bump(void);
uint64_t probe_counter_read(void);

#ifdef __cplusplus
}
#endif
