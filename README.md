# QEMU for the NXP i.MX 93

The **first QEMU model of the NXP i.MX 93** application processor (dual
Cortex‑A55). It boots a real NXP BSP Linux all the way to an interactive
userspace shell — with networking, storage, GPIO/PMIC, the EdgeLock Enclave
mailbox, and a complete **LCDIFv3 → MIPI‑DSI → ADV7535 → HDMI** display
pipeline you can log into and type on.

This is a fork of upstream [QEMU](https://www.qemu.org/); all the i.MX 93 work
lives on the `imx93-dev` branch. The base for this branch is upstream commit
`edcc429e9e`. Licensed GPL‑2.0‑or‑later, same as QEMU.

> Status: **bring‑up / actively developed.** Not yet submitted upstream. The
> machine type is `imx93-11x11-evk` (the NXP 11×11 EVK, LPDDR4X).

![i.MX93 booting Linux on the emulated HDMI display — dual‑A55 SMP Tux logos](docs/images/hdmi-boot-logo.png)

*Real NXP BSP Linux scanned out at 1920×1080 by the emulated LCDIFv3, over the
DSI → ADV7535 → HDMI chain, on the stock EVK device tree. Two Tux logos = two
Cortex‑A55 cores.*

---

## What works

- **SMP boot** of both Cortex‑A55 cores to userspace (NXP BSP, Linux 6.12.49)
- **Console** on LPUART (`ttyLP0`), clean **PSCI power‑off**
- **Networking** — both NICs live with DHCP: FEC (`eth0`) and eQOS/dwmac4 (`eth1`)
- **Storage** — SD/MMC via uSDHC from `-drive if=sd` (mount real ext4 images)
- **Clocks / power** — CCM clock roots+gates, ANATOP PLLs, MEDIAMIX power domain
- **I²C** — LPI2C master + PMIC (PCA9451A) and PCAL6524 GPIO expander
- **GPIO** controllers, **ELE** (EdgeLock Enclave) s4 MU + responder
- **eDMA3** (fsl‑edma) — real TCD execution (used by the LPI2C EDID read)
- **Display** — LCDIFv3 controller with framebuffer scanout, dw‑mipi‑dsi host,
  ADV7535 HDMI bridge with generated EDID; the Linux DRM stack sets a mode and
  brings up `/dev/fb0`
- **Input** — virtio‑mmio + virtio‑keyboard/tablet, so you can **type in the
  QEMU window** and it lands on the HDMI console

![Interactive login + uname on the emulated HDMI console](docs/images/hdmi-login.png)

*Logging in as `root` and running `uname -a` — typed on the keyboard, rendered
on the emulated display.*

---

## Building

Out‑of‑tree build, aarch64 system target only:

```sh
mkdir build-imx93 && cd build-imx93
../configure --target-list=aarch64-softmmu
ninja qemu-system-aarch64
```

## Running

You need NXP i.MX 93 BSP artifacts: a kernel `Image`, the
`imx93-11x11-evk.dtb`, and a root filesystem. These are built from the
[NXP i.MX Yocto BSP](https://github.com/nxp-imx/meta-imx) (`MACHINE=imx93evk`);
this repo does not ship them.

Boot to a serial console (no rootfs needed to watch driver bring‑up):

```sh
qemu-system-aarch64 -M imx93-11x11-evk -m 4G -display none \
  -kernel Image -dtb imx93-11x11-evk.dtb -initrd rootfs.cpio.gz \
  -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -serial mon:stdio -serial null
```

Helper scripts under `tests/`:

- `tests/boot-imx93/run.sh` — boot to the serial console (override
  `KERNEL=`, `DTB=`, `INITRD=` as needed).
- `tests/login-imx93/run.sh` — **interactive login on the emulated HDMI
  display.** Opens a GTK window; click in and type `root` (the BSP image's
  root has no password). A virtio‑keyboard feeds the framebuffer console, or
  use the serial console in the launching terminal.

Attach an SD card with `-drive if=sd,file=disk.img,format=raw` (lands on
uSDHC1 → `mmcblk0`). Add a second NIC / test eQOS with
`-nic user` and `ip=:::::eth1:dhcp`.

---

## Device models added for the i.MX 93

| Area | Type / file |
| --- | --- |
| SoC / board | `hw/arm/fsl-imx93.c`, `hw/arm/imx93-evk.c` |
| LPUART console | `hw/char/imx_lpuart.c` |
| CCM clocks, ANATOP PLLs | `hw/misc/imx93_ccm.c`, `hw/misc/imx93_anatop.c` |
| PXP, ELE MU | `hw/misc/imx93_pxp.c`, `hw/misc/imx93_ele.c` |
| LPI2C master | `hw/i2c/imx_lpi2c.c` |
| GPIO | `hw/gpio/imx93_gpio.c` |
| eQOS (dwmac4) NIC | `hw/net/imx93_dwmac.c` |
| eDMA3 | `hw/dma/imx93_edma.c` |
| LCDIFv3 display | `hw/display/imx93_lcdif.c` |
| MIPI‑DSI host | `hw/display/imx93_dsi.c` |
| ADV7535 HDMI bridge | `hw/display/adv7535.c` |
| MEDIAMIX blk‑ctrl / power slice | `hw/misc/imx93_media_blk.c` |

The FEC ethernet reuses the existing `hw/net/imx_fec.c`; uSDHC reuses
`hw/sd/sdhci.c` (with the i.MX `SDCLK_AUTO_GATE` quirk).

---

## A note on the i.MX 93 vs. i.MX 95

Unlike the i.MX 95, the **i.MX 93 has no System Manager** — Linux programs the
CCM / ANATOP / SRC / power domains directly rather than going through an M33
firmware and SCMI. So those blocks are modeled functionally here, not stubbed
behind a System Control interface.

The NXP BSP also uses its **downstream `drm/imx` drivers** (`DRM_IMX_LCDIFV3`,
`dw-mipi-dsi`, `adv7511`), not the mainline `mxsfb`/`imx` ones — worth knowing
when comparing register behavior against upstream Linux.

---

## Credits

Built by Kyle Fox. Device models were developed with assistance from Claude
(Anthropic); individual commits carry co‑authorship trailers. Based on
upstream QEMU — see `LICENSE` / `README.rst` for QEMU's own authorship and
licensing.
