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
Enclave** mailbox, **eDMA1/2**, **I²C** (with the WM8962 codec + PMIC +
expanders), and an **LCDIF parallel-RGB display** that scans a framebuffer out
of guest DRAM to a fixed panel. It also boots a **vanilla mainline kernel** with
the mainline `imx91-11x11-evk` device tree (i.MX 91 support landed in v6.18) — a
fully-OSS boot that doubles as the upstream CI functional test. Intended use
cases are BSP development, peripheral-driver development, and CI for the above.
It is not cycle-accurate.

Unlike the i.MX 95, the **i.MX 91 has no System Manager** — Linux programs the
CCM / ANATOP / SRC / power domains directly, so those blocks are modelled
functionally rather than served by firmware over SCMI. Structural conventions
follow the upstream i.MX 8MP code (`hw/arm/fsl-imx8mp.{c,h}`) and the i.MX 93
port; the long-term aim is to be upstream-mergeable into QEMU mainline.

**Maintainer:** Kyle Fox ([@kylefoxaustin](https://github.com/kylefoxaustin))

![i.MX 91 LCDIF parallel-RGB display scanout — the Linux boot logo on the emulated panel](docs/images/imx91-boot-logo.png)

*The Linux kernel boot logo — one Tux for the single Cortex-A55 — scanned out at
800×480 by the emulated LCDIFv3 over the parallel-RGB path on the stock
`imx91-11x11-evk-tianma-wvga-panel` device tree, captured via QMP screendump.*

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
  resolves the OCOTP fuse nvmem the ENET_QoS MAC depends on), **FlexCAN** (a
  frame round-trips through `can0` loopback), **ChipIdea USB host** (a
  `-device usb-storage` enumerates via `ci_hdrc` and its disk mounts as
  `/dev/sda`), and **command-line-attachable I²C** (a `-device tmp105,bus=lpi2c1`
  is enumerated and read from Linux, so the machine hosts peripherals beyond the
  EVK).
- **Runs non-stock device trees.** The machine boots the BSP's ~100 variant
  DTBs — the other boards (FRDM-IMX91, FRDM-IMX91S, 9x9 QSB) and per-peripheral
  variants (mqs, i3c, 8mic, lpuart, flexspi-nand, panels, usbwifi) — to
  userspace; a catch-all background region keeps even a hand-edited DTB poking
  an unmodeled address from data-aborting. The MQS card plays (SAI1→eDMA1), the
  8-mic MICFIL card captures 8 channels, the i3c DTB's wm8962-on-I3C probes
  (Silvaco master bridging to legacy I2C) so its audio card registers, and
  `flexspi-flash=gd5f4gq4` runs the flexspi-nand DTB (the SPI-NAND enumerates +
  reads). Covered by `tests/dtb-matrix-imx91`.
- **Complete DT-referenced SoC surface.** Every peripheral any of those DTBs
  enables is now modelled; the last gap, the DDR controller + DDR PMU, has a
  register/perf-interface compat model so `imx9_ddr0` registers and `perf`
  opens its events — though the counters read 0, because QEMU can't measure
  real DDR bandwidth (DRAM is plain host memory, no controller in the path, no
  cache model). What remains unmodelled is only RM blocks no DTB brings up
  (LPTMR/LPIT/TRGMUX/GPC/CoreSight/boot-ROM/USB-PHY) and deliberate register
  stubs (IOMUXC/SRC/TRDC/block-controls).
- **Functional, validated via cross-compiled oracles.** SAI3/WM8962 audio
  playback (a square wave round-trips to a captured `.wav`) and the parallel
  camera path (5/5 real V4L2 frames off `/dev/video0`) — both ported from the
  i.MX 93 and re-confirmed on the 91 with the `tests/audio-imx91` /
  `tests/camera-imx91` harnesses (the `imx-image-core` rootfs lacks `aplay` /
  `v4l2-ctl`, so the tests cross-compile a tiny ALSA/V4L2 client).
- **SAI capture — functional, end to end.** Recording from the WM8962/SAI3 card
  returns real, non-silent, continuously varying samples to userspace: the SAI
  *receive* path synthesises a sawtooth into the RX FIFO (and on demand when the
  eDMA bursts ahead of the word-rate), and the eDMA drains RDR0 → memory the
  mirror of how it fills TDR0 for playback. Covered by a qtest and an ALSA
  capture oracle (`tests/audio-imx91/run-capture.sh`). Reaching this exposed a
  latent eDMA bug — minor loops only wrote back SADDR, so every device→memory
  (capture) transfer overwrote the same bytes; persisting DADDR fixed all
  capture directions at once.
- **MICFIL (PDM) capture — functional, end to end.** Recording from the MICFIL
  card returns real, non-silent S32 samples to userspace: the model synthesises
  a sawtooth into the data FIFO once the module is enabled (CTRL1.PDMIEN) and
  requests an eDMA drain of DATACH0 as it fills. Covered by a qtest and the ALSA
  capture oracle. Wiring this exposed a second eDMA bug — minor-loop offset
  (SMLOE/MLOFF) wasn't decoded, so the byte count read as ~1 GB and hung; the
  model now honours MLOFF (used by MICFIL to walk DATACH0..n then rewind).
- **Removed (not on i.MX 91 silicon).** Second Cortex-A55, Cortex-M33 + RPMsg,
  Ethos-U65 NPU, PXP 2D engine, MIPI-DSI, MIPI-CSI, LVDS, and the ADV7535
  HDMI bridge — all present on the i.MX 93, none on the i.MX 91.

## What runs today

Stock **NXP Linux 6.12.49** boots to userspace (PID 1) on the single Cortex-A55,
on the **stock `imx91-11x11-evk` device tree** (with one board-side fix-up, see
below).

Each device below is tagged **functional** (the host driver's data path runs end
to end — data actually moves) or **brings up** (the driver binds and the device
registers / enumerates — the registration bar, no working host data path yet).
Many devices are ported from the i.MX 93; where the end-to-end path has not been
independently re-validated on the 91, they are tagged **brings up**.

- **Single-A55 boot — functional.** One Cortex-A55 to userspace (`nproc=1`),
  serial console on `ttyLP0`. The SiP `GET_SOC_INFO` SMC reports the 91's own
  identity, so `/sys/devices/soc0` reads `soc_id = i.MX91`, `revision = 1.0`.
- **Vanilla mainline boot — functional.** Beyond the NXP BSP kernel, the machine
  boots a stock upstream kernel (v6.18-rc3) with the **mainline**
  `imx91-11x11-evk` device tree (i.MX 91 landed in mainline v6.18) to a BusyBox
  userspace — a fully-OSS, redistributable tuple with zero NXP bits. A stock
  mainline kernel + mainline dts booting the model is the upstream-fidelity
  signal; it is the basis for the kernel-free-CI functional test
  (`tests/functional/aarch64/test_imx91_evk.py`).
- **Networking — functional.** Both NICs live with DHCP — FEC (`eth0`, reuses
  `hw/net/imx_fec.c`) and the from-scratch ENET_QoS/dwmac4 (`eth1`,
  `hw/net/imx93_dwmac.c`). Use `-nic user -nic user`.
- **Storage — functional.** SD/MMC via uSDHC from `-drive if=sd`; mounts a real
  ext4 image (`mmcblk0`) and reads/writes/syncs it.
- **I²C — functional.** LPI2C masters with the board's WM8962 codec (answers at
  0x1a on LPI2C1), the PCA9451A PMIC, and the PCAL6524 / ADP5585 expanders.
  Command-line-attachable: `-device tmp105,bus=lpi2c1,address=0x49` is enumerated
  by `i2cdetect` and read by `i2cget` from Linux — the machine hosts peripherals
  the EVK never wired.
