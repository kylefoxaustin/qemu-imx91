# i.MX 91 QEMU — test-result matrix

The fleet's per-IP-block honest test record for the i.MX 91 model. A developer
brings their own correctly-built binary; **code that runs on real i.MX 91
silicon should run on this model and get the right answer, or fail honestly.**
This file is the model-owned source of truth: downstream tools (holobench's
board picker / capability gate) READ it — they do not guess a compatibility
matrix. Fidelity judgments live in the companion
[`fidelity-audit.md`](fidelity-audit.md).

**Fleet reporting rule (operator directive, 2026-06-28):** report *present* IP
with a real test result, and *absent* IP as **N/A — ABSENT**, never as a
"negative"/failing result. The i.MX 91 is a strict subset of the i.MX 93
(NXP AN14012/AN14561): single Cortex-A55, no Cortex-M33, no Ethos-U65 NPU, no
PXP, no MIPI-DSI/CSI/LVDS, no System Manager. A block the silicon doesn't have
cannot "fail" — it is correctly **N/A**.

## Two orthogonal axes (implements the fleet CI guardrail)

The fleet directive: matrices are **CI-generated**, but a green test cannot tell
CI how *deeply* a block is modelled — that is human judgment. So this matrix has
two independent columns, and a generator must **read the tier from annotation,
never infer it from a pass**:

**Tier** — fidelity depth (HUMAN judgment; matches 93/95):

| Tier | Meaning |
|------|---------|
| **A** | Data-path verified — moves/computes real data, confirmed by an integrity oracle (data round-trip, e2e), not just a "done" flag. |
| **B** | Driver bring-up — driver binds; registers / IRQ / timing correct. Control blocks, or data blocks not separately oracle-verified. |
| **C** | Registration — present and probes, minimal beyond that. |
| **N/A** | **Absent on i.MX 91 silicon.** Correctly absent; never a failure. |

**Test result** — pass/fail (CI-fillable from the actual run):

| Result | Meaning |
|--------|---------|
| **PASS (qtest)** | Dedicated qtest green this revision. |
| **PASS (boot)** | Exercised and relied on by a real Linux boot to shell. |
| **HARNESS** | Functional harness present (`tests/<name>-imx91/`); not re-run for this snapshot. |

A ⚑ in the Evidence column flags a **fidelity caveat** (see `fidelity-audit.md`).

**Snapshot provenance:** Test-result column verified against
`build/qemu-system-aarch64` on 2026-06-28; model at commit `7ce8a8d`
(`imx91-dev`). qtest suite: **11/11 passed, 18 assertions, 0 failures.**

## Present IP

