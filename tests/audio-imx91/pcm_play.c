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
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:1,0";
    unsigned int chans = 2;
    unsigned int secs = argc > 2 ? atoi(argv[2]) : 2;
    unsigned int rate = argc > 3 ? (unsigned)atoi(argv[3]) : 48000;
    snd_pcm_t *pcm;
    int err, i;
    long frames = (long)rate * secs;
    short *buf;
    snd_pcm_sframes_t w;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        printf("PLAY[%s]: open: %s\n", dev, snd_strerror(err));
        return 1;
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, chans, rate,
                             1, 200000);
    if (err < 0) {
        printf("PLAY[%s]: set_params: %s\n", dev, snd_strerror(err));
        return 1;
    }
    printf("PLAY[%s]: %u Hz %u ch S16_LE, %ld frames\n", dev, rate, chans,
           frames);

    /* 440 Hz square wave: period ~109 frames at 48 kHz. */
    buf = malloc(frames * chans * sizeof(short));
    for (i = 0; i < frames; i++) {
        short v = ((i / 55) & 1) ? 8000 : -8000;
        buf[i * 2] = v;
        buf[i * 2 + 1] = v;
    }

    w = snd_pcm_writei(pcm, buf, frames);
    printf("PLAY[%s]: writei -> %ld\n", dev, (long)w);
    if (w < 0) {
        printf("PLAY[%s]: FAIL (%s)\n", dev, snd_strerror((int)w));
        return 1;
    }
    err = snd_pcm_drain(pcm);
    printf("PLAY[%s]: drain -> %s\n", dev, snd_strerror(err));
    snd_pcm_close(pcm);
    printf("PLAY[%s]: %s (%ld frames)\n", dev,
           w == frames ? "PASS" : "PARTIAL", (long)w);
    return 0;
}
