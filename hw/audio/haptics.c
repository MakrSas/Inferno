/*
 * The taptic engine's drive signal, condensed for an embedder.
 *
 * The guest plays haptics the way it plays sound. Its audio server renders a waveform for the actuator
 * and the kernel sends it down the i2s port the AOP drives for it, alc2, over SIO DMA channel 0x90,
 * as 32-bit mono at 48 kHz. That much was read off the channel on the rig, and so was how a sample is
 * written, which is not as plain PCM. A sample goes out as half of its sign plus a term logarithmic in
 * its magnitude: the term is zero at full scale, and every doubling of the magnitude raises it by
 * about 0.0078 — 6 dB. Whatever the strength, then, the drive looks like a square wave between about
 * +0.5 and -0.5, and the strength sits in how far both plateaus are shifted down. A continuous event
 * at full intensity puts its loudest samples at -0.0076; a transient at the same intensity comes out
 * within 2 dB of that.
 *
 * A phone cannot play the waveform as it is either: its haptics engine takes an intensity and a
 * sharpness, not samples. So it is condensed here into frames of ten milliseconds — the intensity the
 * guest drove the actuator at and the frequency it drove it at — and the last few seconds of those
 * wait for the embedder to read.
 *
 * Copyright (c) 2026 Inferno iOS port.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "hw/audio/haptics.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "qemu/lockable.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "ui/inferno-embed.h"

#define HAPTICS_RATE          (48000)
#define HAPTICS_FRAME_SAMPLES (HAPTICS_RATE * INFERNO_HAPTIC_FRAME_MS / 1000)
#define HAPTICS_RING_FRAMES   (512)
// A crossing counts once the signal has gone this far past zero, either way, so that noise around zero
// cannot read as a high frequency. It is also the level below which the actuator counts as still.
#define HAPTICS_THRESHOLD     (0.01)
// Anything slower than this between two crossings is a pause in the driving, not half of a period.
#define HAPTICS_LONGEST_HALF  (HAPTICS_RATE / 40)
// And anything quicker is a glitch — a gap in the stream, the edge of a buffer the guest wrote late — not
// a vibration: no taptic engine is driven at hundreds of hertz, let alone a thousand.
#define HAPTICS_SHORTEST_HALF (HAPTICS_RATE / 1000)

// The log term: how much a doubling of the magnitude adds, and where the loudest samples of a continuous
// event at full intensity put it. Measured on the rig against Core Haptics events of known intensity,
// from 0.02 to 1.
#define HAPTICS_OCTAVE (0.0078)
#define HAPTICS_FULL   (-0.0076)
// A sample only counts towards the strength when it sits on a plateau with a plausible term: that leaves
// out silence, the few samples an edge passes through, and the quietest tails of a fade.
#define HAPTICS_PLATEAU_MIN (0.3)
#define HAPTICS_PLATEAU_MAX (0.75)
#define HAPTICS_TERM_MIN    (-0.25)
#define HAPTICS_TERM_MAX    (0.03)

/*
 * The guest writes its samples into buffers the kernel queued on the channel well ahead, 63360 bytes at a
 * time. When its audio thread falls behind, part of a buffer goes out holding what the same memory held a
 * buffer earlier: an echo, a third of a second late, of a vibration that may already be over. On the rig
 * such stretches were the stream one or two buffers back byte for byte — after bursts of activity, and
 * inside long events too.
 *
 * So every sample that is not silence is held against the samples one and two buffers back. A long enough
 * run of matches is an echo — a waveform the guest actually rendered does not repeat itself bit for bit
 * after a third of a second — and a frame mostly made of echo carries nothing new. In the middle of a
 * vibration the last word then stands for a while, so that a stall does not chop it up; after one, an echo
 * is not taken for a new vibration at all.
 */
#define HAPTICS_BUFFER_BYTES  (63360)
#define HAPTICS_HISTORY_BYTES (1 << 17)
#define HAPTICS_ECHO_RUN      (16)
#define HAPTICS_ECHO_HOLD     (15)
// A frame with less than half a millisecond of sound in it holds a stray sample or two, not a vibration.
#define HAPTICS_MIN_SOUNDING  (24)

typedef struct
{
    QemuMutex          lock;
    QemuCond           ready;
    InfernoHapticFrame ring[HAPTICS_RING_FRAMES];
    uint32_t           head;
    uint32_t           used;

    /*
     * The frame being gathered. Only the feeder touches these, and there is one feeder — the machine's
     * audio drain, on the main loop — so they need no lock.
     */
    uint32_t samples;
    double   peak;
    double   loudest;
    bool     measured;
    int      sign;
    uint32_t since_crossing;
    uint32_t half_periods;
    uint64_t half_period_samples;
    uint32_t sounding;
    uint32_t echoed;
    float    frequency;
    float    level;
    bool     still;
    // Frames in a row that were echoes.
    uint32_t stale_frames;
    // Samples in a row that matched the stream a buffer or two back.
    uint32_t echo_run;
    // Bytes of the stream seen so far, which is where a frame sits in INFERNO_HAPTICS_DUMP, and the last
    // HAPTICS_HISTORY_BYTES of them.
    uint64_t fed;
    uint8_t  history[HAPTICS_HISTORY_BYTES];
} AppleHaptics;

