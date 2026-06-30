# i.MX 91 QEMU — peripheral fidelity audit

The board-farm standard (fleet directive, operator 2026-06-28): a developer
brings their own correctly-built binary; **code that runs on real i.MX 91 silicon
should run on the model and get the right answer, or fail honestly.** The worst
bug class is a *silent wrong answer* — a block that signals completion/success but
didn't actually compute (accelerators that don't compute; analog blocks returning
constants). Either compute correctly, or fault/flag honestly.

This is the honest per-block record for the i.MX 91: what the farm can trust, what
fails honestly (acceptable), and what **silently lies** (do not trust for real
data). It is the companion to [`test-result-matrix.md`](test-result-matrix.md)
(present/absent + test status) — this file is the *fidelity* judgment.

## Classes

- **COMPUTES** — submits work, returns the correct result. Trust it.
- **HONEST-PARAMETERIZABLE** — returns a documented, operator-settable value (a
  model stand-in for a real-world analog input). Deterministic, not a hidden lie.
- **FAULTS / ABSENT (honest)** — errors, rejects probe, or isn't present. The
  guest sees a failure/absence and can handle it. Acceptable.
- **SILENT-WRONG** — signals done/success but returns a constant/garbage with no
  signal to the guest. **Do not rely on it for real data.** Must be fixed
  (compute / make injectable) or made to fault honestly.

## The i.MX 91 advantage: the worst silent-wrong class is absent

The fleet's hardest silent-wrong problem is the **un-modellable proprietary
accelerator** — the Neutron NPU (i.MX 95) and Ethos-U65 (i.MX 93), whose compute
is NXP-proprietary firmware that cannot be emulated, so the model must ack `DONE`
without computing and rely on a non-gating error channel for honesty.

**The i.MX 91 has no NPU at all.** That entire class is **N/A — ABSENT** here (see
the matrix). The 91 carries none of the "acks-DONE-without-computing" risk that
93/95 must actively flag. The NXP-accel honest-fault taxonomy (Neutron = fault via
side-band MBOX1; Ethos = fault via in-band non-gating `rsp->status`; the honest
fault is never the completion signal) is recorded for the fleet but **does not
apply to any i.MX 91 board** — the 91 instantiates no remote accelerator.

## Verdicts

