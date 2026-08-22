/*
 * Host-side test of the Rust core's C ABI surface.
 *
 * Test 0 in EMU_Tracker_Pre_Development_Guidelines.md section 23. This proves
 * only that the source and tooling behave; it says nothing about DriverKit
 * compatibility, which is what Tests 1-4 are for.
 *
 * Written in C rather than as `cargo test` on purpose: the crate is a no_std
 * staticlib, and the thing worth testing is the boundary C++ will actually call
 * across, not Rust-internal calls that bypass the ABI entirely.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

extern uint32_t emu_rust_probe_add(uint32_t a, uint32_t b);
extern uint32_t probe_core_size(void);
extern uint32_t probe_core_align(void);
extern void*    probe_core_init(void* storage, uint32_t initial);
extern uint32_t probe_core_increment(void* core);
extern void     probe_core_destroy(void* core);
extern uint64_t probe_counter_bump(void);
extern uint64_t probe_counter_read(void);

static int failures = 0;

static void check(int condition, const char* what)
{
    if (condition) {
        printf("  pass  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

int main(void)
{
    printf("Rust C ABI tests\n");

    /* Pure function, with the marker constant proving the value came from Rust. */
    check(emu_rust_probe_add(7, 11) == (7u + 11u + 0x454d5500u), "add returns EMU marker");
    check(emu_rust_probe_add(0xFFFFFFFFu, 1) == 0x454d5500u,     "add wraps rather than overflows");

    /* Caller-provided storage: C owns the memory, Rust only initializes it. */
    _Alignas(16) unsigned char storage[64];
    check(probe_core_size() <= sizeof storage,                   "core fits in provided storage");
    check(probe_core_align() <= 16,                              "core alignment satisfiable");

    void* core = probe_core_init(storage, 100);
    check(core != NULL,                                          "init accepts valid storage");
    check(probe_core_increment(core) == 101,                     "increment 100 -> 101");
    check(probe_core_increment(core) == 102,                     "increment 101 -> 102");

    probe_core_destroy(core);
    check(probe_core_increment(core) == UINT32_MAX,              "stale handle rejected after destroy");

    /* Pointer validation at the FFI edge, which is where every C++ pointer arrives. */
    check(probe_core_init(NULL, 0) == NULL,                      "null storage rejected");
    check(probe_core_init(storage + 1, 0) == NULL,               "misaligned storage rejected");

    /* Atomics: the subset the future statistics engine depends on. */
    uint64_t first = probe_counter_bump();
    check(probe_counter_bump() == first + 1,                     "counter increments monotonically");
    check(probe_counter_read() == first + 1,                     "counter read matches last bump");

    if (failures == 0) {
        printf("\nTEST 0 PASSED\n");
        return 0;
    }
    printf("\nTEST 0 FAILED (%d failing checks)\n", failures);
    return 1;
}