- **GPIO + ELE — functional.** GPIO controllers (the i.MX 91 uses the same
  `fsl,imx93-gpio` model — its imx7ulp two-aperture DT layout maps onto the same
  registers, no model change needed); **ELE** (EdgeLock Enclave) s4 MU +
  responder, with the secure-enclave driver bringing up `hsm0` and resolving the
  OCOTP MAC/SoC-UID fuse nvmem.
- **Clocks / power — functional.** CCM clock roots+gates, ANATOP fractional-N
  PLLs, the MEDIAMIX power domain (genpd) and block-control GPR.
- **eDMA1/2 — functional.** real TCD execution (drives the LPI2C transfers and
  the audio FIFO drains, with per-CH_MUX source-id request routing).
- **Display (LCDIF parallel RGB) — functional.** Booting the
  `…-tianma-wvga-panel` dtb, the imx-drm stack binds the LCDIFv3 CRTC and the
  parallel-display-format bridge (on the MEDIAMIX block-ctrl — no separate
  device), sets an 800×480 mode and creates `/dev/fb0`; the LCDIFv3 model DMAs
  the framebuffer out of guest DRAM and scans it out to the emulated display
  (a written pattern is captured byte-correct via QMP screendump).
- **USB host (ChipIdea OTG) — functional.** A `-device usb-storage,drive=…`
  enumerates through `ci_hdrc` (`new full-speed USB device … using ci_hdrc`) and
  attaches as a SCSI disk; its ext4 image mounts as `/dev/sda` and reads back
  byte-correct. `lsusb` lists it.
