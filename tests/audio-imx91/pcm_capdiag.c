/*
 * Granular ALSA capture diagnostic for the i.MX 91 SAI3/wm8962 card.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Unlike pcm_capture.c (a pass/fail oracle), this isolates WHICH step of the
 * capture bring-up fails: open, hw_params, sw_params, prepare, start, readi.
 * It prints the snd_pcm_state and the errno after each, so a silent EIO can be
 * pinned to the exact ALSA/ASoC stage. Built/run by run-capture.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>

static const char *st(snd_pcm_t *p)
{
    return snd_pcm_state_name(snd_pcm_state(p));
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "hw:1,0";
    unsigned int rate = 48000, chans = 2;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_t *pcm;
    short buf[2048 * 2];
    snd_pcm_sframes_t r;
    int err, dir = 0;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    printf("DIAG open: %s state=%s\n", snd_strerror(err), err ? "-" : st(pcm));
    if (err < 0) {
        return 1;
    }

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    err = snd_pcm_hw_params_set_access(pcm, hw,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    printf("DIAG set_access: %s\n", snd_strerror(err));
    err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    printf("DIAG set_format: %s\n", snd_strerror(err));
    err = snd_pcm_hw_params_set_channels(pcm, hw, chans);
    printf("DIAG set_channels(%u): %s\n", chans, snd_strerror(err));
    err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir);
    printf("DIAG set_rate(->%u): %s\n", rate, snd_strerror(err));
    err = snd_pcm_hw_params(pcm, hw);
    printf("DIAG hw_params: %s state=%s\n", snd_strerror(err), st(pcm));
    if (err < 0) {
        return 1;
    }

    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    err = snd_pcm_sw_params(pcm, sw);
    printf("DIAG sw_params: %s state=%s\n", snd_strerror(err), st(pcm));

    err = snd_pcm_prepare(pcm);
    printf("DIAG prepare: %s state=%s\n", snd_strerror(err), st(pcm));
    if (err < 0) {
        return 1;
    }

    err = snd_pcm_start(pcm);
    printf("DIAG start: %s state=%s\n", snd_strerror(err), st(pcm));

    r = snd_pcm_readi(pcm, buf, 2048);
    printf("DIAG readi: ret=%ld (%s) state=%s\n", (long)r,
           r < 0 ? snd_strerror((int)r) : "ok", st(pcm));

    snd_pcm_close(pcm);
    return 0;
}
