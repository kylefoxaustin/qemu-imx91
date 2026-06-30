# interconnect tier — pass real data between i.MX 91 instances

Mission #5 of the board-farm directive: the emulated boards must **hook up and
pass real data** over their links (ethernet, USB, SPI, UART). This harness proves
the i.MX 91 does — and in the exact shape Holobench wires links into a lab
(a QEMU socket bridge between two instances), so the 91 is drop-in lab-ready.

```sh
tests/interconnect-imx91/run-eth.sh     # two i.MX 91 instances, FEC <-> socket <-> FEC
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

## How Holobench wires it

The two-instance `-nic socket,listen=`/`-nic socket,connect=` pair is the same
transport Holobench's LabCoordinator uses for ethernet segments (per-link socket,
isolated transport). To wire the 91 into a lab, point one instance's
`-nic socket,connect=` at the segment's listener; the model side stays stock.

## Roadmap (other links)

Ethernet is proven. Next transports to add the same byte-exact oracle for:
- **UART** — bridge a second LPUART (`/dev/ttyLP*`) between two instances via a
  `-serial socket` chardev; pass + verify a payload.
- **USB** — the 91's ChipIdea controller over `usbredir` (the 93<->MCX link shape);
  shares the upstream importer-contract + char-socket reconnect work the fleet is
  already hardening.
- **SPI/CAN** — LPSPI / FlexCAN are qtest-proven at the controller level; a
  cross-instance bridge is the remaining step.
