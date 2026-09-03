/*
 * Signal analysis for the loopback tests.
 *
 * Kept apart from the Core Audio plumbing in main.c because it is the half
 * that can be wrong without anything sounding wrong: an analyser that reports
 * "no glitches" because of an indexing slip is worse than no test at all.
 * Everything here works on plain mono arrays and has no idea where they came
 * from, which is what lets `hal-loopback selftest` drive it from synthetic
 * signals with known faults and no hardware.
 */

#pragma once

#include <stdint.h>

/*
 * The analysis block, and the reason the test tones are what they are.
 *
 * Every tone is placed exactly on an FFT bin, so the played signal repeats
 * with a period of exactly EMU_BLOCK frames. A loopback cable carries it
 * through an LTI path -- converters and an analogue link -- which can change a
 * sinusoid's amplitude and phase but not its periodicity. So the captured
 * signal is EMU_BLOCK-periodic too, whatever the delay is, and consecutive
 * blocks must be identical to within the noise floor.
 *
 * That turns three different faults into three different measurements, with no
 * alignment step and no assumption about the delay:
 *
 *   a hole or a splice   -> the block stops being a clean tone   (THD+N, level)
 *   a sample slip        -> the block's phase steps              (phase)
 *   a clock mismatch     -> the phase drifts linearly            (phase slope)
 */
#define EMU_BLOCK 4096u

/* Forward FFT, in place, n a power of two. */
void emu_fft(double* re, double* im, unsigned n);

/* What one block of a captured tone looks like. Levels are dBFS of amplitude;
 * `thdn_db` is everything that is not the tone, relative to the tone. */
typedef struct {
    double level_db;      /* the tone itself */
    double thdn_db;       /* residual / tone, in dB: -80 is good, 0 is noise */
    double phase_deg;     /* at the tone's bin; constant unless frames moved */
    double rival_db;      /* the *other* channel's tone: crosstalk or a swap */
    double dc_db;         /* bin 0, reported rather than folded into THD+N */
    double peak;          /* time-domain peak, for clipping */
} EmuToneBlock;

/* One block, starting at x[0]. `rival` may equal `bin` to disable that term. */
void emu_tone_block(const float* x, unsigned n, unsigned bin, unsigned rival,
                    EmuToneBlock* out);

/* The whole run, in blocks of EMU_BLOCK. */
typedef struct {
    unsigned blocks;
    double   level_min_db, level_max_db, level_mean_db;
    double   thdn_worst_db, thdn_mean_db;
    double   rival_worst_db;
    double   dc_worst_db;
    double   peak;

    /* Phase unwrapped across blocks and referred to the first block. A step of
     * one frame moves it by 360 * bin / EMU_BLOCK degrees. */
    double   phase_span_deg;        /* max - min: any frame movement at all */
    double   phase_step_max_deg;    /* the largest single step: where it moved */
    double   phase_slope_deg;       /* per block, least squares: clock drift */
    double   frames_slipped;        /* phase_span expressed in frames */

    /* Which block held the worst of each, counted from the start of the span. */
    unsigned worst_thdn_block;
    unsigned worst_phase_block;

    /* Block-to-block difference: the same periodicity argument in the time
     * domain, which localises a glitch to the frame rather than the block. */
    double   discontinuity;         /* largest |x[i] - x[i - EMU_BLOCK]| */
    uint64_t discontinuity_frame;   /* where, relative to the analysed span */
} EmuToneSummary;

/* Scan `count` frames of mono. Blocks that do not fit are ignored. */
void emu_tone_scan(const float* x, uint64_t count, unsigned bin, unsigned rival,
                   EmuToneSummary* out);

/*
 * Where in `sig` the reference `ref` turns up, by normalised cross-correlation
 * over lags 0..maxlag, with the peak parabolically interpolated so the answer
 * is finer than a frame. `quality` gets the correlation coefficient (1.0 is a
 * perfect match) and `polarity` -1 if the loopback inverts.
 *
 * Returns a negative lag if `sig` is too short to search.
 */
double emu_find_delay(const float* ref, unsigned reflen,
                      const float* sig, uint64_t siglen, unsigned maxlag,
                      double* quality, int* polarity);

/* --- the signals, shared by the generator and the self-test --------------- */

/* A tone at `bin`, amplitude `amp`, phase 0, into n frames of mono. */
void emu_gen_tone(float* x, uint64_t n, unsigned bin, double amp);

/* A Hann-windowed linear chirp, f0 -> f1 as a fraction of the sample rate. */
void emu_gen_chirp(float* x, unsigned n, double f0_norm, double f1_norm, double amp);

/* Drives every analyser above with signals whose faults are known. Prints what
 * it checked; returns the number of checks that failed. */
int emu_analysis_selftest(void);
