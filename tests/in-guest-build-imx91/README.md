# in-guest build tier — compile *and* run on the i.MX 91 A55

The strongest self-hosting signal for the board farm: a developer can build their
C/C++ code **on the emulated board** and run the result, not just cross-compile it
on a host. ([`tests/sweep-imx91`](../sweep-imx91/) cross-compiles on the host then
runs in the guest; this compiles *and* runs entirely in the guest.)

Ported from the i.MX 95 in-guest-build tier (fleet diff-ability — 95 recommended
all Linux-booting emulators carry it). **91 delta:** the i.MX 91 has **no System
Manager / M33** (single Cortex-A55), so there is no `-device loader,...m33.elf`;
the boot is the plain `imx91-11x11-evk` tuple (see [`tests/boot-imx91`](../boot-imx91/)),
and the gcc rootfs is on the SD bus (`-drive if=sd`) which the USDHC presents as
`/dev/mmcblk0`.

```sh
tests/in-guest-build-imx91/run.sh           # tcc + musl, busybox initramfs (lightweight)
tests/in-guest-build-imx91/run-gcc.sh       # real gcc/g++ + glibc rootfs off SD (representative)
tests/in-guest-build-imx91/run-gcc-build.sh # REAL upstream projects: ./configure/make + their own tests
```

All three compile every test **in-guest**, run each binary on the A55, and check
its stdout against the program's own `// EXPECT:` line (exit 0 iff every case
builds and prints the expected output; SKIP if a prerequisite is missing).

**Verified on this model (2026-06-29):** L1 `pass=3 fail=0`, L2 `pass=3 fail=0`,
L3 `pass=3 fail=0` — the A55 hosts a compiler, runs the binaries it produces, and
natively builds + tests real upstream projects (gcc 14.3.0 in the rootfs).

## Level 1 — tcc + musl (`run.sh`)

Builds a tiny native toolchain **from source** (nothing committed as a binary),
boots the busybox initramfs on `imx91-11x11-evk`, and compiles `tests/*.c` with it.
Prereqs: QEMU, kernel+dtb (BSP `DEPLOY`), the 91 busybox cpio, a cross gcc + git +
curl.

- **TinyCC (native aarch64)** — a C compiler cross-built to *run on the A55* and
  emit arm64. tcc is itself real third-party code. Its arm64 backend is young (its
  linker can't do glibc's GOT/TLS relocations and its inline asm lacks `svc`),
  which is why the libc is musl and the programs are ordinary libc C.
- **musl libc (static)** — tcc's canonical companion; static objects avoid the
  relocations glibc pulls. Linked `-static` (the initramfs has no dynamic loader).

Hard-won build gotchas, all encoded in the script: the `c2str` build-helper is
compiled with the **host** cc; `libtcc1.a`'s arm64 runtime objects are compiled
with the cross cc; **both** musl and `libtcc1.a` must be `-fno-stack-protector`
(tcc links `libtcc1` after libc, so `__stack_chk_*` otherwise go unresolved); musl's
`crt1.o/crti.o/crtn.o` + `libc.a` are staged in `/usr/lib/<triplet>`, headers in
`/usr/include`.

## Level 2 — the real gcc rootfs (`run-gcc.sh`)

A **real native GCC** (glibc gcc/g++ + libstdc++ + libm, GCC 14) on a Debian rootfs
booted as the SD root filesystem, compiling `gcc-tests/*.{c,cpp}` (incl. C++ STL +
libm) — what a farm developer actually does. The aarch64 rootfs is the upstream
multi-arch `gcc` Docker image: `docker pull --platform linux/arm64` fetches a
foreign-arch image **without running it**, and `docker export` yields a complete
native arm64 gcc (no cross or emulation on the host). The script stages tests + a
tiny init, builds an ext4 image with `mke2fs -d` (no root), and boots it as
`/dev/mmcblk0`. Prereqs add **docker** + `mke2fs`. Gotchas (each cost a boot):

- invoke gcc by **full path** (`/usr/local/bin/gcc`) — bare `gcc` computes a
  relative exec-prefix and can't find `cc1` ("cannot execute 'cc1'");
- `PATH` must include `/usr/bin` so `collect2` finds `ld` at link;
- the rootfs has no init system, so it boots `init=/igtest.sh` directly and the
  script **must never exit** (else "Attempted to kill init" panic) — it loops
  after poweroff;
- inject the script via `mke2fs -d` (rebuild the image), **not** `debugfs write`
  (which silently truncated it here).

## Level 3 — real upstream projects (`run-gcc-build.sh`)

Builds **real upstream projects** on the A55 and runs each project's **own** test
suite — proving the machine is a working dev host (download → `./configure`/`make`
→ `make test` passes, all in-guest). Corpus: **bzip2** (`make && make test`),
**zlib** (`./configure && make && make test`), **lua** (`make posix`, then run a
Lua script). Tarballs are fetched host-side (cached) and staged into the rootfs,
so the guest needs no network. Real builds under TCG are slow — the default timeout
is generous (`TMO=1200`).

## Adding a test

Drop a `tests/<name>.c` (tcc variant) or `gcc-tests/<name>.{c,cpp}` (gcc variant)
whose **first line** is `// EXPECT: <exact stdout>`. tcc/musl cases must stay
within tcc's arm64 codegen (loops, arithmetic, libc: `printf`, `qsort`, `malloc`,
`string.h`); gcc cases can use anything glibc/libstdc++ provide (C++ STL, libm, …).
Committed: tcc — `sum.c` (loops+printf), `fib.c` (arithmetic), `qsort.c` (musl
stdlib+fn ptrs); gcc — `sum.c`, `sqrt.c` (libm), `cppsort.cpp` (g++ + STL).

Build artifacts (tcc, musl, the gcc rootfs, project tarballs, ext4 images) are
cached under `build/in-guest-build/` (gitignored).
