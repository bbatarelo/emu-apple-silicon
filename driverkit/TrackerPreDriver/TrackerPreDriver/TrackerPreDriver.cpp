//
//  TrackerPreDriver.cpp
//  TrackerPreDriver
//
//  Created by Bruno Batarelo on 20.08.2026..
//
//  Currently this is the Milestone 0 feasibility probe, not the real driver.
//  It exercises the C++ -> C ABI -> Rust -> C ABI -> C++ round trip inside the
//  dext process. No USB and no AudioDriverKit yet, deliberately: see
//  EMU_Tracker_Pre_Development_Guidelines.md Part V.
//

#include <os/log.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>

#include "TrackerPreDriver.h"
#include "RustBridge.h"

#define LOG_PREFIX "TrackerPreDriver: "

namespace {

// Storage for the probe core. Statically reserved so the probe performs no
// allocation, exactly as the real streaming engine will be required to behave.
// Sized generously and alignment-checked against Rust's own reported values.
alignas(16) uint8_t gProbeCoreStorage[64];

// Runs the Milestone 0 checks. Returns true only if every stage produced the
// expected deterministic result.
bool RunRustProbe()
{
    // --- Stage 1: pure function, recognizable marker ---
    const uint32_t result = emu_rust_probe_add(7, 11);
    const uint32_t expected = static_cast<uint32_t>(7 + 11 + 0x454d5500u);

    if (result != expected) {
        os_log(OS_LOG_DEFAULT,
               LOG_PREFIX "PROBE FAIL: emu_rust_probe_add returned 0x%{public}x, expected 0x%{public}x",
               result, expected);
        return false;
    }
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "probe stage 1 ok: add -> 0x%{public}x", result);

    // --- Stage 2: opaque Rust object over caller-provided storage ---
    const uint32_t size = probe_core_size();
    const uint32_t align = probe_core_align();

    if (size > sizeof(gProbeCoreStorage) || align > alignof(decltype(gProbeCoreStorage))) {
        os_log(OS_LOG_DEFAULT,
               LOG_PREFIX "PROBE FAIL: storage too small/misaligned (need %{public}u/%{public}u)",
               size, align);
        return false;
    }

    ProbeCore* core = probe_core_init(gProbeCoreStorage, 100);
    if (core == nullptr) {
        os_log(OS_LOG_DEFAULT, LOG_PREFIX "PROBE FAIL: probe_core_init returned null");
        return false;
    }

    if (probe_core_increment(core) != 101 || probe_core_increment(core) != 102) {
        os_log(OS_LOG_DEFAULT, LOG_PREFIX "PROBE FAIL: probe_core_increment sequence wrong");
        probe_core_destroy(core);
        return false;
    }

    probe_core_destroy(core);

    // Rust must reject the stale handle rather than mutating freed state.
    if (probe_core_increment(core) != UINT32_MAX) {
        os_log(OS_LOG_DEFAULT, LOG_PREFIX "PROBE FAIL: use-after-destroy not rejected");
        return false;
    }
    os_log(OS_LOG_DEFAULT, LOG_PREFIX "probe stage 2 ok: lifecycle and handle validation");

    // --- Stage 3: atomics ---
    const uint64_t first = probe_counter_bump();
    const uint64_t second = probe_counter_bump();

    if (second != first + 1 || probe_counter_read() != second) {
        os_log(OS_LOG_DEFAULT, LOG_PREFIX "PROBE FAIL: atomic counter inconsistent");
        return false;
    }
    os_log(OS_LOG_DEFAULT,
           LOG_PREFIX "probe stage 3 ok: atomics -> %{public}llu", second);

    return true;
}

} // namespace

kern_return_t
IMPL(TrackerPreDriver, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, LOG_PREFIX "super Start failed: 0x%{public}x", ret);
        return ret;
    }

    if (!RunRustProbe()) {
        // Fail loudly. A probe that silently "succeeds" would defeat the
        // entire purpose of the Milestone 0 gate.
        return kIOReturnError;
    }

    os_log(OS_LOG_DEFAULT, LOG_PREFIX "Rust-in-DriverKit probe PASSED");

    RegisterService();
    return kIOReturnSuccess;
}
