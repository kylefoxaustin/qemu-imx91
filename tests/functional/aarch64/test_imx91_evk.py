#!/usr/bin/env python3
#
# Functional test that boots a Linux kernel on the i.MX 91 11x11 EVK and
# checks it reaches userspace.
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import LinuxKernelTest, Asset


class Imx91EvkMachine(LinuxKernelTest):
    """
    Boot stock NXP BSP Linux on the imx91-11x11-evk machine to userspace.

    The i.MX 91 is a single Cortex-A55 (no Cortex-M33) member of the i.MX 9
    family, so the machine is -smp 1 and there is no co-processor firmware to
    load: a plain kernel + device tree + initramfs boot exercises the full
    Linux bring-up path (the single A55, GICv3, the LPUART console, the CCM
    clock tree and ANATOP PLLs, eDMA, GPIO, ...). Reaching the userspace
    marker confirms the machine boots a real BSP kernel end to end.
    """

    ASSET_KERNEL = Asset(
        ('https://github.com/kylefoxaustin/qemu-imx91/releases/download/'
         'imx91-v1.0/Image-imx91-evk-6.12.49.bin'),
        '037498d2ba56f330de83ebf7eb903a052ac8dd6e52d0b4d026e0ff62e777c6e3')

    ASSET_DTB = Asset(
        ('https://github.com/kylefoxaustin/qemu-imx91/releases/download/'
         'imx91-v1.0/imx91-11x11-evk.dtb'),
        '16397128d5c08e170f3e282d50793ffd73739cca7967ed4ae4c8ebc39add28a7')

    ASSET_INITRD = Asset(
        ('https://github.com/kylefoxaustin/qemu-imx91/releases/download/'
         'imx91-v1.0/busybox-imx91.cpio.gz'),
        '357e88378f1765feaa59c3eb4bdcba078d32eafb5b8012eaa6e16d0f53c6c972')

    def test_aarch64_imx91_evk(self):
        self.require_accelerator('tcg')
        self.set_machine('imx91-11x11-evk')

        kernel = self.ASSET_KERNEL.fetch()
        dtb = self.ASSET_DTB.fetch()
        initrd = self.ASSET_INITRD.fetch()

        # LPUART1 (ttyLP0, console index 0) is the Linux console.
        self.vm.set_console(console_index=0)
        # The machine is single-core (one Cortex-A55), so the default of
        # -smp 1 is the only accepted value - do not pass -smp. earlycon's
        # address is the LPUART base + the 0x10 register offset (the regular
        # driver applies that offset from the DT base, earlycon does not).
        # cpuidle.off=1 keeps the A55 on plain WFI (deep PSCI idle deferred).
        self.vm.add_args(
            '-m', '2G',
            '-kernel', kernel,
            '-dtb', dtb,
            '-initrd', initrd,
            '-append',
            'earlycon=lpuart32,mmio32,0x44380010 '
            'console=ttyLP0,115200 cpuidle.off=1 rdinit=/init',
        )
        self.vm.launch()
        # PID 1 reached userspace: printed by the /init in the busybox
        # initramfs (built by tests/busybox-imx91/build.sh).
        self.wait_for_console_pattern(
            'IMX91 FUNCTIONAL TEST: userspace reached')


if __name__ == '__main__':
    LinuxKernelTest.main()