// Nobody may be reading at all — the rig, Android — and then there is no point in doing the sums.
static int apple_haptics_wanted;

// INFERNO_HAPTICS_TRACE=1 prints every frame, and gathers frames with nobody reading, which is how they
// are checked on the rig.
static bool apple_haptics_trace(void)
{
    static int trace = -1;

    if (trace < 0) { trace = getenv("INFERNO_HAPTICS_TRACE") != NULL; }
    return trace != 0;
}

// INFERNO_HAPTICS_DUMP=<file> keeps the raw stream the frames were made of; a traced frame says where in
// it the frame ends.
static FILE* apple_haptics_dump(void)
{
    static int   opened;
    static FILE* dump;

    if (!opened) {
        const char* path = getenv("INFERNO_HAPTICS_DUMP");

        opened = 1;
        if (path != NULL) { dump = fopen(path, "wb"); }
    }
    return dump;
}

static AppleHaptics* apple_haptics_get(void)
{
    static AppleHaptics state = { .still = true, .loudest = -1.0 };
    static gsize        initialised;

    // The app may ask for frames before the machine exists, so the lock cannot wait for the machine.
    if (g_once_init_enter(&initialised)) {
        qemu_mutex_init(&state.lock);
        qemu_cond_init(&state.ready);
        g_once_init_leave(&initialised, 1);
    }
    return &state;
}

/*
 * Above about 165 Hz the guest's synthesiser eases off, by 3 dB at the top of its sharpness range, whatever
 * the intensity. Measured at full intensity and put back here, so that a frame carries the intensity the
 * guest asked for and the host's own engine can ease off in its own way.
 */
static double apple_haptics_rolloff(double hz)
{
    static const struct
    {
        double hz;
        double term;
    } points[] = {
        { 165, 0.0 },
        { 185, 0.0005 },
        { 204, 0.0019 },
        { 230, 0.0041 },
    };
    size_t i;

    if (hz <= points[0].hz) { return 0.0; }
    for (i = 1; i < ARRAY_SIZE(points); i++) {
        if (hz <= points[i].hz || i == ARRAY_SIZE(points) - 1) {
            return points[i - 1].term
                   + (points[i].term - points[i - 1].term) * (hz - points[i - 1].hz) / (points[i].hz - points[i - 1].hz);
        }
    }
    return 0.0;
}

// Takes one sample into the history and says whether it matches the stream one or two buffers back.
static bool apple_haptics_echoes(AppleHaptics* h, const uint8_t* sample)
{
    static const uint32_t lags[] = { HAPTICS_BUFFER_BYTES, 2 * HAPTICS_BUFFER_BYTES };
    const uint64_t        mask   = HAPTICS_HISTORY_BYTES - 1;
    bool                  echo   = false;
    size_t                l;
    size_t                k;

    for (l = 0; l < ARRAY_SIZE(lags) && !echo; l++) {
        echo = h->fed >= lags[l];
        for (k = 0; echo && k < 4; k++) { echo = h->history[(h->fed - lags[l] + k) & mask] == sample[k]; }
    }
    for (k = 0; k < 4; k++) { h->history[(h->fed + k) & mask] = sample[k]; }
    return echo;
}

