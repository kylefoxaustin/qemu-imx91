/*
 * Minimal ALSA capture oracle for the i.MX 91 SAI3/wm8962 card.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Records from an ALSA capture PCM (default hw:1,0, the wm8962/SAI3 card),
 * driving the SAI3 RX FIFO -> eDMA datapath. The SAI model synthesises a
 * sawtooth (no codec is wired), so a working capture path returns real,
 * non-silent, varying samples. Prints "CAP[...]: PASS (non-silent)" when the
 * captured buffer has real signal. There is no arecord in the BSP image, hence
 * this. Cross-compile against an ALSA sysroot (see run.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:1,0";
    unsigned int rate = 48000, chans = 2;
    long frames = argc > 2 ? atol(argv[2]) : 4096;
    snd_pcm_t *pcm;
    short *buf;
    snd_pcm_sframes_t r;
    long i, nonzero = 0, distinct = 0;
    int peak = 0;
    short prev = 0;
    int err;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        printf("CAP[%s]: open: %s\n", dev, snd_strerror(err));
        return 1;
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, chans, rate,
                             1, 500000);
    if (err < 0) {
        printf("CAP[%s]: set_params: %s\n", dev, snd_strerror(err));
        return 1;
    }
    printf("CAP[%s]: %u Hz %u ch S16_LE, reading %ld frames\n", dev, rate,
           chans, frames);

    buf = malloc(frames * chans * sizeof(short));
    r = snd_pcm_readi(pcm, buf, frames);
    printf("CAP[%s]: readi -> %ld\n", dev, (long)r);
    if (r < 0) {
        printf("CAP[%s]: FAIL (%s)\n", dev, snd_strerror((int)r));
        return 1;
    }

    for (i = 0; i < r * chans; i++) {
        short v = buf[i];
        int a = v < 0 ? -v : v;

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
    printf("CAP[%s]: frames=%ld peak=%d nonzero=%ld changes=%ld "
           "first=%d,%d,%d,%d\n", dev, (long)r, peak, nonzero, distinct,
           buf[0], buf[1], buf[2], buf[3]);

    /* Real signal: a meaningful peak and a varying (not stuck) waveform. */
    if (peak > 1000 && nonzero > r && distinct > 8) {
        printf("CAP[%s]: PASS (non-silent)\n", dev);
        err = 0;
    } else {
        printf("CAP[%s]: FAIL (silent/stuck)\n", dev);
        err = 1;
    }
    snd_pcm_close(pcm);
    return err;
}
