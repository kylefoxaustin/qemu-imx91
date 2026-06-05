NXP i.MX 93 11x11 Evaluation Kit (``imx93-11x11-evk``)
======================================================

The ``imx93-11x11-evk`` machine models the NXP i.MX 93 11x11 LPDDR4X
Evaluation Kit. The i.MX 93 is a dual Cortex-A55 applications processor.
Unlike the i.MX 95, it has no System Manager: Linux programs the clock
(CCM), analog PLLs (ANATOP), reset and power-domain blocks directly, so
those are modelled functionally rather than served by firmware over SCMI.

Supported devices
-----------------

The ``imx93-11x11-evk`` machine implements the following devices:

 * 2 Cortex-A55 application cores
 * Generic Interrupt Controller (GICv3)
 * LPUART serial controllers (LPUART1 is the Linux console)
 * CCM clock controller and ANATOP PLLs
 * MEDIAMIX block control / GPR and SRC power-domain slice
 * LPI2C controllers with the board PMIC (PCA9451A), the PCAL6524 and
   ADP5585 I/O expanders
 * GPIO controllers
 * ELE (EdgeLock Enclave) messaging-unit responder
 * eDMA v3 controller
 * uSDHC (SD/MMC)
 * FEC and eQOS (dwmac4) Ethernet
 * Display: LCDIFv3 controller with a MIPI-DSI host + ADV7535 HDMI bridge,
   and an LDB + LVDS-PHY path to a fixed LVDS panel
 * virtio-mmio transports (for virtio-keyboard / -tablet input)
 * FlexCAN controllers on QEMU's CAN bus

Other peripherals are instantiated as unimplemented-device stubs that log
accesses.

Boot options
------------

The machine boots a Linux kernel image with ``-kernel`` plus a device
tree (``-dtb``) and a root filesystem; these are built from the NXP i.MX
BSP and are not shipped with QEMU. The base device tree drives the HDMI
display path; the ``...-boe-wxga-lvds-panel`` device tree selects the
LVDS panel path instead.

.. code-block:: bash

  $ qemu-system-aarch64 -M imx93-11x11-evk -m 4G -display none \
      -kernel Image -dtb imx93-11x11-evk.dtb -initrd rootfs.cpio.gz \
      -append "earlycon=lpuart32,mmio32,0x44380010 \
               console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
      -serial mon:stdio -serial null

The earlycon address is the LPUART base plus the driver's ``0x10``
register offset (``0x44380010``), which earlycon, unlike the regular
driver, does not apply itself.

CAN bus
-------

Attach a CAN bus to the FlexCAN controllers with, for example::

  -object can-bus,id=cb -machine canbus0=cb,canbus1=cb

Input
-----

A ``virtio-keyboard-device`` (and optionally ``virtio-tablet-device``)
binds to the modelled virtio-mmio transports, delivering keyboard input
to the framebuffer console when run with a graphical display backend.
