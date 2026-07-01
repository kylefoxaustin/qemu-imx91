/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * usbbulk - find the MCX vendor gadget (1fc9:0094) via usbfs and run a bulk
 * echo test against it. Raw USBDEVFS ioctls, no libusb, so it links -static
 * for a busybox initramfs. Used by run-usb.sh to prove the full USB data path
 * over the live i.MX 91 <-> MCX usbredir link (guest app -> ci_hdrc/EHCI ->
 * usb-redir -> socket -> MCX firmware EP1 echo -> back).
 *
 * Emits BULK:PASS / BULK:FAIL:<why> markers on stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

static int rdint(const char *dir, const char *f)
{
    char p[512];
    int v = -1;
    FILE *fp;

    snprintf(p, sizeof(p), "%s/%s", dir, f);
    fp = fopen(p, "r");
    if (fp) {
        if (fscanf(fp, "%x", &v) != 1) {
            v = -1;
        }
        fclose(fp);
    }
    return v;
}

int main(void)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    char devnode[64] = "";
    int busnum = -1, devnum = -1, iface = 0, fd, n, i;
    unsigned char out[64], in[64];
    struct usbdevfs_bulktransfer bt;

    if (!d) {
        printf("BULK:FAIL:no /sys/bus/usb\n");
        return 1;
    }
    while ((e = readdir(d))) {
        char dir[300];

        snprintf(dir, sizeof(dir), "/sys/bus/usb/devices/%s", e->d_name);
        if (rdint(dir, "idVendor") == 0x1fc9 &&
            rdint(dir, "idProduct") == 0x0094) {
            busnum = rdint(dir, "busnum");
            devnum = rdint(dir, "devnum");
            break;
        }
    }
    closedir(d);
    if (busnum < 0) {
        printf("BULK:FAIL:gadget 1fc9:0094 not found\n");
        return 1;
    }
    snprintf(devnode, sizeof(devnode), "/dev/bus/usb/%03d/%03d",
             busnum, devnum);
    printf("BULK:FOUND 1fc9:0094 at bus %d dev %d (%s)\n",
           busnum, devnum, devnode);

    fd = open(devnode, O_RDWR);
    if (fd < 0) {
        printf("BULK:FAIL:open %s\n", devnode);
        return 1;
    }
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        printf("BULK:FAIL:claim iface\n");
        return 1;
    }

    for (i = 0; i < 64; i++) {
        out[i] = (unsigned char)(i * 7 + 3);
    }
    bt.ep = 0x01;                       /* EP1 OUT (bulk) */
    bt.len = 64;
    bt.timeout = 5000;
    bt.data = out;
    n = ioctl(fd, USBDEVFS_BULK, &bt);
    printf("BULK: EP1 OUT wrote %d bytes\n", n);

    memset(in, 0, sizeof(in));
    bt.ep = 0x81;                       /* EP1 IN (bulk) */
    bt.data = in;
    n = ioctl(fd, USBDEVFS_BULK, &bt);
    printf("BULK: EP1 IN read %d bytes\n", n);

    if (n == 64 && !memcmp(in, out, 64)) {
        printf("BULK:PASS: 64 bytes echoed byte-for-byte over the live link\n");
        return 0;
    }
    printf("BULK:FAIL: echo mismatch (n=%d)\n", n);
    return 1;
}
