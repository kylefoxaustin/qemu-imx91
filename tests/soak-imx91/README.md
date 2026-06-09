# i.MX 91 soak test

A long-running endurance + regression harness that exercises **every function**
of the QEMU i.MX 91 machine across the BSP's variant device trees, accumulating
per-function pass/fail counters, tracking qemu RSS for leaks, and running the
kernel-free qtest suite periodically.

## Run it

```sh
tests/soak-imx91/soak.sh            # run until ^C (prints a dashboard on exit)
tests/soak-imx91/soak.sh 12         # run for 12 hours
CYCLES=20 tests/soak-imx91/soak.sh  # run 20 boot-cycles then stop
DTBS="imx91-11x11-evk imx91-9x9-qsb" ITERS=10 tests/soak-imx91/soak.sh
```

Paths default to the in-tree BSP deploy; override `DEPLOY`, `QEMU`, `KERNEL`,
`ALSA_SYSROOT`, etc. as needed. `^C` (or the time/cycle limit) prints the
dashboard.

## Design

Two layers (adapted from the i.MX 93's 36h soak, with its supervisor lessons):

- **`soak-init`** — the in-guest battery. Runs each boot, self-selecting tests
  from what the booted DTB exposes, and emits structured serial markers:
  `SOAK:PASS|FAIL|SKIP:<function>:<detail>`, `SOAK:BOOT:<model>`, and
  `SOAK:BATTERY:DONE`. Data-path tests (audio, storage) loop `$ITERS` times per
  boot. It ends with a clean PSCI `poweroff` so qemu exits immediately.

- **`soak.sh`** — the host supervisor. Builds the shared initramfs (rootfs +
  matching kernel modules + cross-compiled `pcm_play`/`pcm_capture`/`v4l2_cap`
  oracles + injected `libasound`) and an ext4 SD image **once**, then loops:
  rotate to the next DTB variant (+ its machine opts, e.g.
  `flexspi-flash=gd5f4gq4` for the NAND DTB), boot it, sample VmRSS, parse the
  markers into cumulative per-function counters, and every `QTEST_EVERY` cycles
  run the 10 kernel-free qtests. A leak is flagged if peak RSS exceeds 1.5x the
  first post-liveness sample.

## Coverage

Across the variant rotation (evk / i3c / mqs / 8mic / flexspi-nand / mt9m114 /
panel / frdm / 9x9-qsb), the battery verifies:

| group | functions |
|-------|-----------|
| core | boot to userspace, soc-id |
| net | FEC + ENET_QoS DHCP |
| storage | uSDHC write+sync+md5 readback (looped) |
| flash | FlexSPI NOR; SPI-NAND enumerate+read (nand DTB) |
| i2c/i3c | bound-device check; wm8962-on-I3C (i3c DTB) |
| audio | SAI play, SAI capture, MICFIL capture, MQS (mqs DTB), 8-mic (8mic DTB) |
| camera | MT9M114 -> CSI -> ISI V4L2 capture (mt9m114 DTB) |
| usb | usb-storage enumeration |
| misc | GPIO, RTC, ADC, TMU, watchdog presence, DDR-PMU perf interface |
| kernel-free | the 10 imx91 qtests, every N cycles |

## Notes

- **soc-id** can SKIP: in this init flow the shared `imx93_ele` model answers
  the ELE GET_INFO with the 93's soc_id (0x9300), so `se_ctrl` logs "No matching
  index" and can race soc-imx9's probe, leaving `/sys/devices/soc0` absent. The
  SiP SoC-info value (i.MX91) is verified independently (functest + a minimal
  boot), so the soak doesn't fail over this probe-order artifact.
- FlexIO is intentionally not exercised (the 93 documents a ~1/1000
  atomic-shift-event timing flake under sustained load; the 91 dodges it).
- `-audio driver=wav` writes the played PCM to a file, never the host backend
  (which would beep for the whole run).
- Never rebuild the on-disk qemu mid-soak — the supervisor re-execs it each boot.
