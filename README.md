# qemu-imx91

A QEMU machine type for the NXP **i.MX 91** SoC, targeting the **11×11 EVK**
(LPDDR4) variant.

> **This is a fork of QEMU mainline**, derived from the
> [qemu-imx93](https://github.com/kylefoxaustin/qemu-imx93) port. The i.MX 91
> work lives on the `imx91-dev` branch; the bulk of the history is the i.MX 93
> port and, beneath it, upstream QEMU. The upstream QEMU README is preserved at
> [`README.rst`](README.rst) — this file describes the i.MX 91-specific work.

qemu-imx91 is the **first QEMU model of the i.MX 91**. The i.MX 91 is a
**subset of the i.MX 93** (NXP AN14012 / AN14561): a **single Cortex-A55** (no
second A55, no Cortex-M33), **no Ethos-U65 NPU**, **no PXP 2D engine**, and
**no MIPI-DSI / MIPI-CSI / LVDS** — the display is **LCDIF parallel RGB** only,
and the camera is the parallel path. So this machine is the i.MX 93 port with
those blocks subtracted and the SoC retargeted to the 91's single-core topology.

It boots stock **NXP BSP Linux to userspace** on the single Cortex-A55 and
brings up the EVK's core: **networking** (FEC + ENET_QoS, both with DHCP),
**SD/eMMC storage** (ext4 mount, read/write), **GPIO/PMIC**, the **EdgeLock
Enclave** mailbox, **eDMA3/4**, **I²C** (with the WM8962 codec + PMIC +
expanders), and an **LCDIF parallel-RGB display** that scans a framebuffer out
of guest DRAM to a fixed panel. Intended use cases are BSP development,
peripheral-driver development, and CI for the above. It is not cycle-accurate.

Unlike the i.MX 95, the **i.MX 91 has no System Manager** — Linux programs the
CCM / ANATOP / SRC / power domains directly, so those blocks are modelled
functionally rather than served by firmware over SCMI. Structural conventions
follow the upstream i.MX 8MP code (`hw/arm/fsl-imx8mp.{c,h}`) and the i.MX 93
port; the long-term aim is to be upstream-mergeable into QEMU mainline.

**Maintainer:** Kyle Fox ([@kylefoxaustin](https://github.com/kylefoxaustin))

![i.MX 91 LCDIF parallel-RGB display scanout — color bars on the emulated panel](docs/images/imx91-display-scanout.png)

*Color bars written to `/dev/fb0` and scanned out at 800×480 by the emulated
LCDIFv3 over the parallel-RGB path on the stock `imx91-11x11-evk-tianma-wvga-panel`
device tree — captured via QMP screendump (colors byte-correct).*

## Quickstart

This fork **builds and runs as-is** — a plain clone lands on `imx91-dev`.

**1. Clone and build** — a standard QEMU build (see [Building](#building) for
host packages):

    git clone https://github.com/kylefoxaustin/qemu-imx91.git
    cd qemu-imx91
    mkdir build && cd build
    ../configure --target-list=aarch64-softmmu
    ninja qemu-system-aarch64
    ./qemu-system-aarch64 -M help | grep imx91     # -> imx91-11x11-evk

**2. The full stack — Linux to userspace.** Booting Linux needs a kernel
`Image`, the `imx91-11x11-evk.dtb`, and a root filesystem, all built from the
NXP BSP (not redistributable, so not in the repo — see
[Required artifacts](#required-artifacts)). The easy path is
`tests/boot-imx91/run.sh`. The equivalent manual invocation:

    ./build/qemu-system-aarch64 -M imx91-11x11-evk -m 4G -display none \
        -kernel <Image> -dtb <imx91-11x11-evk.dtb> -initrd <rootfs.cpio.gz> \
        -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        -serial mon:stdio -serial null

Three details are load-bearing:

- **The i.MX 91 is single-core**, so the machine is `-smp 1` (the default) — no
  `-smp` flag needed; the second A55 and the Cortex-M33 of the i.MX 93 are absent.
- **earlycon address `0x44380010`, not `0x44380000`.** The i.MX LPUART has
  VERID/PARAM/GLOBAL/PINCFG at 0x00–0x0C and BAUD at 0x10. Linux's regular
  driver applies the `reg_off = 0x10` offset to the DT base automatically;
  earlycon does not, so the cmdline address must be pre-offset.
- **`cpuidle.off=1`** is the conservative first-boot default (avoids the
  GICv3 WakeRequest gap shared by all GICv3 QEMU machines).

For networking, give the board NICs a host backend with **`-nic user -nic user`**
(this populates QEMU's `nd_table` so the FEC and ENET_QoS get a peer; a bare
`-netdev` leaves them unconnected). For the display, boot the
`imx91-11x11-evk-tianma-wvga-panel.dtb` variant (the base EVK dtb has no panel).

## Scope: what's modelled, what's deferred

QEMU SoC machines model controllers so their **Linux drivers bind and the
subsystem registers** — not so every byte reaches a host sink. This port holds
that bar, and goes past it where the data path is the point and is validatable
end to end:

- **Functional data paths (validated on the i.MX 91).** Networking (FEC +
  ENET_QoS, both DHCP), storage (uSDHC → ext4 mount + read/write/sync), the
  **LCDIF parallel-RGB display** (framebuffer scanout to `/dev/fb0`,
  screendump-verified), I²C (the WM8962 codec answers on LPI2C1), and the
  **EdgeLock Enclave** (the secure-enclave driver configures `hsm0`, which
  resolves the OCOTP fuse nvmem the ENET_QoS MAC depends on).
- **Ported from the i.MX 93 (binds / registers; not yet re-validated end to
  end on the 91).** SAI / MICFIL / WM8962 audio (ASoC cards register), the
  parallel camera path (MT9M114 → parallel-CSI → ISI / V4L2), FlexCAN, ChipIdea
  USB host, and the full set of 8 LPI2C / 8 LPSPI controllers
  (`-device`-attachable beyond the EVK). These device models are inherited from
  the i.MX 93 unchanged and the stock 91 dtb exercises them, but their
  end-to-end data paths have not yet been re-confirmed on the 91.
- **Removed (not on i.MX 91 silicon).** Second Cortex-A55, Cortex-M33 + RPMsg,
  Ethos-U65 NPU, PXP 2D engine, MIPI-DSI, MIPI-CSI, LVDS, and the ADV7535
  HDMI bridge — all present on the i.MX 93, none on the i.MX 91.

## What runs today

Stock **NXP Linux 6.12.49** boots to userspace (PID 1) on the single Cortex-A55,
on the **stock `imx91-11x11-evk` device tree** (with one board-side fix-up, see
below).

Each device is tagged **functional** (the host driver's data path ran end to end
on the 91) or **brings up** (ported from the 93; the driver binds / registers,
end-to-end not yet re-validated on the 91).

- **Single-A55 boot — functional.** One Cortex-A55 to userspace (`nproc=1`),
  serial console on `ttyLP0`.
- **Networking — functional.** Both NICs live with DHCP — FEC (`eth0`, reuses
  `hw/net/imx_fec.c`) and the from-scratch ENET_QoS/dwmac4 (`eth1`,
  `hw/net/imx93_dwmac.c`). Use `-nic user -nic user`.
- **Storage — functional.** SD/MMC via uSDHC from `-drive if=sd`; mounts a real
  ext4 image (`mmcblk0`) and reads/writes/syncs it.
- **I²C — functional.** LPI2C masters with the board's WM8962 codec (answers at
  0x1a on LPI2C1), the PCA9451A PMIC, and the PCAL6524 / ADP5585 expanders.
- **GPIO + ELE — functional.** GPIO controllers (the i.MX 91 uses the same
  `fsl,imx93-gpio` model — its imx7ulp two-aperture DT layout maps onto the same
  registers, no model change needed); **ELE** (EdgeLock Enclave) s4 MU +
  responder, with the secure-enclave driver bringing up `hsm0` and resolving the
  OCOTP MAC/SoC-UID fuse nvmem.
- **Clocks / power — functional.** CCM clock roots+gates, ANATOP fractional-N
  PLLs, the MEDIAMIX power domain (genpd) and block-control GPR.
- **eDMA3/4 — functional.** real TCD execution (drives the LPI2C transfers).
- **Display (LCDIF parallel RGB) — functional.** Booting the
  `…-tianma-wvga-panel` dtb, the imx-drm stack binds the LCDIFv3 CRTC and the
  parallel-display-format bridge (on the MEDIAMIX block-ctrl — no separate
  device), sets an 800×480 mode and creates `/dev/fb0`; the LCDIFv3 model DMAs
  the framebuffer out of guest DRAM and scans it out to the emulated display
  (a written pattern is captured byte-correct via QMP screendump).
- **Audio (SAI / MICFIL / WM8962) — brings up.** The ASoC stack registers the
  EVK ALSA cards (ported from the i.MX 93; PCM playback not yet re-validated on
  the 91).
- **Camera (parallel path) — brings up.** MT9M114 → parallel-CSI → ISI / V4L2
  media graph registers (ported from the 93; frame capture not yet re-validated
  on the 91).
- **CAN / USB / LPSPI — brings up.** FlexCAN, ChipIdea USB host, and the 8 LPSPI
  controllers are inherited from the 93 and bind on the 91; not yet re-exercised
  end to end here.

## Roadmap

| Feature | What | Target |
|---|---|---|
| Re-validate kept paths | Confirm audio playback, camera capture, CAN, USB, SPI end to end on the i.MX 91 | next |
| SoC-info | Replace the i.MX 93 SiP/OCOTP soc-id constants with the i.MX 91's real ATF values | next |
| Upstreaming | Submit the machine to qemu-devel alongside the i.MX 93 | later |

## Required artifacts

To boot Linux to userspace you need three artifacts, all built from the
[NXP i.MX Yocto BSP](https://github.com/nxp-imx/meta-imx) (`MACHINE=imx91evk`):

| Artifact | Where from |
| --- | --- |
| Kernel `Image`            | `linux-imx`, imx defconfig (the BSP kernel) |
| `imx91-11x11-evk.dtb` (or `…-tianma-wvga-panel.dtb` for display) | same kernel build |
| initramfs / rootfs        | any aarch64 rootfs with `/init` (e.g. the BSP `imx-image-core`) |

The `tests/*/run.sh` scripts take `KERNEL=`, `DTB=`, `INITRD=` (and `QEMU=`)
env vars and print exactly which to set if an artifact is missing.

## Known limitations

- **Secure-enclave tamper IRQ is injected.** The stock i.MX 91 EVK dtb's
  `fsl,imx93-se` node omits the tamper-IRQ `interrupts` property that the i.MX
  93's carries, so the `fsl-se` driver would fail `platform_get_irq()` and never
  register — cascading to the OCOTP nvmem provider and leaving the ENET_QoS MAC
  in deferred probe forever. The board's `modify_dtb` injects the two AONMIX
  secvio/tamper SPIs (34/35, the same lines the i.MX 93 dtb wires) so the
  secure-enclave driver registers and the fuse nvmem cells resolve.
- **`fsl-se … Failed to read tamper status` is benign.** The ELE registers fine
  (`ele-trng`, `hsm0` configured). The tamper read is an NXP SiP SMC normally
  serviced by TF-A; a `-kernel` boot has no secure firmware, so it errors.
- **SoC-info constants are inherited from the i.MX 93.** The SiP `GET_SOC_INFO`
  and OCOTP soc-id still report the 93's value; cosmetic (`/sys/devices/soc0`),
  to be corrected to the 91's real value.
- The base `imx91-11x11-evk.dtb` has no display panel (`display-subsystem: no
  available port`); use the `…-tianma-wvga-panel` variant for the display.
- Not cycle-accurate (TCG); no silicon timing is implied by any throughput.

## Architecture overview

- **1× Cortex-A55** (GICv3 / GIC-600, no ITS), the application core running
  Linux. DDR at `0x8000_0000`. No second A55 and no Cortex-M33 (both i.MX 93).
- Real device models for everything boot/net/storage/display/I²C exercise:
  LPUART, CCM, ANATOP, MEDIAMIX (blk-ctrl GPR + SRC power slice), ELE MU, MU1
  (now a standalone A55-side mailbox — no M33 peer), LPI2C + PMICs, GPIO, uSDHC,
  FEC + ENET_QoS, eDMA3/4, LCDIFv3, ISI, SAI/MICFIL/WM8962, ChipIdea USB,
  FlexCAN, and virtio-mmio. Everything else is a logging stub (the IOMUXC pinmux
  is a deliberate no-op).
- The NXP BSP uses its **downstream `drm/imx` drivers** (`DRM_IMX_LCDIFV3`,
  the parallel-display-format bridge), not the mainline ones.

All memory-map addresses and IRQ numbers come from the NXP BSP (`imx91.dtsi` /
the i.MX 91 Reference Manual), never guessed.

## Repository tour

| Path | Purpose |
| --- | --- |
| `hw/arm/fsl-imx91.c`, `include/hw/arm/fsl-imx91.h` | SoC realization: single A55, GIC, device wiring, memory map, virtio-mmio, logging stubs (derived from fsl-imx93 with the 93-only blocks removed) |
| `hw/arm/imx91-evk.c` | 11×11 EVK board file (SD attach, DTB virtio-mmio + secure-enclave-IRQ injection) |
| `hw/arm/Kconfig`, `hw/arm/meson.build` | `FSL_IMX91` / `FSL_IMX91_EVK` config + build wiring |
| (shared with the i.MX 93, unchanged) | `hw/char/imx_lpuart.c`, `hw/misc/imx93_{ccm,anatop,ele,media_blk}.c`, `hw/misc/imx_mu.c`, `hw/i2c/imx_lpi2c.c`, `hw/ssi/imx93_lpspi.c`, `hw/gpio/imx93_gpio.c`, `hw/net/{imx_fec.c,imx93_dwmac.c}`, `hw/dma/imx93_edma.c`, `hw/display/{imx93_lcdif,imx93_isi}.c`, `hw/audio/{imx93_sai,imx93_micfil,wm8962}.c`, `hw/net/can/flexcan.c`, `hw/sd` uSDHC, ChipIdea USB |
| `tests/boot-imx91/run.sh` | boot Linux to the serial console (Path-C probe pass) |
| `tests/functest-imx91/` | end-to-end smoke test: uSDHC r/w, I²C, both Ethernets (DHCP) |
| `tests/display-imx91/` | headless LCDIF scanout verify (write `/dev/fb0`, QMP screendump, assert non-black) |

## Building

    mkdir -p build && cd build
    ../configure --target-list=aarch64-softmmu
    ninja qemu-system-aarch64

**Host packages (Ubuntu 22.04+):**

    sudo apt install -y \
        meson ninja-build python3 python3-venv python3-tomli \
        gcc libc6-dev pkg-config libglib2.0-dev libpixman-1-dev \
        libgtk-3-dev binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu

## Smoke tests

    # machine registers
    ./build/qemu-system-aarch64 -M help | grep imx91

    # Linux to userspace (needs the BSP artifacts)
    KERNEL=<Image> DTB=<dtb> INITRD=<rootfs> tests/boot-imx91/run.sh

## Methodology & contributing

The i.MX 91 is built by **subtracting an i.MX 93** rather than green-field
modelling: clone the i.MX 93 port, remove the blocks the 91 lacks, retarget to
`imx91-11x11-evk`, then Path-C the gaps (boot real Linux, read the exact abort /
deferred-probe stall, map or fix that, repeat) and re-validate by booting to
userspace after each change. Every address and IRQ comes from the NXP DTS / RM,
never guessed; data paths are validated end to end ("it bound" ≠ "data flows").
A good example: the ENET_QoS MAC was stuck in deferred probe; tracing it
revealed the stock 91 dtb omits the secure-enclave tamper IRQ, which the board
now injects.

## Milestone history

- **Bootstrap** — cloned the i.MX 93 port to `imx91-dev`; created the
  `fsl-imx91` SoC + `imx91-11x11-evk` machine as a renamed copy (shared i.MX 9x
  device models reused unchanged).
- **Chop-down** — removed the second A55, the Cortex-M33 (+ RPMsg / MU peer),
  the Ethos-U65 NPU, the PXP 2D engine, and the MIPI-DSI/CSI + LVDS + ADV7535
  display chain; retargeted to single-core. Validated boot-to-userspace after
  each stage; checkpatch clean throughout.
- **Functional bring-up** — uSDHC storage (r/w/sync), both Ethernets (DHCP),
  I²C (WM8962), the EdgeLock secure enclave (the dtb tamper-IRQ fix-up), and the
  LCDIF parallel-RGB display scanout, all validated end to end.

## License & credits

GPL-2.0-or-later, same as QEMU. Derived from the qemu-imx93 fork of upstream
QEMU; see [`README.rst`](README.rst) and `LICENSE` for QEMU's own authorship and
licensing.

---

**Created and maintained by Kyle Fox — [@kylefoxaustin](https://github.com/kylefoxaustin).**
The first-ever QEMU port of the NXP i.MX 91.