| IP block | Tier | Test result | Evidence |
|----------|:----:|-------------|----------|
| Cortex-A55 (single core) | A | PASS (boot) | Boots Linux to shell; runs all qtests |
| GICv3 (dist + redist) | A | PASS (boot) | Underpins boot + every qtest IRQ path |
| eDMA1/2 (i.MX9) | A | PASS (boot) | Real src→dst move; audio-capture DADDR-persist e2e |
| SAI1/2/3 | A | PASS (qtest) | `imx91-sai-test` + `tests/audio-imx91/` (samples to userspace) |
| MICFIL (PDM mic) | A | PASS (qtest) | `imx91-micfil-test` + audio capture e2e |
| XCVR (audio transceiver) | A | PASS (qtest) | `imx91-xcvr-test` |
| FlexSPI1 (NOR + SPI-NAND) | A | PASS (qtest) | `imx91-flexspi-test`; boots from real flash contents |
| ISI (parallel-camera capture) | A | PASS (qtest) | `imx91-isi-test` + `tests/camera-imx91/` (`v4l2_cap` frame oracle) |
| FlexCAN ×2 | A | PASS (qtest) | `imx91-flexcan-test` (CAN frame round-trip) |
| FlexIO1 (I²C/SPI/UART emul) | A | PASS (qtest) | `imx91-flexio-test` (event-driven shift-race fix) |
| I3C1 | A | PASS (qtest) | `imx91-i3c-test` |
| LPI2C ×8 | A | PASS (qtest) | `imx91-lpi2c-test` |
| LPSPI ×8 | A | PASS (qtest) | `imx91-lpspi-test` |
| FEC / ENET | A | HARNESS | `tests/functest-imx91/`, boot networking (real frames) |
| EQOS (DWMAC) | A | HARNESS | `tests/functest-imx91/`, boot networking |
| USDHC ×3 | A | HARNESS | SDHCI ADMA datapath (real block data, rootfs from SD/eMMC) |
| LPUART ×8 | A | PASS (boot) | Console I/O |
| SiP SoC-info SMC (id/rev/UID) | A | PASS (boot) | Guest-verified `/sys/devices/soc0/soc_id = i.MX91` rev A0 |
| DDRC (i.MX9) | B | PASS (qtest) | `imx91-ddrc-test` (controller config/register bring-up) |
| LCDIF (parallel-RGB v3) | B | HARNESS | `tests/display-imx91/` (QMP screendump) |
| USB-OTG ×2 (ChipIdea) | B | HARNESS | `tests/functest-imx91/` |
| TPM ×6 | B | PASS (boot) | clockevent/clocksource at boot |
| GPIO ×4 | B | HARNESS | `tests/functest-imx91/` |
| WDOG ×5 | B | HARNESS | `tests/functest-imx91/` |
| TMU (thermal) | B | PASS (boot) | ⚑ honest-parameterizable: fixed, settable `temperature` prop (default 40 °C) |
| ADC1 | B | HARNESS | operator-settable `adc-chN` QOM props (default `0x100+ch*0x111`); injection verified (was silent-wrong, fixed 2026-06-28) |
| OCOTP (fuses) | B | PASS (boot) | SoC serial/UID read (configured MAC/UID) |
| BBNSM (RTC / secure) | B | PASS (boot) | boot |
| SYSCTR / TSTMR | B | PASS (boot) | system timebase |
| SEMA42 ×2 | B | HARNESS | `tests/functest-imx91/` |
| MU1 / MU2 (messaging units) | B | PASS (boot) | boot |
| CCM / ANATOP / SRC (clocks) | B | PASS (boot) | Linux programs directly (no SM) |
| ELE (EdgeLock Enclave, MU) | B | PASS (boot) | honest-fault rail: data cmds count `ele-uncomputed-cmds` + opt-in `ele-unmodelled-errcode` faults the guest (verified); SoC-id is via SiP, not ELE (`fidelity-audit.md`) |
| WM8962 codec (I²C) | B | HARNESS | `tests/audio-imx91/` |
| MT9M114 sensor (I²C) | B | HARNESS | `tests/camera-imx91/` |
| PCA9451 PMIC / PCA9538 expander (I²C) | B | PASS (boot) | I²C regdev |

**SoC identification is correct.** Linux reads the chip id from the SiP SoC-info
SMC (`fsl_imx91_sip_handler`, registered via `arm_register_sip_handler`), which
returns `0xa0009100` → id `0x91`, rev 1.0 (A0). The i.MX 93's `0x9300` word was a
93→91 port artifact and has been fixed; no live `0x9300` remains in the model.

**Fidelity fixes — see [`fidelity-audit.md`](fidelity-audit.md):** the **ADC**
silent-wrong was fixed (operator-settable `adc-chN`, injection verified), and the
**ELE** data-command silent-wrong got the fleet honest-fault rail
(`ele-uncomputed-cmds` counter + one-time log + opt-in `ele-unmodelled-errcode`
guest fault, verified). The only documented residual is ELE DMA-buffer-payload
commands (`GET_INFO`/RNG), which no 91 consumer hits and which leave SoC-id
unaffected. None touch the core boot/identity/storage/audio/camera/net paths,
which are all Tier A.

## Self-hosting capability — in-guest build

Beyond per-IP coverage, the model is verified to **compile and run real code on
the emulated A55** (the 10k-dev "build your code ON the board" guarantee — not
just cross-then-run). Harness: [`tests/in-guest-build-imx91/`](../../tests/in-guest-build-imx91/).
All three levels green (2026-06-29):

| Level | What | Result |
|-------|------|--------|
| 1 | native tcc + musl (built from source), busybox initramfs | **pass=3 fail=0** |
| 2 | real GCC 14.3.0 / g++ + glibc rootfs off SD (`/dev/mmcblk0`) — C, libm, C++ STL | **pass=3 fail=0** |
| 3 | real upstream projects (bzip2/zlib/lua) built natively + their own `make test` | **pass=3 fail=0** |

## Interconnect capability — pass real data between instances

