# interconnect tier — pass real data between i.MX 91 instances

Mission #5 of the board-farm directive: the emulated boards must **hook up and
pass real data** over their links (ethernet, USB, SPI, UART). This harness proves
the i.MX 91 does — and in the exact shape Holobench wires links into a lab
(a QEMU socket bridge between two instances), so the 91 is drop-in lab-ready.

```sh
tests/interconnect-imx91/run-eth.sh     # two instances, FEC <-> socket <-> FEC
tests/interconnect-imx91/run-uart.sh    # two instances, LPUART2 <-> socket <-> LPUART2
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

## How Holobench wires it

The two-instance socket pair (`-nic socket,listen=`/`connect=` for ethernet,
`-chardev socket,server=on/off` for UART) is the same per-link, isolated-transport
shape Holobench's LabCoordinator uses for ethernet segments and serial links. To
wire the 91 into a lab, point one instance's connector at the segment's listener;
the model side stays stock.

## Roadmap (other links)

Ethernet and UART are proven. Next transports to add the same byte-exact oracle:
- **USB** — the 91's ChipIdea controller over `usbredir` (the 93<->MCX link shape);
  shares the upstream importer-contract + char-socket reconnect work the fleet is
  already hardening.
- **SPI/CAN** — LPSPI / FlexCAN are qtest-proven at the controller level; a
  cross-instance bridge is the remaining step.
