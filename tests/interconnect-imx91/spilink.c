/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * spilink - spidev oracle for the i.MX 91 SPI board-to-board link (run-spi.sh).
 * One instance is the SENDER: it clocks a payload out of its LPSPI master,
 * which the spi-link peripheral forwards over the socket to the peer. The other
 * is the RECEIVER: it clocks dummy bytes (so the spi-link shifts the peer's
 * bytes in on MISO) and collects the payload, then verifies it byte-exact. Idle
 * MISO reads back 0xff (the payload has no 0xff), so the receiver skips 0xff
 * while accumulating. Raw spidev ioctls, no libs, so it links -static for a
 * busybox initramfs. Emits SPILINK:PASS / SPILINK:FAIL:<why> / SPILINK:SENT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/spi/spidev.h>

static int xfer(int fd, const unsigned char *tx, unsigned char *rx, int len)
{
    struct spi_ioc_transfer tr;

    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = len;
    tr.bits_per_word = 8;
    tr.speed_hz = 1000000;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

static int setup(int fd)
{
    unsigned char mode = 0, bits = 8;
    unsigned int speed = 1000000;

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *role = argc > 1 ? argv[1] : "";
    const char *dev = argc > 2 ? argv[2] : "/dev/spidev0.0";
    const char *payload = argc > 3 ? argv[3] :
                          "IMX91-SPI-LINK-payload-0123456789";
    int plen = strlen(payload), fd, got = 0, i;
    unsigned char tx[64], rx[64], collected[128];
    time_t start;

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        printf("SPILINK:FAIL:open %s\n", dev);
        return 1;
    }
    if (setup(fd) < 0) {
        printf("SPILINK:FAIL:spidev setup\n");
        return 1;
    }

    if (!strcmp(role, "send")) {
        if (xfer(fd, (const unsigned char *)payload, rx, plen) < 0) {
            printf("SPILINK:FAIL:send xfer errno=%d\n", errno);
            return 1;
        }
        printf("SPILINK:SENT %d bytes [%s]\n", plen, payload);
        return 0;
    }

    /* receive: clock dummy bytes, collect non-idle (non-0xff) payload bytes */
    memset(tx, 0, sizeof(tx));
    start = time(NULL);
    while (got < plen && time(NULL) - start < 15) {
        int n = xfer(fd, tx, rx, sizeof(rx));

        if (n < 0) {
            printf("SPILINK:FAIL:recv xfer errno=%d\n", errno);
            return 1;
        }
        for (i = 0; i < (int)sizeof(rx) && got < plen; i++) {
            if (rx[i] != 0xff) {
                collected[got++] = rx[i];
            }
        }
        if (got < plen) {
            usleep(100000);
        }
    }
    collected[got < 128 ? got : 127] = 0;
    if (got == plen && !memcmp(collected, payload, plen)) {
        printf("SPILINK:PASS: %d bytes crossed the SPI link byte-exact\n",
               plen);
        return 0;
    }
    printf("SPILINK:FAIL: got %d/%d [%s]\n", got, plen, collected);
    return 1;
}
