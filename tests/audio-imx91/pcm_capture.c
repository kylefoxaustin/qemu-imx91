/*
 * Minimal ALSA capture oracle for the i.MX 91 audio cards.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Records from an ALSA capture PCM and checks the buffer carries real,
 * non-silent, varying samples. Two cards use it: the wm8962/SAI3 card
 * (hw:1,0, S16_LE - the SAI3 RX FIFO -> eDMA path) and the MICFIL PDM card
 * (hw:2,0, S32_LE - the MICFIL DATACH0 -> eDMA path). Both models synthesise a
 * sawtooth (no physical codec/mic is wired), so a working capture path returns
 * real signal. Prints "CAP[...]: PASS (non-silent)" on success. There is no
 * arecord in the BSP image, hence this. Cross-compile against an ALSA sysroot.
 *
 * Usage: pcm_capture <dev> [frames] [S16|S32] [channels] [rate]  (S16/2ch/48k)
 *
 * With [rate] set it also times the capture in GUEST monotonic time and prints
 * CAPDUR: the model feeds samples at the sample rate the guest programmed, so
 * capturing N frames must take N/rate seconds.  A model that feeds at a fixed
 * 48 kHz regardless of the programmed rate finishes a 16 kHz capture 3x early --
 * duration, not the (rate-independent) waveform, is what catches it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:1,0";
    long frames = argc > 2 ? atol(argv[2]) : 4096;
    int s32 = argc > 3 && strcmp(argv[3], "S32") == 0;
    snd_pcm_format_t fmt = s32 ? SND_PCM_FORMAT_S32_LE : SND_PCM_FORMAT_S16_LE;
    unsigned int chans = argc > 4 ? (unsigned)atoi(argv[4]) : 2;
    unsigned int rate = argc > 5 ? (unsigned)atoi(argv[5]) : 48000;
    snd_pcm_t *pcm;
    void *buf;
    snd_pcm_sframes_t r;
    long i, samples, nonzero = 0, distinct = 0;
    long long peak = 0, prev = 0;
    struct timespec t0, t1;
    double dur;
    int err;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        printf("CAP[%s]: open: %s\n", dev, snd_strerror(err));
        return 1;
    }
    err = snd_pcm_set_params(pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED, chans,
                             rate, 1, 500000);
    if (err < 0) {
        printf("CAP[%s]: set_params: %s\n", dev, snd_strerror(err));
        return 1;
    }
    printf("CAP[%s]: %u Hz %u ch %s, reading %ld frames\n", dev, rate, chans,
           s32 ? "S32_LE" : "S16_LE", frames);

    /*
     * Start the stream explicitly. snd_pcm_set_params leaves the capture
     * stream PREPARED but does not auto-start it on the first readi here, so
     * without this the read returns -EIO. An explicit start enables the
     * receiver (SAI RCSR.RE / MICFIL CTRL1.PDMIEN) and the eDMA channel.
     */
    err = snd_pcm_start(pcm);
    if (err < 0) {
        printf("CAP[%s]: start: %s\n", dev, snd_strerror(err));
        return 1;
    }

    buf = malloc(frames * chans * (s32 ? 4 : 2));
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r = snd_pcm_readi(pcm, buf, frames);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("CAP[%s]: readi -> %ld\n", dev, (long)r);
    if (r < 0) {
        printf("CAP[%s]: FAIL (%s)\n", dev, snd_strerror((int)r));
        return 1;
    }

    /*
     * The capture takes N/rate seconds of guest time -- but only if the model
     * feeds at the rate the guest programmed.  Print it so the harness can
     * assert it; a fixed-48kHz feed makes a 16 kHz capture finish 3x early.
     */
    dur = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("CAPDUR[%s]: rate=%u frames=%ld dur=%.3f expect=%.3f\n",
           dev, rate, (long)r, dur, (double)r / rate);

    samples = r * chans;
    for (i = 0; i < samples; i++) {
        long long v = s32 ? ((int32_t *)buf)[i] : ((short *)buf)[i];
        long long a = v < 0 ? -v : v;

        if (a > peak) {
            peak = a;
        }
        if (v) {
            nonzero++;
        }
        if (i && v != prev) {
            distinct++;
        }
        prev = v;
    }
    printf("CAP[%s]: frames=%ld peak=%lld nonzero=%ld changes=%ld\n",
           dev, (long)r, peak, nonzero, distinct);

    /* Real signal: a meaningful peak and a varying (not stuck) waveform. */
    if (peak > (s32 ? (1 << 20) : 1000) && nonzero > r && distinct > 8) {
        printf("CAP[%s]: PASS (non-silent)\n", dev);
        err = 0;
    } else {
        printf("CAP[%s]: FAIL (silent/stuck)\n", dev);
        err = 1;
    }
    snd_pcm_close(pcm);
    return err;
}
