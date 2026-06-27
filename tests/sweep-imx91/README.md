# i.MX 91 code sweep

A manifest-driven sweep that builds a corpus of **real third-party code** (each
with source + a defined oracle), cross-compiles it for the A55, runs it on the
i.MX 91 QEMU machine, and scores the results. The Linux/MPU analog of the
bare-metal example-corpus sweeps the fleet (91/93/95/mcxn947) run — same
`SOAK:` marker grammar and scorer as `tests/soak-imx91`, so the dashboards are
diff-able across machines.

## Run

    tests/sweep-imx91/sweep.sh                 # whole corpus
    tests/sweep-imx91/sweep.sh zlib sqlite     # named items only

    BOOT_TIMEOUT=600 ITEM_TIMEOUT=120 ./sweep.sh   # tune the guards

It cross-builds the corpus, stages the static test binaries into a one-shot
initramfs over the stock `imx-image-core` rootfs, boots once, and prints a
PASS/FAIL/SKIP dashboard. The raw console of the last boot is kept in
`.last-boot.log` for post-mortem.

## Layout

    scoreboard.sh      shared SOAK:-marker scorer (bump/score_log/dashboard)
    build-corpus.sh    host: drives every corpus/<name>/build.sh into a stage tree
    sweep-init         in-guest /init: runs each item under a per-item timeout
    sweep.sh           supervisor: build -> initramfs -> boot once -> score
    corpus/<name>/
        build.sh       host-side cross-compile; stages artifacts into $STAGE
        run.sh         in-guest; runs the binary vs its oracle, emits SOAK: lines
        <assets>       static data shipped alongside (selftest.lua, workload.sql)

## Adding a corpus item

Create `corpus/<name>/build.sh` (env: `CROSS`, `STAGE`, `DL`) that fetches +
cross-builds and copies runnable artifacts into `$STAGE`, and `run.sh` that
exercises them and prints one line per routine:

    SOAK:PASS:<name>:<detail>      ok
    SOAK:FAIL:<name>:<reason>      ran but oracle rejected
    SOAK:SKIP:<name>:<reason>      n/a / dep absent  (first-class, not a fail)

A build that fails is auto-staged as `SOAK:SKIP:<name>:build-failed` so the
scoreboard stays honest.

## Oracle tiers (highest signal first)

1. **self-checking** — the upstream's own test program ships the oracle and
   tracks the version (zlib `example`, crypto-algorithms KATs, lua self-test).
2. **round-trip** — compress/decompress/encode-decode and compare (bzip2, zstd).
3. **differential** — build the same source for host + target, the host output
   is the golden, the A55 must reproduce it (xxHash, sqlite). Catches
   width/endian/UB-codegen divergence the self-tests miss.

## Corpus

**Batch A — fast self-checking** (20 routines):
zlib · bzip2 · zstd · xxHash · lua · sqlite · crypto-algorithms.

**Batch B — the bulk** (~570 routines):
- **coreutils** (36) — differential: 36 version-stable cases (sort/factor/wc/
  cut/tr/hashes/base64/expr/numfmt/…) diffed vs host golden.
- **LTP syscalls** (508) — each test self-reports via its exit BITMASK; a curated
  SAFE allowlist (`corpus/ltp/allow.txt`) of ~130 syscall families, excluding all
  destructive/global-state tests so nothing can wedge the guest.
- **CPython 3.10.14** (26) — a cross-built interpreter (dynamic, `--disable-shared`,
  CPU-pure stdlib C modules forced builtin via `Modules/Setup.local`) running
  inline assert-based functional tests of the stdlib (json/math/struct/datetime/
  asyncio/hashlib/pickle/re/decimal/codecs/…). NOTE: the full `python -m test`
  regrtest harness needs the cross-generated `_sysconfigdata__linux_aarch64-
  linux-gnu` module, which a plain `make python` does not produce — so we run the
  interpreter against inline oracles instead (which also fits this sweep's
  defined-oracle model). Wiring up regrtest is a follow-up (95emulator has a
  working static-CPython recipe).

Latest full run: **562 PASS / 28 SKIP / 1 FAIL** of 591 routines across 10
codebases, one boot. The lone FAIL is `ltp-epoll_ctl04` (kernel-6.12
epoll-nesting semantics differ
from LTP-20240524's expectation — pure kernel, no i.MX device; a known
upstream-version divergence, not a model bug). `fork13`/`setsockopt06` are named
SKIPs (TCG-impractical: fork-storm / net timing too slow under emulation).

## Gotchas found

- **static linking is unreliable with the Ubuntu aarch64 cross toolchain** and is
  the single biggest trap here. Three distinct failure modes, all fixed by
  building **dynamic** (links the guest's own glibc; requires guest glibc >=
  build glibc — true for this BSP):
  - large static binaries (sqlite3) **SIGSEGV at startup** (static IFUNC/NSS
    fragility) — small ones happen to work, so it's easy to miss;
  - autotools projects (coreutils) **fail to link** with undefined
    `__preinit_array`/`__fini_array`;
  - a big static corpus (LTP, 500+ × ~1MB each) **bloats the initramfs** until the
    kernel panics at `mount_root`. Dynamic drops each to ~20KB.
  Rule of thumb: prefer dynamic; only the smallest leaf programs link static OK.
- **strip staged binaries** — build-corpus.sh strips all staged ELFs (debug_info
  bloats the initramfs).
- block-buffered stdout to a pipe hides partial output before a crash; localize
  with staged probes, not a single end-to-end diff.
- LTP per-test timeout must reach the GUEST (kernel cmdline `LTP_PER=`), not just
  a host env var; default 30s — TCG is slow and sync-loop/timing tests
  false-timeout at 10s.
