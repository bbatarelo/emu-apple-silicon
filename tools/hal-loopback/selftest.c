/*
 * The test of the tests.
 *
 * Every analyser in analysis.c reports a number that is supposed to be small
 * when the audio is good. A bug in one of them produces exactly the same
 * output as a clean run, on hardware that is misbehaving. So each detector is
 * driven here from a signal built with the fault it is meant to catch, and
 * checked to fire -- and, just as important, checked *not* to fire on the
 * faults the other detectors own.
 *
 * No hardware and no Core Audio: this runs in `make test`.
 */

#include "loopback.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BLOCKS   16u
#define FRAMES   (BLOCKS * EMU_BLOCK)
#define BIN_L    85u          /* ~996 Hz at 48 kHz */
#define BIN_R    137u         /* ~1605 Hz; not a harmonic of 85 */
#define AMP      0.25         /* -12.04 dBFS */

static float gSignal[FRAMES];
static float gScratch[FRAMES];
static int   gFailures;
static int   gChecks;

static void check(int ok, const char* what, const char* detail)
{
    gChecks++;
    if (!ok) gFailures++;
    printf("  %-4s %-46s %s\n", ok ? "ok" : "FAIL", what, detail);
}

/*
 * The chirp again, but evaluated at a fractional time offset, so the delay
 * estimator can be asked for an answer that is not a whole frame. Deliberately
 * a separate expression from emu_gen_chirp: a test that reuses the code under
 * test only proves it agrees with itself.
 */
static void chirp_delayed(float* x, unsigned n, unsigned span, double delay,
                          double f0, double f1, double amp, double gain)
{
    memset(x, 0, (size_t)n * sizeof *x);
    for (unsigned i = 0; i < n; i++) {
        double t = (double)i - delay;
        if (t < 0.0 || t > (double)(span - 1)) continue;
        double sweep = f0 * t + 0.5 * (f1 - f0) * t * t / (double)span;
        double window = 0.5 - 0.5 * cos(2.0 * M_PI * t / (double)(span - 1));
        x[i] = (float)(gain * amp * window * sin(2.0 * M_PI * sweep));
    }
}

