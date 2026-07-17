/*
 * Minimal ALSA playback oracle for the i.MX93 SAI3/wm8962 card.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Plays a generated square wave to an ALSA PCM device (default hw:1,0, the
 * wm8962/SAI3 card), driving the eDMA3 cyclic -> SAI3 TX FIFO datapath. A clean
 * "PASS (N frames)" with drain success means the DMA paced the whole stream at
 * the audio rate without under-running. There is no aplay in the BSP, hence
 * this. Cross-compile against an ALSA sysroot (see run.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:1,0";
    unsigned int chans = 2;
    unsigned int secs = argc > 2 ? atoi(argv[2]) : 2;
    unsigned int rate = argc > 3 ? (unsigned)atoi(argv[3]) : 48000;
    const char *fmtname = argc > 4 ? argv[4] : "S16";
    /* SPDIF/XCVR needs IEC958 subframe (32-bit); default S16 for the SAI card. */
    snd_pcm_format_t fmt = SND_PCM_FORMAT_S16_LE;
    int width = 2;
    snd_pcm_t *pcm;
    int err, i;
    long frames = (long)rate * secs;
    void *buf;
    snd_pcm_sframes_t w;
    struct timespec t0, t1;
    double dur;

    if (!strcmp(fmtname, "S32")) {
        fmt = SND_PCM_FORMAT_S32_LE; width = 4;
    } else if (!strcmp(fmtname, "IEC958")) {
        fmt = SND_PCM_FORMAT_IEC958_SUBFRAME_LE; width = 4;
    }

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        printf("PLAY[%s]: open: %s\n", dev, snd_strerror(err));
        return 1;
    }
    err = snd_pcm_set_params(pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED, chans,
                             rate, 1, 200000);
    if (err < 0) {
        printf("PLAY[%s]: set_params: %s\n", dev, snd_strerror(err));
        return 1;
    }
    printf("PLAY[%s]: %u Hz %u ch %s, %ld frames\n", dev, rate, chans,
           fmtname, frames);

    /* Square wave toggling every 55 frames (tone = rate/110 Hz). */
    buf = malloc(frames * chans * width);
    for (i = 0; i < frames; i++) {
        if (width == 2) {
            short v = ((i / 55) & 1) ? 8000 : -8000;
            ((short *)buf)[i * 2] = v;
            ((short *)buf)[i * 2 + 1] = v;
        } else {
            int32_t v = ((i / 55) & 1) ? (8000 << 16) : -(8000 << 16);
            ((int32_t *)buf)[i * 2] = v;
            ((int32_t *)buf)[i * 2 + 1] = v;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    w = snd_pcm_writei(pcm, buf, frames);
    printf("PLAY[%s]: writei -> %ld\n", dev, (long)w);
    if (w < 0) {
        printf("PLAY[%s]: FAIL (%s)\n", dev, snd_strerror((int)w));
        return 1;
    }
    err = snd_pcm_drain(pcm);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("PLAY[%s]: drain -> %s\n", dev, snd_strerror(err));
    /*
     * writei+drain block until the hardware has clocked out every frame, so the
     * elapsed guest time is frames/rate -- but only if the TX feeds at the rate
     * the guest programmed.  A fixed-rate feed finishes a non-48kHz stream early.
     */
    dur = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("PLAYDUR[%s]: rate=%u frames=%ld dur=%.3f expect=%.3f\n",
           dev, rate, (long)w, dur, (double)w / rate);
    snd_pcm_close(pcm);
    printf("PLAY[%s]: %s (%ld frames)\n", dev,
           w == frames ? "PASS" : "PARTIAL", (long)w);
    return 0;
}
