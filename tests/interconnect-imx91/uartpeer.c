/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * uartpeer - the i.MX 91's in-guest peer for the mixed-SoC UART board-to-board
 * lab (uart-link-imx-mcx). It is the guest-side equivalent of the MCX's
 * tests/mcxn-uart-link/uart_peer.py, run over the 91's LPUART2 (/dev/ttyLP1)
 * instead of a host socket: send ONE "GO" byte to release the peer firmware's
 * transmit, then ECHO every payload byte back so the peer can verify its own
 * i*7+3 pattern round-trips. The MCX M33 (uartlink.elf) generates + checks the
 * pattern; the peer here is a dumb GO + echo, so it works against any partner
 * (MCX today, another 91/93/95 tomorrow). Emits UARTPEER: markers.
 *
 * Raw termios, no libs, so it links -static for a busybox initramfs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/ttyLP1";
    int want = argc > 2 ? atoi(argv[2]) : 32;    /* bytes to echo */
    int fd, echoed = 0;
    struct termios t;
    unsigned char buf[64];

    fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        printf("UARTPEER:FAIL:open %s (%s)\n", dev, strerror(errno));
        return 1;
    }
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cc[VMIN] = 1;              /* block for >=1 byte ... */
        t.c_cc[VTIME] = 50;            /* ... or a 5s inter-byte gap */
        tcsetattr(fd, TCSANOW, &t);
    }

    /* GO byte: releases the peer firmware's transmit (it consumes one). */
    if (write(fd, "G", 1) != 1) {
        printf("UARTPEER:FAIL:write GO (%s)\n", strerror(errno));
        return 1;
    }
    printf("UARTPEER:GO-SENT\n");
    fflush(stdout);

    /* Echo the payload back byte-exact until we've mirrored `want` bytes. */
    while (echoed < want) {
        int n = read(fd, buf, sizeof(buf));

        if (n <= 0) {
            printf("UARTPEER:FAIL:recv (echoed %d/%d, %s)\n",
                   echoed, want, n < 0 ? strerror(errno) : "eof/timeout");
            return 1;
        }
        if (write(fd, buf, n) != n) {
            printf("UARTPEER:FAIL:echo (%s)\n", strerror(errno));
            return 1;
        }
        echoed += n;
    }
    printf("UARTPEER:PASS echoed %d bytes byte-exact\n", echoed);
    return 0;
}