Mission #5: the model passes real data over its links between QEMU instances, in
the per-link socket shape a lab coordinator wires. Harness:
[`tests/interconnect-imx91/`](../../tests/interconnect-imx91/).

| Link | What | Result |
|------|------|--------|
| Ethernet | two 91s, FEC eth0 `-nic socket` bridge, byte-exact payload | **PASS** |
| UART | two 91s, LPUART2 `/dev/ttyLP1` `-chardev socket` bridge, byte-exact | **PASS** |
| SPI | two 91s, LPSPI1 `/dev/spidev0.0` via `spi-link` `-chardev socket`, byte-exact | **PASS** |
| CAN | two 91s, FlexCAN `can0` via `can-host-chardev` `-chardev socket`, frame byte-exact | **PASS** |
| USB | 91 usbredir host ↔ MCX gadget, HS enum + EP1 bulk-echo byte-exact | **PASS** |
| USB-CDC | 91 `cdc_acm` ↔ MCX CDC gadget, `/dev/ttyACM0` serial round-trip byte-exact | **PASS** |

## Developer access — PuTTY (serial + SSH)

The board-farm "a dev reaches the board like a real EVK" check. Harness:
[`tests/putty-imx91/`](../../tests/putty-imx91/) boots the full BSP rootfs:

| Path | What | Result |
|------|------|--------|
| Serial | `serial-getty@ttyLP0` → `imx91evk login:` → root shell (PuTTY over serial) | **PASS** |
| SSH | openssh sshd → `ssh root@` over an eQOS `hostfwd` | **PASS** |

Both FEC (eth0) + eQOS (eth1) bind and DHCP on the **stock EVK dtb** — real
ethernet, no netdev tweak needed.

## Absent IP — N/A (NOT a negative result)

These blocks do **not exist on i.MX 91 silicon**. Listed so the absence is
explicit and auditable; none should ever appear as a failing test.

| IP block | Tier | Why absent |
|----------|:----:|-----------|
| Ethos-U65 NPU | N/A | No NPU on i.MX 91 (93-only). The fleet's worst silent-wrong class is absent here. |
| Cortex-M33 (+ its MU peer) | N/A | No M33 on i.MX 91 |
| 2nd Cortex-A55 | N/A | i.MX 91 is single-core |
| PXP 2D engine | N/A | Not present on i.MX 91 |
| MIPI-DSI | N/A | i.MX 91 has parallel-RGB LCDIF only |
| MIPI-CSI | N/A | i.MX 91 has parallel camera (ISI) only |
| LVDS | N/A | Not present on i.MX 91 |
| ADV7535 HDMI bridge | N/A | No DSI → no HDMI-bridge chain |
| System Manager (SM) | N/A | Absent on i.MX 93/91; Linux drives CCM/ANATOP/SRC directly |

## How to read this for board-farm use

- **Tier A** — trust the block for real developer data (oracle-verified).
- **Tier B** — driver and control/timing are correct; for data blocks not yet
  oracle-verified, confirm against your asset set. Mind any ⚑ fidelity caveat.
- **Tier C** — present and probes; minimal beyond registration.
- **N/A** — the silicon has no such block; absence is correct. Guest code that
  probes for it should see "not present," not a silent-wrong success.
- **⚑** — read `fidelity-audit.md` before trusting that block for real data.

## CI generation

Per the operator directive, this matrix is CI-generated. The GitHub Actions
workflow [`.github/workflows/imx91-validation-matrix.yml`](../../.github/workflows/imx91-validation-matrix.yml)
builds the aarch64 target + the i.MX 91 qtests and runs
[`tests/gen-test-matrix.py`](../../tests/gen-test-matrix.py), which fills the
**Test result** column from the actual qtest run while reading **Tier** and the ⚑
caveats verbatim from the in-repo annotation [`test-matrix.yaml`](test-matrix.yaml) —
CI assembles, it does **not** invent tiers (mis-tiering would itself be a silent
fail). The generator exits non-zero on any qtest regression, so the job gates on
it; the assembled matrix is published as a job summary + artifact. The boot /
in-guest-build tiers need BSP assets absent in CI, so the generator labels those
rows from their declared kind and gates only on the qtest rows it runs.

This document is the curated human reference; the CI-generated form is the
machine artifact. Final convergence to the fleet-canonical template follows after
the first repo (i.MX 93) clears upstream review (holobench distills + propagates).
