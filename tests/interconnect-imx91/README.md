# interconnect tier — pass real data between i.MX 91 instances

Mission #5 of the board-farm directive: the emulated boards must **hook up and
pass real data** over their links (ethernet, USB, SPI, UART). This harness proves
the i.MX 91 does — and in the exact shape Holobench wires links into a lab
(a QEMU socket bridge between two instances), so the 91 is drop-in lab-ready.

```sh
tests/interconnect-imx91/run-eth.sh     # two instances, FEC <-> socket <-> FEC
tests/interconnect-imx91/run-uart.sh    # two instances, LPUART2 <-> socket <-> LPUART2
tests/interconnect-imx91/run-usb.sh     # 91 usbredir host <-> MCX gadget, enum + bulk
```

## Ethernet link (`run-eth.sh`)

Two i.MX 91 guests, each board's **FEC (eth0)** bridged by a QEMU socket netdev
(`-nic socket,listen=` on the server instance, `-nic socket,connect=` on the
client). Static IPs on eth0 (server `192.168.7.1`, client `192.168.7.2`); the
second NIC (`-nic user` = EQOS/eth1) is unused.

The oracle is real data movement, not link-up: the [`linktool`](linktool.c) static
binary echoes a known payload — client sends it, server echoes it back, client
verifies **byte-exact**. A pass means the payload actually traversed
guest A → FEC → socket bridge → FEC → guest B and back. Verified:

```
LINK:PASS:server:echoed 40 bytes [IMX91-ETH-LINK-payload-0123456789-abcdef]
LINK:PASS:client:echo byte-exact (40 bytes)
PASS: payload crossed FEC<->socket<->FEC byte-exact between two i.MX 91 guests
```

`linktool` avoids `getaddrinfo`/NSS (raw sockets + `inet_pton`) so it links
`-static` cleanly with the Ubuntu aarch64 cross gcc. The client retries `connect`
for ~60 s to ride out boot/ARP warmup, so launch order and first-packet ARP loss
don't matter.

### Gotcha (cost a boot)

**Memory must be >= ~1 GiB** (`MEM=2G` default). The i.MX 91 DTB gives the FEC a
coherent DMA pool at a high physical address; at `-m 512M` that region is outside
RAM and the driver's `dma_alloc_from_dev_coherent` → `__memset` takes a
synchronous external abort (`0x96000050`) during probe. This is a guest-memory
sizing requirement, not a model bug — the FEC works (it moves the bytes). The
boot/functest harnesses already use a large `-m` for the same reason.

## UART link (`run-uart.sh`)

Two i.MX 91 guests, each board's **LPUART2 (`/dev/ttyLP1`)** bridged by a QEMU
socket chardev (`-chardev socket,...,server=on` on the receiver, `server=off` on
the sender, attached as each board's 2nd `-serial`). The receiver reads a line,
the sender writes the payload; the receiver verifies it **byte-exact**. Verified:

```
LINK:PASS:recv:got byte-exact [IMX91-UART-LINK-payload-0123456789]
PASS: payload crossed LPUART2<->socket<->LPUART2 byte-exact between two guests
```

The base EVK DTB only enables LPUART1 (the console, `ttyLP0`), so the harness
generates a patched DTB (via the BSP `dtc`) enabling `serial@44390000` (LPUART2);
the `serial1` alias already points at it. SKIPs cleanly if `dtc` isn't found.
The node keeps its `dmas` props — DMA-mode RX is modelled, so no PIO workaround.

### LPUART DMA-RX (a model fix this harness drove out)

Bringing up this link uncovered that DMA-mode UART RX wasn't modelled, and the fix
landed in `hw/char/imx_lpuart.c`: the i.MX `imx-lpuart` driver pages RX through a
**cyclic eDMA** channel and flushes it on an **IDLE interrupt** (it sets
`dma_idle_int`), not via RDRF. So the model now, on each received byte with
`BAUD.RDMAE` set, asserts the LPUART's `dma-req-rx` line (the eDMA pages the byte
from `DATA` into the driver's ring) and raises `STAT.IDLE` (driving the driver's
ring flush). The real blocker was the MMIO `.valid.min_access_size = 4`: the eDMA
reads `DATA` a **byte** at a time, so the access was rejected before reaching the
handler and `rx_full` never cleared — fixed by allowing byte-wide `valid` access
(the `.impl` width keeps the handler in 32-bit units). Each LPUART's RX request is
wired to its eDMA at the DTB source id in `fsl-imx91.c`. TX needs no request line
(mem→device runs whole at channel start). Recorded in
[`docs/validation/fidelity-audit.md`](../../docs/validation/fidelity-audit.md).

## USB link (`run-usb.sh`)

USB is host/device asymmetric: the 91's ChipIdea controller is a **host** (EHCI),
so the 91 is the usbredir **host/importer** (stock `-device usb-redir`) and the
**device** end is the fleet's MCX gadget (`frdm-mcxn947` running the HS/ChipIdea
usbredir device firmware, `tests/mcxn-usb-link/serve.sh` in the mcxn947qemu tree)
— the same pairing the i.MX 93 ↔ MCX link uses. The harness launches the gadget
server on a private socket, boots the 91 as the importer, waits for enumeration,
then runs the [`usbbulk`](usbbulk.c) usbfs oracle. Verified end-to-end:

```
usb 2-1: new high-speed USB device number 2 using ci_hdrc
ENUM: device enumerated after 1s   →   BULK:FOUND 1fc9:0094 at bus 2 dev 2
BULK: EP1 OUT wrote 64 bytes / EP1 IN read 64 bytes
BULK:PASS: 64 bytes echoed byte-for-byte over the live link
```

Full chain both ways: guest usbfs app → i.MX 91 Linux USB → ci_hdrc/EHCI → QEMU
usb-redir → unix socket → MCX firmware EP1 echo → back. **Depends on the ChipIdea
`PORTSC.PSPD` fix** (commit `bf8262fec5`, cross-validated from the i.MX 93) —
without it the 91 host downgrades the HS gadget to full-speed and clamps its
512-byte bulk EPs.

**Gotcha (2×TCG contention):** the gadget server and the 91 both run under TCG on
one host, so let the server warm up before the 91 boots (`WARMUP=5` default) —
otherwise the enumeration control transfers starve and the device attaches at HS
but never finishes reading descriptors. SKIPs cleanly if the mcxn947qemu gadget
server isn't present (set `MCXSERVE=`).

## How Holobench wires it

The per-link socket pair (`-nic socket,listen=`/`connect=` for ethernet,
`-chardev socket,server=on/off` for UART, and a usbredir socket for USB) is the
same isolated-transport shape Holobench's LabCoordinator uses. To wire the 91
into a lab, point its connector/importer at the segment's listener/device end;
the model side stays stock. For USB the 91 is the host importer
(`-device usb-redir,chardev=...`) against the MCX device end — the exact arg-pair
the fleet's i.MX 93 ↔ MCX link uses.

## Roadmap (other links)

Ethernet, UART and USB are proven. Next transports to add the same byte-exact
oracle:
- **SPI/CAN** — LPSPI / FlexCAN are qtest-proven at the controller level; a
  cross-instance bridge is the remaining step.