- **CAN (FlexCAN) — functional.** The `flexcan` driver brings `can0` up and a
  frame round-trips through controller loopback (`cansend` → `candump`). The EVK
  dtb enables one of the two FlexCAN controllers; inter-controller TX/RX over
  QEMU's CAN bus (`-object can-bus,id=cb -machine canbus0=cb,canbus1=cb`) is
  covered by a kernel-free qtest (plus a 1000-frame stress).
- **Audio playback (SAI3 + WM8962) — functional.** The ASoC stack registers the
  three EVK ALSA cards (SAI1 bt-sco, MICFIL, and the WM8962/SAI3 card via the
  modelled WM8962 codec on LPI2C1). A generated square wave plays on the
  WM8962/SAI3 card: the SAI3 TX FIFO drains the samples through eDMA, and with
  QEMU's wav audio backend the played PCM comes back in a real `.wav`
  (peak-checked square wave). See `tests/audio-imx91/run.sh`.
- **SPDIF playback (XCVR) — functional.** The `snd-soc-fsl-xcvr` driver brings
  up the `imxaudioxcvr` card; PCM plays out the XCVR's SPDIF-TX FIFO, draining
  through eDMA2 to the wav backend (peak-checked). Covered by a qtest and the
  ALSA playback oracle.
- **Concurrent multi-stream audio — functional.** eDMA requests route per
  CH_MUX source id, so streams sharing one eDMA instance run at once: SAI3
  playback + SAI3 capture + XCVR/SPDIF transmit (all on eDMA2) plus MICFIL
  capture (eDMA1) flow to completion simultaneously, where a shared dma-req line
  previously let only one eDMA2 stream make progress.
- **FlexIO-as-I2C — functional.** The FlexIO IP is on the 91 silicon (the stock
  EVK leaves its `flexio@425c0000` i2c-master disabled). Enabling it, the
  in-kernel `i2c-imx-flexio` driver registers an adapter and a
  `-device tmp105,bus=flexio1-i2c` enumerates and is read from Linux. The shift
  datapath runs on a virtual-clock timer with a defer-on-undrained-RX gate that
  closes a ~1/1000 IRQ-storm race (converged byte-identical with the i.MX 93).
- **Camera capture — functional.** Booting the `…-mt9m114` dtb, the parallel
  path runs end to end — MT9M114 → parallel-CSI → ISI → V4L2 — delivering real
  frames off `/dev/video0`. The ISI model DMAs a moving test pattern into the
  ping-pong buffers; a V4L2 client enabling the (default-disabled) sensor link
  and propagating the pad formats streams 5/5 byte-checked 1280×720 YUYV frames.
  See `tests/camera-imx91/run.sh`. Beyond the built-in pattern, the ISI can
  stream frames from a **host file/dir** supplied at launch
  (`-global driver=imx93.isi,property=frames,value=<path>`) — a format-agnostic
  byte source (one raw file, or a directory of `*.raw` looped) so a real
  image/video can drive the capture path.
- **LPSPI — functional (SSI slave round-trip).** Each of the 8 LPSPI masters
  exposes a named SSI bus, so an SPI peripheral attaches at runtime:
  `-device is25lp064,bus=lpspi1,drive=…`. A qtest reads the flash's JEDEC ID
  (0x9d 0x60 0x17) byte-exact through the controller (TDR → SSI transfer →
  RDR), and also covers the CR.MEN gate, TCF/FCF latch and RX-FIFO reset.

## Roadmap

The current release is **`imx91-v1.1`** — the complete, soak-validated i.MX 91
model (`v1.0`) plus upstream readiness: stock Linux boots to userspace, the
entire DT-referenced SoC surface is modelled, and every major data path
(networking, storage, display, audio play/capture/SPDIF, camera, CAN, USB,
I²C/SPI/I3C, FlexIO) is validated end to end. `v1.1` adds the kernel-free-CI
functional test booting a **vanilla mainline kernel** on fully-OSS assets, the
SAI rx-capture qtest fix, and an in-guest LCDIF display test in the soak. What
remains is upstream submission, not new capability.

