NXP i.MX 91 11x11 Evaluation Kit (``imx91-11x11-evk``)
======================================================

The ``imx91-11x11-evk`` machine models the NXP i.MX 91 11x11 LPDDR4
Evaluation Kit. The i.MX 91 is a cost-reduced member of the i.MX 9 family:
a **single Cortex-A55** with no Cortex-M33 real-time core, no Ethos-U
microNPU, no PXP 2D engine and a reduced multimedia set, but otherwise
sharing the i.MX 93's peripheral IP. Like the i.MX 93 (and unlike the
i.MX 95) it has no System Manager, so Linux programs the clock (CCM),
analog PLLs (ANATOP), reset and power-domain blocks directly; those are
modelled functionally rather than served by firmware over SCMI.

Supported devices
-----------------

The ``imx91-11x11-evk`` machine implements the following devices:

 * 1 Cortex-A55 application core
 * Generic Interrupt Controller (GICv3)
 * LPUART serial controllers (LPUART1 is the Linux console)
 * CCM clock controller and ANATOP PLLs
 * MEDIAMIX block control / GPR and SRC power-domain slice
 * LPI2C controllers with the board PMIC (PCA9451A) and the PCAL6524 /
   ADP5585 I/O expanders
 * LPSPI controllers (each exposes a named SSI bus for an attached slave)
 * Silvaco I3C controller (bridges to legacy I2C devices)
 * GPIO controllers
 * ELE (EdgeLock Enclave) messaging-unit responder
 * MU1 and MU2 Messaging Units (standalone A55-side mailboxes)
 * eDMA1 and eDMA2 controllers
 * uSDHC (SD/MMC)
 * FEC and eQOS (dwmac4) Ethernet
 * ChipIdea USB host controllers
 * FlexSPI controller with NOR and SPI-NAND flash
 * i.MX 9 DDR controller and DDR performance-monitor (PMU)
 * Display: LCDIFv3 controller driving a parallel-RGB (DPI) panel
 * Audio: SAI (I2S), MICFIL (PDM) and XCVR (SPDIF) front-ends, the MQS, and
   a WM8962 codec
 * Camera: MT9M114 sensor -> parallel-CSI -> ISI V4L2 capture pipeline
 * FlexIO (usable as an I2C master)
 * FlexCAN controllers on QEMU's CAN bus
 * virtio-mmio transports (for virtio-keyboard / -tablet input)

Other peripherals are instantiated as unimplemented-device stubs that log
accesses. A catch-all background region (below the named stubs) lets the
machine boot the BSP's device-tree variants - including hand-edited ones
that poke an unmodelled address - without taking a data abort.

Boot options
------------

The machine boots a Linux kernel image with ``-kernel`` plus a device
tree (``-dtb``) and a root filesystem; these are built from the NXP i.MX
BSP and are not shipped with QEMU. The base device tree has no display
panel; the ``...-tianma-wvga-panel`` device tree selects the parallel-RGB
panel path so the LCDIFv3 scans out a framebuffer.

.. code-block:: bash

  $ qemu-system-aarch64 -M imx91-11x11-evk -m 4G -display none \
      -kernel Image -dtb imx91-11x11-evk.dtb -initrd rootfs.cpio.gz \
      -append "earlycon=lpuart32,mmio32,0x44380010 \
               console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
      -serial mon:stdio -serial null

The machine is single-core, so ``-smp 1`` (the default) is the only
accepted topology - no ``-smp`` flag is needed. The earlycon address is
the LPUART base plus the driver's ``0x10`` register offset
(``0x44380010``), which earlycon, unlike the regular driver, does not
apply itself.

For networking, give the two board NICs a host backend with
``-nic user -nic user`` (this populates QEMU's ``nd_table`` so the FEC and
eQOS each get a peer).

CAN bus
-------

Attach a CAN bus to the FlexCAN controllers with, for example::

  -object can-bus,id=cb -machine canbus0=cb,canbus1=cb

Input
-----

A ``virtio-keyboard-device`` (and optionally ``virtio-tablet-device``)
binds to the modelled virtio-mmio transports, delivering input to the
framebuffer console when run with a graphical display backend.
