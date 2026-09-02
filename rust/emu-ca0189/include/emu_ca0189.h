/*
 * C view of the emu-ca0189 Rust core.
 *
 * Field order and types must match src/descriptor.rs and src/protocol.rs
 * exactly. emu_device_model_size() exists so a mismatch is caught at startup
 * instead of silently corrupting every value printed.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU_MAX_ALT_SETTINGS   48
#define EMU_MAX_EXTENSION_UNITS 8
#define EMU_MAX_TERMINALS      12

#define EMU_XU_CLOCK_RATE        0xe301
#define EMU_XU_CLOCK_SOURCE      0xe302
#define EMU_XU_DIGITAL_IO_STATUS 0xe303
#define EMU_XU_DEVICE_OPTIONS    0xe304
#define EMU_XU_DIRECT_MONITORING 0xe305
#define EMU_XU_METERING          0xe306

#define EMU_SELECTOR_ENABLE_PROCESSING  0x01
#define EMU_SELECTOR_CLOCK_RATE_SUPPORT 0x02
#define EMU_SELECTOR_CLOCK_RATE         0x03

typedef struct {
    uint8_t  unit_id;
    uint16_t extension_code;
    uint8_t  num_in_pins;
    uint8_t  source_id;
    uint8_t  channels;
    uint32_t controls;
} EmuExtensionUnit;

typedef struct {
    uint8_t  terminal_id;
    uint16_t terminal_type;
    uint8_t  source_id;
    uint8_t  channels;
    uint8_t  is_input;
} EmuTerminal;

typedef struct {
    uint8_t  interface_number;
    uint8_t  alternate_setting;
    uint8_t  num_endpoints;
    uint8_t  data_endpoint;
    uint8_t  data_endpoint_attributes;
    uint8_t  feedback_endpoint;
    uint16_t max_packet_size;
    uint8_t  interval;
    uint8_t  terminal_link;
    uint8_t  channels;
    uint8_t  subframe_size;
    uint8_t  bit_resolution;
    uint32_t sample_rate;
} EmuAltSetting;

typedef struct {
    uint8_t  configuration_value;
    uint8_t  num_interfaces;
    uint16_t max_power_ma;

    uint8_t  control_interface;
    uint8_t  status_endpoint;

    uint8_t  num_extension_units;
    EmuExtensionUnit extension_units[EMU_MAX_EXTENSION_UNITS];

    uint8_t  num_terminals;
    EmuTerminal terminals[EMU_MAX_TERMINALS];

    uint16_t num_alt_settings;
    EmuAltSetting alt_settings[EMU_MAX_ALT_SETTINGS];
} EmuDeviceModel;

/* USB control transfer setup packet, built by the Rust side so the encoding
 * lives in exactly one place. */
typedef struct {
    uint8_t  bm_request_type;
    uint8_t  b_request;
    uint16_t w_value;
    uint16_t w_index;
    uint16_t w_length;
} EmuControlSetup;

/* Returns 0 on success, a positive ParseError code, or -1 for a null pointer. */
int32_t emu_parse_config_descriptor(const uint8_t* bytes, uint32_t len, EmuDeviceModel* out);

int32_t emu_setup_get_clock_rate(uint8_t unit_id, uint8_t interface, EmuControlSetup* out);
int32_t emu_setup_set_clock_rate(uint8_t unit_id, uint8_t interface, EmuControlSetup* out);
int32_t emu_setup_get_clock_rate_support(uint8_t unit_id, uint8_t interface,
                                         uint16_t length, EmuControlSetup* out);

uint32_t emu_rate_code_to_hz(uint8_t code);
uint8_t  emu_hz_to_rate_code(uint32_t hz);
uint32_t emu_device_model_size(void);

/* --- streaming: feedback queue and packet planning --------------------- */

/* Opaque; storage is provided by the caller so nothing allocates. */
typedef struct EmuFeedback EmuFeedback;

uint32_t     emu_feedback_size(void);
uint32_t     emu_feedback_align(void);
EmuFeedback* emu_feedback_init(uint8_t* storage);

/* Sets the rate-accurate fallback used when the queue is starved. */
void     emu_feedback_set_nominal(EmuFeedback* fb, uint32_t sample_rate, uint64_t interval_ns);
void     emu_feedback_push(EmuFeedback* fb, uint32_t frames);
/* Frames for the next playback packet, falling back to `nominal` and counting
 * starvation when no capture measurement is queued. */
uint32_t emu_feedback_next(EmuFeedback* fb, uint32_t nominal);
uint32_t emu_feedback_depth(EmuFeedback* fb);
uint32_t emu_feedback_overflows(EmuFeedback* fb);
uint32_t emu_feedback_starved(EmuFeedback* fb);

uint32_t emu_frames_in_packet(uint32_t bytes, uint32_t bytes_per_frame);
uint32_t emu_output_packet_bytes(uint32_t frames, uint32_t output_bytes_per_frame);

/* --- streaming: completion timestamp filter ---------------------------- */

/* Opaque; storage is provided by the caller so nothing allocates. Critically
 * damped smoothing of the once-per-request completion timestamps that anchor
 * Core Audio's timeline; observations must arrive at a uniform cadence. */
typedef struct EmuTsFilter EmuTsFilter;

uint32_t     emu_ts_filter_size(void);
uint32_t     emu_ts_filter_align(void);
/* `start`: expected first timestamp; `nominal_step`: expected observation
 * spacing, in the same unit as the timestamps (the engine uses host ticks). */
EmuTsFilter* emu_ts_filter_init(uint8_t* storage, uint64_t start, uint64_t nominal_step);
/* One raw timestamp in, the filtered timestamp out. */
uint64_t emu_ts_filter_apply(EmuTsFilter* f, uint64_t raw);
/* Moves the prediction to a discontinuity the caller knows about -- the raw
 * timestamp the next observation will carry -- keeping the learned rate, and
 * without counting a reset. For a rebuilt bus schedule after a stall. */
void     emu_ts_filter_rebase(EmuTsFilter* f, uint64_t expected_next);
/* Times the filter snapped to a discontinuity instead of slewing. */
uint32_t emu_ts_filter_resets(EmuTsFilter* f);

#ifdef __cplusplus
}
#endif
