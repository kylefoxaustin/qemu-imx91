/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ttyecho - open a /dev/ttyACM CDC-ACM serial port raw, write a payload, read
 * the echo back, and verify it byte-exact. Used by run-usb-cdc.sh to prove the
 * full USB-CDC serial round-trip over the live i.MX 91 <-> MCX usbredir link
 * (guest cdc_acm write -> ci_hdrc/EHCI -> usb-redir -> socket -> MCX gadget EP1
 * echo -> back). Static, no libusb, so it links for a busybox initramfs.
 *
 * Emits TTYACM:PASS / TTYACM:FAIL:<why> markers on stdout.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/ttyACM0";
    const char *payload = "IMX91-CDC-ttyACM-roundtrip-0123456789";
    int plen = strlen(payload), fd, got = 0, r, sent = 0, retries = 0;
    struct termios t;
    fd_set rfds;
    struct timeval tv;
    char buf[128];

    fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        printf("TTYACM:FAIL:open errno=%d\n", errno);
        return 1;
    }
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        tcsetattr(fd, TCSANOW, &t);
    }
    printf("TTYACM:OPEN ok (%s)\n", dev);

    /*
     * First-write timing race: cdc_acm's first bulk-OUT right after bind can
     * EIO before the freshly-configured CDC-data (2nd-iface) endpoint is
     * ready. A brief settle + retry-on-EIO masks it, like a real app that
     * just opened a modem. (Cross-confirmed on the i.MX 93.)
     */
    usleep(200000);                     /* 200 ms settle after cdc_acm bind */
    while (sent < plen) {
        r = write(fd, payload + sent, plen - sent);
        if (r < 0 && errno == EIO && retries < 40) {
            retries++;
            usleep(50000);              /* 50 ms, ride out the write race */
            continue;
        }
        if (r < 0) {
            printf("TTYACM:FAIL:write errno=%d after %d retries\n",
                   errno, retries);
            return 1;
        }
        if (r == 0) {
            break;
        }
        sent += r;
    }
    printf("TTYACM: sent %d/%d bytes (first-write retries=%d)\n",
           sent, plen, retries);
    tcdrain(fd);

    while (got < plen) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 6;
        tv.tv_usec = 0;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            break;
        }
        r = read(fd, buf + got, plen - got);
        if (r <= 0) {
            break;
        }
        got += r;
    }
    buf[got < 128 ? got : 127] = 0;
    if (got == plen && !memcmp(buf, payload, plen)) {
        printf("TTYACM:PASS: %d bytes round-tripped over USB-CDC serial\n",
               plen);
        return 0;
    }
    printf("TTYACM:FAIL: sent %d, got %d/%d [%s]\n", sent, got, plen, buf);
    return 1;
}