| Feature | What | Target |
|---|---|---|
| Upstreaming | Submit the machine to qemu-devel as a follow-on to the i.MX 93 series — the shared device models land with the 93; this adds the 91 SoC + board, the three 91-only device models (DDR controller, SPI-NAND, Silvaco I3C), and the kernel-free qtests | next |
| Inert RM blocks | LPTMR / LPIT / TRGMUX, GPC regs, SRAM controller, CoreSight, boot ROM — no DTB enables them, so nothing exercises them | only if a use case demands |

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

**No NXP access? A fully-OSS boot works too.** A vanilla mainline kernel
(≥ v6.18, arm64 defconfig), the mainline `imx91-11x11-evk.dtb` built from that
tree, and the static-aarch64 BusyBox initramfs from `tests/busybox-imx91/`
boot the machine to a shell with **zero NXP bits** — all GPL, freely
redistributable. This is exactly the tuple the upstream functional test uses.

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
- The base `imx91-11x11-evk.dtb` has no display panel (`display-subsystem: no
  available port`); use the `…-tianma-wvga-panel` variant for the display.
- **LPSPI models one chip-select per bus.** Each LPSPI exposes a named SSI bus
  and a single attached slave round-trips (its CS is asserted by default). The
  model does not yet decode `TCR.PCS` to select among *multiple* slaves on one
  bus — fine for the usual one-slave-per-controller case, a gap only if a board
  muxes several SPI devices on a single LPSPI.
- Not cycle-accurate (TCG); no silicon timing is implied by any throughput.

## Architecture overview

- **1× Cortex-A55** (GICv3 / GIC-600, no ITS), the application core running
  Linux. DDR at `0x8000_0000`. No second A55 and no Cortex-M33 (both i.MX 93).
- Real device models for everything boot/net/storage/display/I²C exercise:
  LPUART, CCM, ANATOP, MEDIAMIX (blk-ctrl GPR + SRC power slice), ELE MU, MU1
  (now a standalone A55-side mailbox — no M33 peer), MU2, LPI2C + PMICs, GPIO,
  uSDHC, FEC + ENET_QoS, eDMA1/2, LCDIFv3, ISI, SAI/MICFIL/XCVR/WM8962, FlexSPI,
  Silvaco I3C, DDR controller + PMU, ChipIdea USB, FlexCAN, and virtio-mmio.
  Everything else is a logging stub (the IOMUXC pinmux is a deliberate no-op).
- The NXP BSP uses its **downstream `drm/imx` drivers** (`DRM_IMX_LCDIFV3`,
  the parallel-display-format bridge), not the mainline ones; the machine also
  boots a vanilla mainline kernel (which binds the upstream LPUART/CCM/eDMA/…
  drivers), so it tracks both driver stacks.

All memory-map addresses and IRQ numbers come from the NXP BSP (`imx91.dtsi` /
the i.MX 91 Reference Manual), never guessed.

## Repository tour

