/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * spistress - clock a large continuous SPI stream to prove the spi-link
 * peripheral never hangs the vCPU under socket back-pressure. Runs vs a peer
 * that does NOT drain (its spi-link rx FIFO fills, can_receive() goes to 0, the
 * chardev stops reading the socket, and the socket send buffer fills) - what
 * used to block spi-link's old qemu_chr_fe_write_all() in a TDR write. With the
 * non-blocking tx-FIFO + G_IO_OUT drain, every transfer returns and this prints
 * STRESS:DONE; with the old blocking write it would hang mid-stream.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/spidev0.0";
    int iters = argc > 2 ? atoi(argv[2]) : 300;
    unsigned char tx[2048], rx[2048];
    unsigned char mode = 0, bits = 8;
    unsigned int sp = 1000000;
    struct spi_ioc_transfer tr;
    int fd, i, fails = 0;

    memset(tx, 0x5A, sizeof(tx));
    fd = open(dev, O_RDWR);
    if (fd < 0) {
        printf("STRESS:FAIL:open %s\n", dev);
        return 1;
    }
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &sp);
    printf("STRESS:START %d x 2KB (=%d KB) vs a non-draining peer\n",
           iters, iters * 2);
    fflush(stdout);
    for (i = 0; i < iters; i++) {
        memset(&tr, 0, sizeof(tr));
        tr.tx_buf = (unsigned long)tx;
        tr.rx_buf = (unsigned long)rx;
        tr.len = sizeof(tx);
        tr.bits_per_word = 8;
        tr.speed_hz = 1000000;
        if (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
            fails++;
        }
        if (i % 50 == 0) {
            printf("STRESS:progress %d\n", i);
            fflush(stdout);
        }
    }
    printf("STRESS:DONE %d transfers, %d xfer-errs - NO vCPU HANG\n",
           iters, fails);
    return 0;
}
