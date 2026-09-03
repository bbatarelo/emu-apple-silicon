/*
 * The analysers behind the loopback tests. See loopback.h for why the tones
 * are placed on FFT bins; everything here follows from that.
 *
 * No hardware, no Core Audio, no allocation: this file is driven by
 * `hal-loopback selftest` from signals with faults built into them, which is
 * the only way to know a "no glitches" result means anything.
 */

#include "loopback.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Scratch for one block. Single-threaded, on the main thread, after the stream
 * has stopped -- nothing here runs while audio is moving. */
static double gRe[EMU_BLOCK];
static double gIm[EMU_BLOCK];

void emu_fft(double* re, double* im, unsigned n)
{
    /* Bit-reversal permutation. */
    for (unsigned i = 1, j = 0; i < n; i++) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (unsigned i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (unsigned k = 0; k < len / 2; k++) {
                double ur = re[i + k],           ui = im[i + k];
                double vr = re[i + k + len / 2], vi = im[i + k + len / 2];
                double tr = vr * cr - vi * ci;
                double ti = vr * ci + vi * cr;
                re[i + k] = ur + tr;             im[i + k] = ui + ti;
                re[i + k + len / 2] = ur - tr;   im[i + k + len / 2] = ui - ti;
                double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static double amplitude_db(double amp)
{
    return amp > 1e-12 ? 20.0 * log10(amp) : -240.0;
}

void emu_tone_block(const float* x, unsigned n, unsigned bin, unsigned rival,
                    EmuToneBlock* out)
{
    memset(out, 0, sizeof *out);
    if (n > EMU_BLOCK) n = EMU_BLOCK;

    double peak = 0.0;
    for (unsigned i = 0; i < n; i++) {
        gRe[i] = (double)x[i];
        gIm[i] = 0.0;
        double a = fabs((double)x[i]);
        if (a > peak) peak = a;
    }
    out->peak = peak;

    emu_fft(gRe, gIm, n);

    /* Single-sided amplitude: a real tone of amplitude A puts A*n/2 in its
     * bin, so the bin's amplitude is 2|X|/n. Bin 0 is not doubled. */
    const double scale = 2.0 / (double)n;

    double tone = hypot(gRe[bin], gIm[bin]) * scale;
    out->level_db = amplitude_db(tone);
    out->phase_deg = atan2(gIm[bin], gRe[bin]) * 180.0 / M_PI;
    out->dc_db = amplitude_db(fabs(gRe[0]) / (double)n);
    out->rival_db = (rival != bin)
                  ? amplitude_db(hypot(gRe[rival], gIm[rival]) * scale)
                  : -240.0;

    /*
     * Everything that is not the tone, as an amplitude ratio. The tone's own
     * bin and its immediate neighbours go, because a bin-centred tone still
     * leaks a little into them through the analogue path's phase response, and
     * so does the other channel's tone -- crosstalk is reported separately
     * rather than charged to this channel's distortion. DC likewise.
     */
    double residual = 0.0;
    for (unsigned b = 1; b < n / 2; b++) {
        if (b + 1 >= bin && b <= bin + 1) continue;
        if (rival != bin && b + 1 >= rival && b <= rival + 1) continue;
        double a = hypot(gRe[b], gIm[b]) * scale;
        residual += a * a;
    }
    residual = sqrt(residual);
    out->thdn_db = tone > 1e-12 ? amplitude_db(residual / tone) : 0.0;
}

void emu_tone_scan(const float* x, uint64_t count, unsigned bin, unsigned rival,
                   EmuToneSummary* out)
{
    memset(out, 0, sizeof *out);
    out->level_min_db = 1e9;
    out->level_max_db = -1e9;
    out->thdn_worst_db = -1e9;
    out->rival_worst_db = -1e9;
    out->dc_worst_db = -1e9;

    unsigned blocks = (unsigned)(count / EMU_BLOCK);
    if (blocks == 0) return;

    double level_sum = 0.0, thdn_sum = 0.0;
    double unwrapped = 0.0, previous_raw = 0.0;
    double phase_min = 0.0, phase_max = 0.0;
    /* Least squares of unwrapped phase against block index. */
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;

    for (unsigned b = 0; b < blocks; b++) {
        EmuToneBlock blk;
        emu_tone_block(x + (uint64_t)b * EMU_BLOCK, EMU_BLOCK, bin, rival, &blk);

        if (blk.level_db < out->level_min_db) out->level_min_db = blk.level_db;
        if (blk.level_db > out->level_max_db) out->level_max_db = blk.level_db;
        level_sum += blk.level_db;

        if (blk.thdn_db > out->thdn_worst_db) {
            out->thdn_worst_db = blk.thdn_db;
            out->worst_thdn_block = b;
        }
        thdn_sum += blk.thdn_db;

        if (blk.rival_db > out->rival_worst_db) out->rival_worst_db = blk.rival_db;
        if (blk.dc_db > out->dc_worst_db) out->dc_worst_db = blk.dc_db;
        if (blk.peak > out->peak) out->peak = blk.peak;

        if (b == 0) {
            previous_raw = blk.phase_deg;
            unwrapped = 0.0;
        } else {
            double delta = blk.phase_deg - previous_raw;
            while (delta > 180.0) delta -= 360.0;
            while (delta <= -180.0) delta += 360.0;
            previous_raw = blk.phase_deg;
            unwrapped += delta;
            if (fabs(delta) > out->phase_step_max_deg) {
                out->phase_step_max_deg = fabs(delta);
                out->worst_phase_block = b;
            }
        }
        if (unwrapped < phase_min) phase_min = unwrapped;
        if (unwrapped > phase_max) phase_max = unwrapped;

        double bx = (double)b;
        sx += bx; sy += unwrapped; sxx += bx * bx; sxy += bx * unwrapped;
    }

    out->blocks = blocks;
    out->level_mean_db = level_sum / blocks;
    out->thdn_mean_db = thdn_sum / blocks;
    out->phase_span_deg = phase_max - phase_min;

    /* One frame of movement is this many degrees at the tone's bin. */
    double deg_per_frame = 360.0 * (double)bin / (double)EMU_BLOCK;
    out->frames_slipped = deg_per_frame > 0.0 ? out->phase_span_deg / deg_per_frame : 0.0;

    double denominator = blocks * sxx - sx * sx;
    out->phase_slope_deg = fabs(denominator) > 1e-9
                         ? (blocks * sxy - sx * sy) / denominator : 0.0;

    /* The same periodicity, in the time domain: two points one period apart
     * must hold the same sample. Locates a glitch to the frame. */
    for (uint64_t i = EMU_BLOCK; i < count; i++) {
        double d = fabs((double)x[i] - (double)x[i - EMU_BLOCK]);
        if (d > out->discontinuity) {
            out->discontinuity = d;
            out->discontinuity_frame = i;
        }
    }
}

/* Normalised cross-correlation of `ref` against `sig` at one lag. */
static double correlate_at(const float* ref, unsigned reflen, double ref_energy,
                           const float* sig, uint64_t lag)
{
    const float* s = sig + lag;
    double dot = 0.0, energy = 0.0;
    for (unsigned i = 0; i < reflen; i++) {
        dot += (double)ref[i] * s[i];
        energy += (double)s[i] * s[i];
    }
    double norm = sqrt(ref_energy * energy);
    return norm > 1e-20 ? dot / norm : 0.0;
}

double emu_find_delay(const float* ref, unsigned reflen,
                      const float* sig, uint64_t siglen, unsigned maxlag,
                      double* quality, int* polarity)
{
    if (quality) *quality = 0.0;
    if (polarity) *polarity = 0;
    if (reflen == 0 || siglen < (uint64_t)maxlag + reflen + 1) return -1.0;

    double ref_energy = 0.0;
    for (unsigned i = 0; i < reflen; i++) ref_energy += (double)ref[i] * ref[i];
    if (ref_energy <= 0.0) return -1.0;

    /* Window energy is carried forward instead of recomputed per lag; the
     * correlation itself is the O(reflen) part and stays honest. */
    double window = 0.0;
    for (unsigned i = 0; i < reflen; i++) window += (double)sig[i] * sig[i];

    double best = -1.0, best_signed = 0.0;
    unsigned best_lag = 0;

    for (unsigned lag = 0; lag <= maxlag; lag++) {
        const float* s = sig + lag;
        double dot = 0.0;
        for (unsigned i = 0; i < reflen; i++) dot += (double)ref[i] * s[i];

        double norm = sqrt(ref_energy * window);
        double c = norm > 1e-20 ? dot / norm : 0.0;
        if (fabs(c) > best) { best = fabs(c); best_signed = c; best_lag = lag; }

        window -= (double)sig[lag] * sig[lag];
        window += (double)sig[lag + reflen] * sig[lag + reflen];
    }

    if (quality) *quality = best;
    if (polarity) *polarity = best_signed < 0.0 ? -1 : 1;

    /* Parabola through the peak and its two neighbours, recomputed exactly, so
     * the answer is not quantised to a frame. */
    double refined = (double)best_lag;
    if (best_lag > 0 && (uint64_t)best_lag + reflen + 1 <= siglen) {
        double left  = fabs(correlate_at(ref, reflen, ref_energy, sig, best_lag - 1));
        double right = fabs(correlate_at(ref, reflen, ref_energy, sig, best_lag + 1));
        double denominator = left - 2.0 * best + right;
        if (fabs(denominator) > 1e-15) {
            double shift = 0.5 * (left - right) / denominator;
            if (shift > -1.0 && shift < 1.0) refined += shift;
        }
    }
    return refined;
}

void emu_gen_tone(float* x, uint64_t n, unsigned bin, double amp)
{
    for (uint64_t i = 0; i < n; i++) {
        double phase = 2.0 * M_PI * (double)bin * (double)(i % EMU_BLOCK) / (double)EMU_BLOCK;
        x[i] = (float)(amp * sin(phase));
    }
}

void emu_gen_chirp(float* x, unsigned n, double f0_norm, double f1_norm, double amp)
{
    for (unsigned i = 0; i < n; i++) {
        double t = (double)i;
        double sweep = f0_norm * t + 0.5 * (f1_norm - f0_norm) * t * t / (double)n;
        double window = 0.5 - 0.5 * cos(2.0 * M_PI * t / (double)(n - 1));
        x[i] = (float)(amp * window * sin(2.0 * M_PI * sweep));
    }
}
