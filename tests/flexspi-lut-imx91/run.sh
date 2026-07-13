#!/usr/bin/env bash
#
# FIXING A RESET VALUE MUST NOT QUIETLY RELAX A GUARDRAIL.
#
# FlexSPI's LUTKEY resets to the key itself (5AF05AF0h) -- the reset-value gate said
# so, and it was right. But the model's LUT unlock was
#
#     if (LUTCR == 2 && regs[LUTKEY] == KEY)  unlock;
#
# so seeding LUTKEY with its REAL reset value would have made that test pass WITHOUT
# THE GUEST EVER WRITING THE KEY. The model would unlock its LUT on a bare LUTCR
# write, and firmware that forgot the key sequence would PASS HERE and be undefined
# on silicon.
#
#     A MODEL THAT IS TOO FORGIVING DOES NOT FAIL SAFE. IT SHIPS THE BUG DOWNSTREAM.
#     A stopped clock gets diagnosed in a minute; a plausible one ships.
#                                                            -- mcxn947qemu
#
# So the unlock now requires the key to have been WRITTEN, and this test holds BOTH
# properties at once: the register reads its true reset value, AND the guardrail
# still refuses a guest that skips the key.
#
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}

[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

FSPI=0x425e0000
LUTKEY=$((FSPI + 0x18))
LUTCR=$((FSPI + 0x1c))
LUT0=$((FSPI + 0x200))
KEY=0x5af05af0

ask() {
    printf '%b' "$1" \
        | timeout -s KILL 20 "$QEMU" -M imx91-11x11-evk -display none \
            -accel qtest -qtest stdio -monitor none -serial none 2>/dev/null \
        | grep '^OK 0x' | tail -1 | awk '{print strtonum($2)}'
}

fail=0
chk() { # name got want
    if [ "$2" = "$3" ]; then
        printf "  ok    %s\n" "$1"
    else
        printf "  FAIL  %-52s got 0x%08x want 0x%08x\n" "$1" "$2" "$3"
        fail=1
    fi
}

# 1. The guardrail: LUTCR=2 with NO key written must NOT unlock the LUT.
got=$(ask "writel $LUTCR 0x00000002\nwritel $LUT0 0xdeadbeef\nreadl $LUT0\n")
chk "LUT stays LOCKED when the guest skips the key" "$got" 0

# 2. The real sequence: key, then unlock.
got=$(ask "writel $LUTKEY $KEY\nwritel $LUTCR 0x00000002\nwritel $LUT0 0xdeadbeef\nreadl $LUT0\n")
chk "LUT unlocks after the key IS written" "$got" $((0xdeadbeef))

# 3. And the register still reads its true reset value (the thing that made 1 hard).
got=$(ask "readl $LUTKEY\n")
chk "LUTKEY reads its RM reset value (5AF05AF0h)" "$got" $((KEY))

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: LUTKEY reports the silicon's reset value AND the unlock still refuses"
    echo "      a guest that never wrote the key."
else
    echo "FAIL: the reset value and the guardrail are not both intact."
fi
exit $fail