int emu_analysis_selftest(void)
{
    char detail[160];
    EmuToneSummary s;

    printf("analysers, against signals whose faults are known\n\n");

    /* --- a clean tone must look clean ------------------------------------ */
    emu_gen_tone(gSignal, FRAMES, BIN_L, AMP);
    emu_tone_scan(gSignal, FRAMES, BIN_L, BIN_R, &s);

    snprintf(detail, sizeof detail, "%.2f dBFS, wanted -12.04", s.level_mean_db);
    check(fabs(s.level_mean_db - 20.0 * log10(AMP)) < 0.05, "level of a -12 dBFS tone", detail);

    snprintf(detail, sizeof detail, "THD+N %.1f dB over %u blocks", s.thdn_worst_db, s.blocks);
    check(s.thdn_worst_db < -100.0, "a clean tone reads clean", detail);

    snprintf(detail, sizeof detail, "phase span %.4f deg, %.4f frames",
             s.phase_span_deg, s.frames_slipped);
    check(s.phase_span_deg < 0.01, "a clean tone does not move", detail);

    snprintf(detail, sizeof detail, "%.3g", s.discontinuity);
    check(s.discontinuity < 1e-6, "block-to-block difference is nil", detail);

    /* --- a tone the analyser is not tuned to is not mistaken for one ------ */
    emu_gen_tone(gSignal, FRAMES, BIN_R, AMP);
    emu_tone_scan(gSignal, FRAMES, BIN_L, BIN_R, &s);
    snprintf(detail, sizeof detail, "own bin %.1f dBFS, rival bin %.1f dBFS",
             s.level_mean_db, s.rival_worst_db);
    check(s.level_mean_db < -60.0 && s.rival_worst_db > -13.0,
          "a swapped channel shows in the rival bin", detail);

    /* --- distortion of a known size reads that size ----------------------- */
    emu_gen_tone(gSignal, FRAMES, BIN_L, AMP);
    emu_gen_tone(gScratch, FRAMES, BIN_L * 2, AMP * 0.01);   /* 2nd harmonic, -40 dB */
    for (unsigned i = 0; i < FRAMES; i++) gSignal[i] += gScratch[i];
    emu_tone_scan(gSignal, FRAMES, BIN_L, BIN_R, &s);
    snprintf(detail, sizeof detail, "read %.2f dB, put in -40.00 dB", s.thdn_mean_db);
    check(fabs(s.thdn_mean_db + 40.0) < 0.2, "a -40 dB harmonic reads as -40 dB", detail);

    /* --- a hole: the ring handed back nothing for a while ----------------- */
    emu_gen_tone(gSignal, FRAMES, BIN_L, AMP);
    const uint64_t hole_at = 5 * EMU_BLOCK + 1000;
    memset(gSignal + hole_at, 0, 100 * sizeof *gSignal);
    emu_tone_scan(gSignal, FRAMES, BIN_L, BIN_R, &s);

    snprintf(detail, sizeof detail, "THD+N %.1f dB in block %u, put the hole in block %llu",
             s.thdn_worst_db, s.worst_thdn_block, (unsigned long long)(hole_at / EMU_BLOCK));
    check(s.thdn_worst_db > -40.0 && s.worst_thdn_block == hole_at / EMU_BLOCK,
          "a 100-frame hole is caught, in the right block", detail);

    snprintf(detail, sizeof detail, "peak %.3f at frame %llu, put it at %llu",
             s.discontinuity, (unsigned long long)s.discontinuity_frame,
             (unsigned long long)hole_at);
    check(s.discontinuity > AMP * 0.5
          && s.discontinuity_frame >= hole_at
          && s.discontinuity_frame < hole_at + 100 + EMU_BLOCK,
          "and located to the frame", detail);

    /* --- a slip: frames went missing, and everything after moved ---------- */
    /* Placed on a block boundary so no block straddles the splice: this is the
     * case THD+N cannot see and only the phase can. */
    const unsigned slip_frames = 3;
    const unsigned slip_block = 8;
    for (uint64_t i = 0; i < FRAMES; i++) {
        uint64_t source = i < (uint64_t)slip_block * EMU_BLOCK ? i : i + slip_frames;
        double phase = 2.0 * M_PI * (double)BIN_L * (double)(source % EMU_BLOCK)
                     / (double)EMU_BLOCK;
        gSignal[i] = (float)(AMP * sin(phase));
    }
    emu_tone_scan(gSignal, FRAMES, BIN_L, BIN_R, &s);

    snprintf(detail, sizeof detail, "read %.3f frames in block %u, dropped %u at block %u",
             s.frames_slipped, s.worst_phase_block, slip_frames, slip_block);
    check(fabs(s.frames_slipped - slip_frames) < 0.05 && s.worst_phase_block == slip_block,
          "a 3-frame slip is counted, in the right block", detail);

    snprintf(detail, sizeof detail, "THD+N still %.1f dB", s.thdn_worst_db);
    check(s.thdn_worst_db < -100.0, "and is invisible to THD+N, as expected", detail);

    /* --- a clock mismatch: the phase walks -------------------------------- */
    /* One frame per block of accumulated drift, which is what a capture clock
     * running fast against the playback clock would do. */
    {
        double drift = 0.0;
        for (uint64_t i = 0; i < FRAMES; i++) {
            drift = (double)i / (double)EMU_BLOCK;      /* frames, growing */
            double phase = 2.0 * M_PI * (double)BIN_L * ((double)i + drift)
                         / (double)EMU_BLOCK;
            gSignal[i] = (float)(AMP * sin(phase));
        }
    }
    emu_tone_scan(gSignal, FRAMES, BIN_L, BIN_R, &s);
    double expected_slope = 360.0 * (double)BIN_L / (double)EMU_BLOCK;   /* deg/block */
    snprintf(detail, sizeof detail, "%.3f deg/block, wanted %.3f",
             s.phase_slope_deg, expected_slope);
    check(fabs(s.phase_slope_deg - expected_slope) < 0.3,
          "a one-frame-per-block drift reads as drift", detail);

    /* --- the delay estimator ---------------------------------------------- */
    const unsigned chirp_len = 960;
    static float chirp[960];
    emu_gen_chirp(chirp, chirp_len, 200.0 / 48000.0, 8000.0 / 48000.0, 0.5);

    struct { double delay; double gain; const char* what; } cases[] = {
        {  137.0,  1.0, "a 137-frame delay is found exactly" },
        { 1024.25, 1.0, "a quarter-frame delay is interpolated" },
        {  512.0, -1.0, "an inverted loopback is reported inverted" },
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        chirp_delayed(gScratch, FRAMES, chirp_len, cases[c].delay,
                      200.0 / 48000.0, 8000.0 / 48000.0, 0.5, cases[c].gain);
        double quality = 0.0;
        int polarity = 0;
        double found = emu_find_delay(chirp, chirp_len, gScratch, FRAMES, 8192,
                                      &quality, &polarity);
        snprintf(detail, sizeof detail, "found %.3f, put it at %.3f (r=%.4f, polarity %+d)",
                 found, cases[c].delay, quality, polarity);
        check(fabs(found - cases[c].delay) < 0.25 && quality > 0.9
              && polarity == (cases[c].gain < 0 ? -1 : 1),
              cases[c].what, detail);
    }

    /* The same, buried in noise, because a real capture is not noiseless. */
    chirp_delayed(gScratch, FRAMES, chirp_len, 733.0, 200.0 / 48000.0,
                  8000.0 / 48000.0, 0.5, 1.0);
    uint32_t rng = 12345;
    for (unsigned i = 0; i < FRAMES; i++) {
        rng = rng * 1664525u + 1013904223u;
        gScratch[i] += (float)(((double)(rng >> 8) / 8388608.0 - 1.0) * 0.02);
    }
    {
        double quality = 0.0;
        int polarity = 0;
        double found = emu_find_delay(chirp, chirp_len, gScratch, FRAMES, 8192,
                                      &quality, &polarity);
        snprintf(detail, sizeof detail, "found %.3f, put it at 733 (r=%.4f)", found, quality);
        check(fabs(found - 733.0) < 0.5, "and still found under -34 dB of noise", detail);
    }

    printf("\n  %d checks, %d failed\n", gChecks, gFailures);
    return gFailures;
}
