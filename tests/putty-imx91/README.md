# PuTTY-readiness (i.MX 91)

The board-farm developer test: can someone reach the emulated i.MX 91 the two
ways they'd reach a real EVK — a **serial terminal** (PuTTY over serial to the
console) and **SSH**? This boots the full BSP rootfs so systemd brings up
`serial-getty` + `sshd`, then verifies both paths end-to-end.

```sh
tests/putty-imx91/run.sh
```

Verified:

```
serial-getty login on ttyLP0 : PASS     (imx91evk login: -> root -> shell)
ssh root@ over eQOS hostfwd  : PASS     (PUTTY_SSH_OK=root:Linux aarch64)
PASS: a developer can PuTTY into the i.MX 91 over serial AND ssh
```

## What it does

- Boots the **full imx-image-core rootfs** (`.wic` as the SD root,
  `root=/dev/mmcblk0p2`) — not a busybox initramfs — so `systemd` starts the real
  `serial-getty@ttyLP0` and `sshd`.
- **Serial:** a socket-backed console (exactly what `-serial pty`/`socket` + PuTTY
  attaches to) reaches `imx91evk login:`, logs in as **root (empty password)**,
  and runs a command in the interactive shell.
- **SSH:** `openssh sshd` (`PermitRootLogin yes` + `PermitEmptyPasswords yes`) —
  `ssh root@` over a slirp `hostfwd` on the **eQOS/eth1** NIC logs in + runs a
  command (driven with `sshpass -p ''`).

Both **FEC (eth0)** and **eQOS (eth1)** bind and DHCP, so SSH works out of the box
over real modeled ethernet — no virtio-net workaround needed.

## Gotchas (baked in)

- **SD size:** QEMU rejects a non-512K-multiple SD image (`Invalid SD card size`),
  so the harness copies the `.wic` and `qemu-img resize`s it to 3G. (It never
  touches the deploy asset.)
- **Silence:** `-audio driver=none` keeps the boot off any host audio backend.
- SKIPs cleanly if the BSP rootfs `.wic.zst`, `zstd`, `qemu-img`, or `sshpass`
  aren't present (the `.wic` is an NXP BSP artifact, not redistributable).

## How Holobench uses it

This is the shape of Holobench's developer-access exercise (PuTTY over serial +
SSH into an EVK). For a lab, point PuTTY at the serial backend (`-serial pty`/a
socket) for the console, or SSH to the `hostfwd` port. The i.MX 91's FEC + eQOS
bind and DHCP on the **stock EVK dtb**, so no netdev tweak is needed. (The i.MX 95
ENETC also binds/COMPUTES, but its stock dtb points the ports at unmodeled
external PHYs and needs a one-line fixed-link dtb override; the 91 needs none.)
