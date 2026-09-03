/*
 * Milestone 4 -- internally duplex USB engine.
 *
 * Capture and playback run together, with capture acting as the clock
 * reference that sizes playback packets.
 */

#pragma once

#include <stdbool.h>
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
    /// Run playback with the capture interface never opened at all.
    ///
    /// Capture has always had to run, at every rate, because its packet
    /// lengths were the only measurement of the device's clock the planner
    /// had. The explicit feedback endpoint lives on the *playback* interface
    /// and states the same demand directly, so with it the dependency is gone
    /// and playback can run alone -- which halves what is on the bus at
    /// 176.4 and 192 kHz, the two rates that crackle.
    ///
    /// Nothing records the result in this mode, by construction: the thing
    /// being switched off is the converter that would hear it. The verdict is
    /// by ear.
    bool     playback_only;
    /// Claim the capture interface and select its alternate setting, but never
    /// queue a read on it.
    ///
    /// Between this and `playback_only` sits the question `playback_only`
    /// alone cannot answer. Turning capture off removes two things at once:
    /// the IN traffic on the bus, and the device's own converter path running.
    /// Here the device is streaming -- its ADC clocked, its FIFO filling --
    /// and the host simply never issues an IN token, so nothing of it reaches
    /// the wire. What changes between the two modes is the bus; what changes
    /// between this and duplex is the bus as well.
    ///
    /// Its FIFO has nowhere to go, so expect this mode to leave the device in
    /// the state a `bInterval 3` stream leaves behind, and worse.
    bool     capture_idle;
    /// Size playback from the feedback endpoint even in full duplex.
    ///
    /// The other modes that do this also change what is on the bus, so they
    /// cannot separate "following the demand" from "removing the traffic".
    /// This changes only the planner: capture runs exactly as it always does
    /// and its measurements are simply not used. It is the direct test of
    /// whether the demand drifts down because it is being over-delivered --
    /// follow it, and if the drift is a response to the surplus it should not
    /// happen.
    bool     plan_from_device;
    /// Write every feedback value to this path as CSV, with the host time it
    /// arrived at.
    ///
    /// The aggregate counts say how many excursions a run had, not when. With
    /// the crackle now known to have an onset -- clean for the first ten or
    /// fifteen seconds and breaking up after -- when is the whole question,
    /// and this is the only channel the device has that changes at all.
    /// NULL writes nothing.
    const char* feedback_trace;
    /// Poll the capture endpoint on one service interval in this many.
    ///
    /// 0 and 1 both mean every interval, which is ordinary duplex. Higher
    /// values leave the remaining intervals with a zero-length frame-list
    /// entry, so the host issues no IN token for them and the transaction
    /// never happens -- the IN traffic is thinned rather than stopped.
    ///
    /// This is the dose question. Stopping capture altogether cleans the
    /// crackle up; if halving the traffic halves it, the mechanism is loading
    /// of some kind, and if any polling at all brings it back in full, it is
    /// not. The captured audio is full of holes in this mode and is not worth
    /// looking at; packets are sized from the feedback endpoint, because a
    /// thinned capture stream cannot size them.
    uint32_t capture_duty;
    /// Milliseconds to start the playback schedule after the capture one.
    ///
    /// Both schedules start on the same bus frame by default, so their
    /// requests complete together and their transactions share the same
    /// service intervals. E-MU's Windows driver does not: it offsets the start
    /// of playback by +3 ms relative to record (`m_SynchronizationDelay`).
    /// With the crackle localised to the presence of IN traffic, the phase
    /// between the two is the first variable that might explain *why* it
    /// matters -- and unlike stopping capture, an offset is something a real
    /// driver could adopt.
    uint32_t sync_delay_ms;
    /// Prefer the shortest service interval a rate offers, rather than the
    /// longest. 44.1 through 96 kHz are published on both a 1 ms and a 0.5 ms
    /// endpoint; the top two rates only on 0.5 ms. Running a low rate on the
    /// short interval puts it on the same endpoint geometry as the rates that
    /// crackle, which is what separates "the interval" from "the rate".
    bool     short_interval;
    /// Milliseconds of audio queued per isochronous request. 0 keeps the
    /// default 8.
    ///
    /// This is the one difference from E-MU's Windows driver that the
    /// synchronisation-delay check turned up and nothing here has varied: it
    /// queues 1 ms URBs where this queues 8 ms and the plug-in 2 ms. Nothing
    /// about the wire changes -- the endpoint is still serviced every 0.5 ms
    /// and carries the same packets -- only how often the host is interrupted
    /// and how the two directions' completions interleave. It therefore
    /// separates what happens on the bus from what happens in the host, which
    /// is exactly the distinction `play` against `play-idle` could not make.
    ///
    /// It also shortens the schedule: with a fixed eight requests in flight,
    /// 1 ms requests plan 8 ms ahead where 8 ms requests plan 64. Two
    /// variables, so read a difference as "granularity or depth", not either
    /// alone.
    uint32_t request_ms;
    /// Follow the feedback endpoint's word as sent, without correcting its
    /// fixed-point scaling.
    ///
    /// The firmware scales the fraction by 64000 where the format says 65536,
    /// so the whole 44.1 kHz family reads 53.1 ppm low. Capture used to cover
    /// for that -- it sized the packets and the endpoint was only a second
    /// opinion -- but with capture off the endpoint is the only clock there is,
    /// and 53.1 ppm at 176.4 kHz is 9.4 frames a second the device does not
    /// get. This exists to hear the difference rather than assert it.
    bool     feedback_raw;
} DuplexConfig;

int emu_duplex_run(IOUSBDeviceInterface500** dev,
                   const EmuDeviceModel* model,
                   const DuplexConfig* config);
