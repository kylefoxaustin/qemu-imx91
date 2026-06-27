#!/bin/bash
# Host-side corpus builder for the i.MX 91 code sweep.
#
# Walks corpus/<name>/build.sh, cross-compiles each item into a staging tree
# ($STAGE/corpus/<name>/) alongside its run.sh, and caches downloads. A build
# that fails is NOT silently dropped: we stage a run.sh that emits
# SOAK:SKIP:<name>:build-failed so the scoreboard stays honest (build failure is
# a SKIP, distinct from a runtime FAIL).
#
#   build-corpus.sh <stage-dir> [name ...]   (no names => whole corpus)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
STAGE=${1:?usage: build-corpus.sh <stage-dir> [name ...]}; shift || true
CROSS=${CROSS:-aarch64-linux-gnu-}
DL=${DL:-$HERE/.dlcache}
BUILDROOT=$(mktemp -d)
mkdir -p "$DL" "$STAGE/corpus" "$STAGE/bin"
export CROSS DL

command -v "${CROSS}gcc" >/dev/null || { echo "error: ${CROSS}gcc not found" >&2; exit 1; }

ITEMS=("$@")
if [ "${#ITEMS[@]}" -eq 0 ]; then
  ITEMS=()
  for d in "$HERE"/corpus/*/; do ITEMS+=("$(basename "$d")"); done
fi

built=0; failed=0
for name in "${ITEMS[@]}"; do
  bdir="$HERE/corpus/$name"
  [ -f "$bdir/build.sh" ] || { echo "skip $name (no build.sh)"; continue; }
  S="$STAGE/corpus/$name"; mkdir -p "$S"
  echo "=== build $name ==="
  work="$BUILDROOT/$name"; mkdir -p "$work"
  if ( cd "$work" && STAGE="$S" bash "$bdir/build.sh" ) >"$S/.build.log" 2>&1; then
    # stage run.sh + any static assets the item ships (selftest scripts, *.sql,
    # golden files committed alongside build.sh) — everything but build.sh.
    for asset in "$bdir"/*; do
      case "$(basename "$asset")" in build.sh) continue ;; esac
      cp "$asset" "$S/"
    done
    # strip staged ELF binaries — cross test binaries are static + carry
    # debug_info; unstripped, a big corpus (e.g. LTP's ~500 tests) bloats the
    # initramfs past what the kernel can load and boot panics at mount_root.
    find "$S" -type f -perm -u+x ! -name '*.sh' 2>/dev/null | while read -r elf; do
      head -c4 "$elf" 2>/dev/null | grep -q $'\x7fELF' && "${CROSS}strip" -s "$elf" 2>/dev/null || true
    done
    built=$((built+1)); echo "    ok"
  else
    failed=$((failed+1))
    echo "    BUILD FAILED (see $S/.build.log); staging a SKIP marker"
    tail -3 "$S/.build.log" | sed 's/^/    | /'
    printf '#!/bin/sh\necho "SOAK:SKIP:%s:build-failed"\n' "$name" > "$S/run.sh"
  fi
done

rm -rf "$BUILDROOT"
echo "corpus: $built built, $failed build-failed -> $STAGE/corpus"
