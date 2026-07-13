#!/usr/bin/env bash
#
# Does the timer FOLLOW THE CLOCK TREE?
#
# This is the test the old model could not have passed, and could not have failed
# either -- because it could not have been written. The TPM hardcoded
#
#     #define TPM_CLK_HZ  24000000    /* nominal module clock */
#
# with no clock input at all, and the CCM produced no frequencies, so Linux
# computed 24 MHz for everything. The two agreed -- and they agreed because BOTH
# WERE FABRICATED, not because either was right. Every timer test stayed green.
#
#     THE SYMPTOM OF A WRONG CLOCK IS SPEED, NOT WRONGNESS, AND NO CORRECTNESS
#     CHECK WILL EVER SEE IT.                              -- ollama_95_neutron
#
# So this test does not count interrupts and it does not read the model's own
# constants. It PROGRAMS the clock root the way firmware does, then measures how
# far the counter actually advanced over a known interval of virtual time, and
# compares against the frequency the TREE says that root is running at.
#
#     expected = root_hz * step_ns / 1e9,  modulo MOD+1
#
# A model that ignores the tree reports THE SAME COUNT AT EVERY DIVIDER. That is
# the signature this sweeps for, and it is exactly the mutation that shipped in
# rt1180emulator's PWM golden for months.
#
# The two zero rows are the point, not padding: a root that is gated OFF, and a
# root muxed to a source nobody drives, must produce NO CLOCK -- and a consumer
# with no clock MUST NOT TICK. A stopped clock gets diagnosed in a minute; a
# plausible clock ships into somebody's product.
#
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}

if [ ! -x "$QEMU" ]; then
    echo "SKIP: qemu not built at $QEMU"
    exit 0
fi

CCM_TPM2_ROOT=0x44450880    # CLOCK_ROOT17_CONTROL (tpm2_root), RM §7
TPM2=0x44320000             # SC @ +0x10, CNT @ +0x14, MOD @ +0x18
STEP_NS=1000000             # 1 ms of virtual time
MODULO=65536                # MOD = 0xffff  =>  period = MOD + 1

# TPM_SEL parents (clk-imx93.c parent_names[TPM_SEL]):
#   MUX 0 = osc_24m (24 MHz)   1 = sys_pll_pfd0 (1 GHz)
#   MUX 2 = audio_pll (off)    3 = clk_ext1 (undriven on this board)
#
# name | CONTROL | root_hz  (pipe-separated: the names contain spaces)
CASES=(
    "osc_24m / 1|0x00000000|24000000"
    "osc_24m / 4|0x00000003|6000000"
    "sys_pll_pfd0 / 1|0x00000100|1000000000"
    "sys_pll_pfd0 / 10|0x00000109|100000000"
    "sys_pll_pfd0 / 25|0x00000118|40000000"
    "root gated OFF|0x01000000|0"
    "clk_ext1 (undriven)|0x00000300|0"
    "audio_pll (unpowered)|0x00000200|0"
)

fail=0

# ONE subject for every case, under a hard kill-timeout.
#
# There is no `quit` in the qtest protocol -- QEMU never exits, so THE HARNESS must
# end it (mcxn947qemu). Spawning a fresh QEMU per case meant each one sat until its
# own timeout fired, and the suite took longer than the suite was allowed to take.
#
# Re-arming per case: writing SC=0 then SC=CMOD restarts the counter's timebase, so
# each measurement starts from zero even though virtual time keeps accumulating.
q=""
n=0
for c in "${CASES[@]}"; do
    IFS='|' read -r name ctrl hz <<<"$c"
    q="$q$(printf 'writel %s %s\nwritel 0x%x 0x0000ffff\nwritel 0x%x 0x00000000\nwritel 0x%x 0x00000008\nclock_step %d\nreadl 0x%x\n' \
             "$CCM_TPM2_ROOT" "$ctrl" $((TPM2 + 0x18)) $((TPM2 + 0x10)) $((TPM2 + 0x10)) \
             "$STEP_NS" $((TPM2 + 0x14)))"$'\n'
    n=$((n + 1))
done

# ⭐ The gate calls the subject, so the subject can wedge the gate. Hard kill, and
#    the answer-count assertion below turns a kill into a FAILED VERDICT rather
#    than a silent short read.  (ollama_95_neutron)
mapfile -t answers < <( { printf '%b' "$q" \
    | timeout -s KILL 60 "$QEMU" -M imx91-11x11-evk -display none \
        -accel qtest -qtest stdio -monitor none -serial none 2>/dev/null \
    | grep '^OK 0x' | awk '{print strtonum($2)}'; } 2>/dev/null )

# In the qtest protocol a `writel` answers a bare "OK" and `clock_step` answers a
# DECIMAL "OK <ns>" -- only `readl` answers "OK 0x...".  So the grep above yields
# exactly one line per case: the CNT read.  (Assumed a fixed stride here at first,
# and threw away seven of the eight answers.  Measure the protocol; do not guess it.)
cnt=("${answers[@]}")

if [ "${#cnt[@]}" -ne "$n" ]; then
    echo "FAIL: asked $n questions, got ${#cnt[@]} answers."
    echo "      A truncated conversation and a correct one differ only in the"
    echo "      answers you never notice are missing."
    exit 1
fi

seen=""
i=0
for c in "${CASES[@]}"; do
    IFS='|' read -r name ctrl hz <<<"$c"
    got=${cnt[$i]}
    i=$((i + 1))

    # The golden comes from the CLOCK TREE'S ARITHMETIC, not from the model:
    #
    #     ticks = root_hz * step_ns / 1e9,  taken modulo (MOD + 1)
    #
    # (Written as hz / (1e9 / step_ns) to stay in 64-bit integers. This arithmetic
    # was WRONG on its first run -- off by 1000x -- and THE MODEL WAS RIGHT. The
    # gate caught a bug in the gate, which is the only reason to trust it now.)
    want=$(( hz / (1000000000 / STEP_NS) % MODULO ))

    if [ "$got" -eq "$want" ]; then
        printf "  ok    %-22s root=%11s Hz  CNT=%6d\n" "$name" "$hz" "$got"
    else
        printf "  FAIL  %-22s root=%11s Hz  CNT=%6d  expected %6d\n" \
               "$name" "$hz" "$got" "$want"
        fail=1
    fi
    seen="$seen $got"
done

# ⭐ THE CONSTANT-COUNT SIGNATURE.
#
# A model that ignores the clock tree does not merely get one row wrong -- it gets
# THE SAME ANSWER FOR EVERY ROW, because its counter runs at a constant. That is
# what the old model did, and no per-row assertion would have looked suspicious on
# its own. Check the shape, not just the values.
distinct=$(echo $seen | tr ' ' '\n' | sort -u | wc -l)
if [ "$distinct" -lt 2 ]; then
    echo "FAIL: every case returned the same count -- the timer is NOT following"
    echo "      the clock tree.  This is the constant-period signature of a model"
    echo "      whose counter runs at a hardcoded frequency."
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo
    echo "PASS: the TPM counter follows the CCM clock tree across mux, divider and"
    echo "      gate -- and STOPS when the tree gives it no clock."
else
    echo
    echo "FAIL: the timer does not follow the clock tree."
fi
exit $fail