static void apple_haptics_emit(AppleHaptics* h)
{
    InfernoHapticFrame frame = { 0 };
    bool               still;
    bool               stale = false;

    if (h->samples == 0) {
        if (h->still) { return; }
        still = true;
    }
    else {
        still = h->peak < HAPTICS_THRESHOLD || h->sounding < HAPTICS_MIN_SOUNDING;
        stale = !still && h->echoed * 2 >= h->sounding;
        if (!stale) {
            if (h->half_periods != 0) {
                h->frequency = (float)(HAPTICS_RATE * (double)h->half_periods / (2.0 * h->half_period_samples));
            }
            // No plateau in this frame — its edges only — leaves the strength where the last frame put it.
            if (h->measured) {
                double term = h->loudest - HAPTICS_FULL + apple_haptics_rolloff(h->frequency);

                h->level = (float)MIN(1.0, pow(2.0, term / HAPTICS_OCTAVE));
            }
        }
    }

    h->stale_frames = stale ? h->stale_frames + 1 : 0;
    if (stale && (h->still || h->stale_frames > HAPTICS_ECHO_HOLD)) { still = true; }
    if (!still) {
        frame.level     = h->level;
        frame.frequency = h->frequency;
    }

    if (apple_haptics_trace() && !(still && h->still)) {
        fprintf(stderr, "haptics: %.3f s level %.3f, %.0f Hz, loudest %+.4f @ %llu%s\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, frame.level, frame.frequency,
                h->measured ? h->loudest : 0.0, (unsigned long long)h->fed, stale ? " stale" : "");
    }

    h->samples             = 0;
    h->peak                = 0;
    h->loudest             = -1.0;
    h->measured            = false;
    h->half_periods        = 0;
    h->half_period_samples = 0;
    h->sounding            = 0;
    h->echoed              = 0;

    // One still frame says the driving stopped; after that, silence costs nothing and wakes nobody.
    if (still && h->still) { return; }
    h->still = still;

    WITH_QEMU_LOCK_GUARD(&h->lock)
    {
        uint32_t tail = (h->head + h->used) % HAPTICS_RING_FRAMES;

        h->ring[tail] = frame;
        if (h->used < HAPTICS_RING_FRAMES) { h->used++; }
        else {
            // A reader that has fallen this far behind wants the newest frames, not the oldest.
            h->head = (h->head + 1) % HAPTICS_RING_FRAMES;
        }
        qemu_cond_signal(&h->ready);
    }
}

void apple_haptics_feed(const uint8_t* data, size_t len)
{
    AppleHaptics* h;
    size_t        i;

    if (!qatomic_read(&apple_haptics_wanted) && !apple_haptics_trace()) { return; }

    h = apple_haptics_get();

    if (len < 4) {
        apple_haptics_emit(h);
        return;
    }

    if (apple_haptics_dump() != NULL) { fwrite(data, 1, len, apple_haptics_dump()); }

    for (i = 0; i + 4 <= len; i += 4) {
        uint32_t word      = ldl_le_p(data + i);
        double   v         = (int32_t)word / 2147483648.0;
        double   magnitude = fabs(v);
        bool     echo      = apple_haptics_echoes(h, data + i);

        h->fed  += 4;
        h->peak  = MAX(h->peak, magnitude);

        /*
         * Silence matches silence anywhere, so it neither makes an echo nor breaks one. A run counts from
         * its first sample once it is long enough to be sure of — as far back as this frame goes — or the
         * shortest echoes, a tap's worth, would never make up half of their frame.
         */
        if (word != 0) {
            h->echo_run = echo ? h->echo_run + 1 : 0;
            h->sounding++;
            if (h->echo_run == HAPTICS_ECHO_RUN) { h->echoed += MIN(HAPTICS_ECHO_RUN, h->sounding); }
            else if (h->echo_run > HAPTICS_ECHO_RUN) {
                h->echoed++;
            }
        }

        if (magnitude > HAPTICS_PLATEAU_MIN && magnitude < HAPTICS_PLATEAU_MAX) {
            double term = v - (v > 0 ? 0.5 : -0.5);

            if (term > HAPTICS_TERM_MIN && term < HAPTICS_TERM_MAX) {
                h->loudest  = MAX(h->loudest, term);
                h->measured = true;
            }
        }

        if (h->since_crossing < UINT32_MAX) { h->since_crossing++; }
        if ((v > HAPTICS_THRESHOLD && h->sign < 0) || (v < -HAPTICS_THRESHOLD && h->sign > 0)) {
            if (h->since_crossing >= HAPTICS_SHORTEST_HALF && h->since_crossing <= HAPTICS_LONGEST_HALF) {
                h->half_periods++;
                h->half_period_samples += h->since_crossing;
            }
            h->since_crossing = 0;
        }
        if (v > HAPTICS_THRESHOLD) { h->sign = 1; }
        else if (v < -HAPTICS_THRESHOLD) {
            h->sign = -1;
        }

        if (++h->samples == HAPTICS_FRAME_SAMPLES) { apple_haptics_emit(h); }
    }
}

size_t inferno_haptics_read(InfernoHapticFrame* out, size_t max, uint32_t timeout_ms)
{
    AppleHaptics* h;
    size_t        n = 0;

    if (out == NULL || max == 0) { return 0; }

    qatomic_set(&apple_haptics_wanted, 1);
    h = apple_haptics_get();

    QEMU_LOCK_GUARD(&h->lock);
    if (h->used == 0 && timeout_ms != 0) { qemu_cond_timedwait(&h->ready, &h->lock, timeout_ms); }
    while (n < max && h->used != 0) {
        out[n++] = h->ring[h->head];
        h->head  = (h->head + 1) % HAPTICS_RING_FRAMES;
        h->used--;
    }
    return n;
}