| Path | Purpose |
| --- | --- |
| `hw/arm/fsl-imx91.c`, `include/hw/arm/fsl-imx91.h` | SoC realization: single A55, GIC, device wiring, memory map, virtio-mmio, logging stubs (derived from fsl-imx93 with the 93-only blocks removed) |
| `hw/arm/imx91-evk.c` | 11×11 EVK board file (SD attach, DTB virtio-mmio + secure-enclave-IRQ injection) |
| `hw/arm/Kconfig`, `hw/arm/meson.build` | `FSL_IMX91` / `FSL_IMX91_EVK` config + build wiring |
| (shared with the i.MX 93, unchanged) | `hw/char/imx_lpuart.c`, `hw/misc/imx93_{ccm,anatop,ele,media_blk,flexio}.c`, `hw/misc/imx_mu.c`, `hw/i2c/imx_lpi2c.c`, `hw/ssi/imx93_lpspi.c`, `hw/gpio/imx93_gpio.c`, `hw/net/{imx_fec.c,imx93_dwmac.c}`, `hw/dma/imx93_edma.c`, `hw/display/{imx93_lcdif,imx93_isi}.c`, `hw/audio/{imx93_sai,imx93_micfil,imx93_xcvr,wm8962}.c`, `hw/net/can/flexcan.c`, `hw/sd` uSDHC, ChipIdea USB |
| `tests/boot-imx91/run.sh` | boot Linux to the serial console (Path-C probe pass) |
| `tests/functest-imx91/` | end-to-end smoke test: uSDHC r/w, I²C, both Ethernets (DHCP) |
| `tests/display-imx91/` | headless LCDIF scanout verify (write `/dev/fb0`, QMP screendump, assert non-black) |
| `tests/camera-imx91/` | V4L2 capture oracle (`v4l2_cap.c`): mt9m114 → CSI → ISI → real frames on `/dev/video0` |
| `tests/audio-imx91/` | SAI3/WM8962 PCM playback (`pcm_play.c`); `WAV=` captures the played square wave to a `.wav` |
| `tests/qtest/imx91-*-test.c` | kernel-free qtests on the imx91-11x11-evk machine: FlexCAN (MCR handshake + inter-controller TX/RX + 1000-frame stress), LPSPI (transfer engine + is25lp064 JEDEC round-trip), SAI (TX FIFO + RX-capture sawtooth), MICFIL (PDM capture), XCVR (SPDIF TX), LPI2C, ISI, FlexSPI (NOR + SPI-NAND read-id), FlexIO, DDRC, I3C |
| `tests/soak-imx91/` | long-run soak: a per-boot function battery over the variant-DTB matrix (net/storage/audio/camera/CAN/I²C/SPI/I3C/FlexIO + an in-guest LCDIF fb0 test), VmRSS leak tracking, and the kernel-free qtests every N cycles |
| `tests/functional/aarch64/test_imx91_evk.py` | upstream-style functional test: fetches a vanilla mainline kernel + mainline dtb + BusyBox initramfs (pinned `Asset` sha256, all OSS) and boots to a userspace marker — runs in QEMU's CI |
| `tests/busybox-imx91/build.sh` | builds the static-aarch64 BusyBox initramfs the functional test + OSS demo boot |

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
  I²C (WM8962 + command-line-attachable tmp105), the EdgeLock secure enclave
  (the dtb tamper-IRQ fix-up), the LCDIF parallel-RGB display scanout, FlexCAN
  (`can0` loopback round-trip), and ChipIdea USB host (`usb-storage` → `/dev/sda`
  mount), all validated end to end.
- **Deterministic CI** — kernel-free qtests on the imx91-11x11-evk machine for
  FlexCAN (incl. inter-controller TX/RX + a 1000-frame stress), LPI2C, ISI, SAI,
  FlexSPI and FlexIO; all green.
- **Audio + camera re-validated** — SAI3/WM8962 plays a square wave back to a
  captured `.wav`, and the MT9M114 → parallel-CSI → ISI path streams 5/5 real
  V4L2 frames off `/dev/video0`, both via cross-compiled ALSA/V4L2 oracles
  (the `imx-image-core` rootfs ships no `aplay`/`v4l2-ctl`).
- **Audio breadth (`imx91-v0.2` → `v1.0`)** — added SPDIF/XCVR playback, and
  per-CH_MUX eDMA request routing so SAI3 play+capture, SPDIF transmit (eDMA2)
  and MICFIL capture (eDMA1) run *concurrently*. The ISI gained a host
  frame-injection source so real images/video can drive the camera path.
- **FlexIO shift-race fix** — closed a ~1/1000 FlexIO-as-I2C IRQ-storm/hang by
  deferring the shift while the prior RX byte is undrained, validated against the
  real `i2c-imx-flexio` driver and a 36-hour soak; converged byte-identical with
  the i.MX 93.
- **Soak + hardening** — a full soak harness (`tests/soak-imx91`) runs a
  per-boot function battery across the variant-DTB matrix with VmRSS leak
  tracking and periodic kernel-free qtests; the shared device models also pass an
  AddressSanitizer + UBSan sweep clean.
- **`imx91-v1.0` — first complete release** — a 24-hour final soak (1423 boots,
  zero function failures, flat RSS) closed the gate: every modelled data path
  validated end to end, declared the first complete, soak-validated i.MX 91.
- **Upstream readiness (`v1.0` → `v1.1`)** — added the QEMU-CI functional test
  that boots a **vanilla mainline kernel** + the mainline `imx91-11x11-evk`
  device tree + a BusyBox initramfs (all OSS, no NXP — so it runs in CI rather
  than skipping on a restricted blob), with the i.MX 91 doc and MAINTAINERS
  entry. Also fixed the SAI rx-capture qtest for the shared RX model and added
  an in-guest LCDIF display test to the soak battery.

## License & credits

GPL-2.0-or-later, same as QEMU. Derived from the qemu-imx93 fork of upstream
QEMU; see [`README.rst`](README.rst) and `LICENSE` for QEMU's own authorship and
licensing.

---

**Created and maintained by Kyle Fox — [@kylefoxaustin](https://github.com/kylefoxaustin).**
The first-ever QEMU port of the NXP i.MX 91.