| Block | Class | Evidence |
|-------|-------|----------|
| **NPU (Ethos-U65)** | **N/A — ABSENT** | Not present on i.MX 91 silicon. The fleet's worst silent-wrong class does not exist here. Never a "negative" cell. |
| **SAR-ADC** | **FIXED → HONEST-PARAMETERIZABLE** (2026-06-28) | Was SILENT-WRONG: every channel returned a fixed mid-scale `0x800` + EOC success, no injection, no flag. Now each channel is a read/write QOM property `adc-ch0..7` settable at runtime (`qom-set /machine/soc/adc1 adc-chN <v>`), defaulting to a documented distinct-per-channel pattern `0x100+ch*0x111` so an un-driven channel is deterministic, not a hidden lie. `adc_convert()` latches EOC + IRQ but no longer overwrites the value. `hw/adc/imx93_adc.c`. **Verified guest-visible:** injected `adc-ch3=0x555` → read back `0x555`; undriven `adc-ch5` → default `0x655`. Ports the i.MX 95 ADC fix to the 91's (separate) impl. |
| **ELE (EdgeLock Enclave, MU)** | **FIXED → honest disclosure + opt-in guest fault** (2026-06-29) | `hw/misc/imx93_ele.c` — bring-up responder: acks commands with `ELE_SUCCESS` and a **zeroed** payload, so the data-returning commands (`READ_FUSE`/`GET_FW_VERSION`/`GET_STATE`, the >2-word replies) were a latent silent-wrong. Now honest, mirroring the 95 Neutron pattern: each such reply (1) increments QOM-gettable `ele-uncomputed-cmds` and logs a one-time `LOG_GUEST_ERROR`, and (2) honours an operator opt-in `ele-unmodelled-errcode` (default 0 = faithful success) that returns a non-`0xD6` status so a guest checking ELE status sees an honest "did not compute" fault. **Verified:** `READ_FUSE` with default → `RR1=0xD6` (happy path intact); with `ele-unmodelled-errcode=0x29` → `RR1=0x29` (guest-visible fault). **Low exposure on the 91:** SoC-id is via the SiP SMC (not ELE), and the 91 does not wire the ELE as `/dev/hwrng`. **Remaining documented limit:** commands whose payload is a guest DMA buffer (`GET_INFO`, any RNG/`GET_RANDOM`) are not modelled — the guest reads its own pre-zeroed buffer; the opt-in errcode covers the rr-word commands, not these (would need the verified ELE message layout to write real DMA output). |
| **TMU (thermal)** | **HONEST-PARAMETERIZABLE** | `hw/misc/imx93_tmu.c:11,53,118` — reports a fixed temperature settable via the `temperature` property (millicelsius, default 40000 = 40 °C), returned as valid Kelvin once monitoring is enabled. Documented; no hidden constant. The guest gets a valid, deterministic, operator-settable temperature — not a measured one, but not a lie. |
| **SiP SoC-info SMC** | **COMPUTES** | `hw/arm/fsl-imx91.c:247,252` — returns `0xa0009100` → Linux `soc-imx9.c` decodes id `0x91`, rev 1.0 (A0). Verified guest-visible: `/sys/devices/soc0/soc_id = i.MX91`. The 93's `0x9300` was a port artifact, fixed; no live `0x9300` remains. |
| **LPUART** | **COMPUTES (PIO) — DMA-RX caveat** | `hw/char/imx_lpuart.c` is PIO-only (RDRF + RIE). TX and PIO-RX move real bytes (console + the `tests/interconnect-imx91/` UART link both pass byte-exact). Caveat: the EVK DTB marks the LPUART nodes `dmas = rx,tx`, so the Linux `imx-lpuart` driver uses **DMA-RX**; the model never raises the eDMA completion (the LPUART↔eDMA request line is unmodelled), so in DMA mode received bytes never reach userspace — an honest *no-data* timeout, not a wrong value. The interconnect harness side-steps it by enabling the link UART in PIO (drops `dmas`). Proper fix: wire the LPUART↔eDMA RX/TX request lines (as SAI/MICFIL audio capture does). |
| **eDMA (i.MX9)** | **COMPUTES** | `hw/dma/imx93_edma.c` — actually reads source → writes dest in guest memory (incl. the signed-20-bit NBYTES / MLOFFYES decode), then DONE/IRQ. Verified by the SAI+MICFIL audio-capture e2e (DADDR-persist path). |
| **SAI1/2/3, MICFIL** | **COMPUTES** | Audio FIFO math moves real samples; SAI+MICFIL capture verified end-to-end to userspace `pcm_capture`. qtests `imx91-sai-test`, `imx91-micfil-test` green. |
| **FlexSPI1** | **COMPUTES** | Serves real flash contents (is25wp064 NOR / gd5f4gq4 SPI-NAND); boots from it. qtest `imx91-flexspi-test` green. |
| **ISI (parallel camera capture)** | **COMPUTES** | Writes captured frames to guest memory (sensor→ISI pipeline); `v4l2_cap` oracle reads real frame data. qtest `imx91-isi-test` green. |
| **FEC / EQOS (ENET)** | **COMPUTES** | Moves real Ethernet frames (boot DHCP/networking). |
| **USDHC ×3** | **COMPUTES** | SDHCI ADMA datapath moves real block data (boot/rootfs from SD/eMMC). |
| **FlexCAN, FlexIO, I3C, LPI2C, LPSPI, XCVR, DDRC** | **COMPUTES** | Register/data round-trips verified by their qtests (all green); FlexIO carries the event-driven shift-race fix. |
| **OCOTP** | **COMPUTES / honest** | Returns configured MAC/UID fuse words (no soc-id field — that's the SiP SMC). No hidden constant surfaced in the audit. |
| **GPIO, WDOG, TPM, MU, BBNSM, SYSCTR, SEMA42, CCM/ANATOP/SRC** | **COMPUTES** (control/timing) | Register/IRQ/timing semantics; relied on by a full Linux boot to shell. No compute-fakery (these are control blocks, not data engines). |

## Silent-wrong blocks — status & disposition

1. ~~**SAR-ADC**~~ — **FIXED (2026-06-28).** Per-channel operator-settable
   `adc-chN` QOM properties (default = documented distinct-per-channel pattern);
   `adc_convert()` no longer overwrites with a constant. Verified guest-visible
   (inject `adc-ch3=0x555` → reads `0x555`). SILENT-WRONG → HONEST-PARAMETERIZABLE.
2. ~~**ELE fuse/version/state payloads**~~ — **FIXED (2026-06-29).** The
   data-returning (>2-word) commands now count into `ele-uncomputed-cmds`, log
   once, and honour the opt-in `ele-unmodelled-errcode` (default 0 = faithful)
   to fault the guest honestly. Verified guest-visible. **Still OPEN (documented,
   low priority):** the DMA-buffer-payload commands (`GET_INFO`, RNG) — the guest
   reads its pre-zeroed buffer; closing these needs the verified ELE message
   layout to write real DMA output, and no 91 consumer currently hits them.

Everything else audited is **COMPUTES** or **HONEST-PARAMETERIZABLE** (TMU). The
data engines (eDMA / SAI / MICFIL / FlexSPI / ISI / ENET / USDHC) are verified by
integrity oracles (audio/camera/boot e2e, qtest round-trips), not just "done"
flags.

## Fixing vs flagging

Per the standard, each silent-wrong block must either be made to **compute /
inject** or made to **fault honestly**. Until the two OPEN items above are
addressed, this record is their honest disclosure for the farm. None of them
affect the 91's core boot/identity path (SoC-id, storage, audio, camera, net are
all COMPUTES).
