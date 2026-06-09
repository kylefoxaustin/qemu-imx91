/*
 * QTest for the i.MX91 FlexIO I2C-master datapath (shifter lock-step).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The register test (imx91-flexio-test) only proves the block identifies and
 * latches config. This test drives the functional I2C-over-shifter datapath the
 * i2c-flexio driver uses: it attaches a tmp105 on the FlexIO I2C bus and clocks
 * a pointer-write + 2-byte register-read through the model exactly the way the
 * driver's interrupt handler does - load shifter 0 (SHIFTBUFBBS_0), let a byte
 * clock out, drain shifter 1 (SHIFTBUFBIS_1) - asserting the temperature bytes
 * come back intact.
 *
 * Each byte clock is one "shift" event the model arms on a virtual-time timer;
 * qtest steps QEMU_CLOCK_VIRTUAL by exactly that period to fire it. Because the
 * test controls the clock explicitly it also pins down the lock-step ordering -
 * the driver loads the next byte and then drains the receive buffer for the
 * current one - which is precisely the ordering the model must honour
 * deterministically rather than by a timing margin.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define FLEXIO_BASE         0x425c0000ULL

#define FLEXIO_SHIFTSTAT    0x10
#define   SHIFTSTAT_TX      0x1
#define   SHIFTSTAT_RX      0x2
#define FLEXIO_SHIFTSIEN    0x20
#define FLEXIO_TIMSTAT      0x18
#define FLEXIO_SHIFTBUFBIS_1 0x284
#define FLEXIO_SHIFTBUFBBS_0 0x380

/* One I2C byte period, per the model's FLEXIO_SHIFT_NS. */
#define SHIFT_NS            100000

#define TMP105_ADDR         0x48
#define TMP105_TEMP_RAW     0x2580     /* 8.8 fixed -> bytes 0x25, 0x80 */
#define TMP105_TEMP_MDEG    37500      /* = 0x2580 * 1000 / 256 */

static void wr(QTestState *q, uint64_t off, uint32_t v)
{
    qtest_writel(q, FLEXIO_BASE + off, v);
}

static uint32_t rd(QTestState *q, uint64_t off)
{
    return qtest_readl(q, FLEXIO_BASE + off);
}

/* Fire one pending shift (one I2C byte clock). */
static void shift(QTestState *q)
{
    qtest_clock_step(q, SHIFT_NS);
}

/*
 * One driver-style byte cycle: load the transmit shifter, clock the byte, then
 * drain the receive shifter - the canonical order the i2c-flexio ISR uses.
 */
static uint8_t byte_cycle(QTestState *q, uint8_t tx)
{
    wr(q, FLEXIO_SHIFTBUFBBS_0, tx);
    shift(q);
    g_assert_cmphex(rd(q, FLEXIO_SHIFTSTAT) & SHIFTSTAT_RX, ==, SHIFTSTAT_RX);
    return rd(q, FLEXIO_SHIFTBUFBIS_1);
}

static void test_flexio_i2c_read(void)
{
    QTestState *q = qtest_init("-machine imx91-11x11-evk -display none "
                               "-device tmp105,bus=flexio1-i2c,"
                               "address=0x48,id=temp0");
    uint8_t msb, lsb;

    /* Pin the sensor to a known temperature so the read is deterministic. */
    qtest_qmp_assert_success(q,
        "{ 'execute': 'qom-set', 'arguments': "
        "{ 'path': '/machine/peripheral/temp0', "
        "'property': 'temperature', 'value': %d } }", TMP105_TEMP_MDEG);

    /* --- Write phase: set the register pointer to 0 (temperature). --- */
    wr(q, FLEXIO_SHIFTSIEN, 3);                 /* arm shifter interrupts */
    byte_cycle(q, (TMP105_ADDR << 1) | 0);      /* address + write */
    byte_cycle(q, 0x00);                        /* pointer = TEMPERATURE */
    wr(q, FLEXIO_SHIFTSIEN, 0);                 /* STOP */
    g_assert_cmphex(rd(q, FLEXIO_TIMSTAT) & 1, ==, 1);   /* transfer done */
    wr(q, FLEXIO_TIMSTAT, 1);

    /* --- Read phase: read the 2-byte temperature register. --- */
    wr(q, FLEXIO_SHIFTSIEN, 3);
    byte_cycle(q, (TMP105_ADDR << 1) | 1);      /* address + read (ACK byte) */
    msb = byte_cycle(q, 0xff);                  /* dummy clock -> temp MSB */
    lsb = byte_cycle(q, 0xff);                  /* dummy clock -> temp LSB */
    wr(q, FLEXIO_SHIFTSIEN, 0);                 /* STOP */

    g_assert_cmphex(msb, ==, (TMP105_TEMP_RAW >> 8) & 0xff);
    g_assert_cmphex(lsb, ==, TMP105_TEMP_RAW & 0xf0);

    qtest_quit(q);
}

/*
 * The lock-step must not depend on *when* a byte clocks relative to the driver
 * draining the previous one. This drives the adversarial ordering: a second
 * byte is clocked (clock stepped) before the first receive byte is read out of
 * SHIFTBUFBIS_1 - exactly what a busy host let the old free-running shift timer
 * do, overwriting the undrained byte and losing an interrupt edge. The datapath
 * must still hand back the bytes in order: the receive buffer cannot advance
 * past a byte the driver has not yet drained.
 */
static void test_flexio_i2c_race(void)
{
    QTestState *q = qtest_init("-machine imx91-11x11-evk -display none "
                               "-device tmp105,bus=flexio1-i2c,"
                               "address=0x48,id=temp0");
    uint8_t msb, lsb;

    qtest_qmp_assert_success(q,
        "{ 'execute': 'qom-set', 'arguments': "
        "{ 'path': '/machine/peripheral/temp0', "
        "'property': 'temperature', 'value': %d } }", TMP105_TEMP_MDEG);

    /* Write phase: set the pointer (ordered - this part isn't under test). */
    wr(q, FLEXIO_SHIFTSIEN, 3);
    byte_cycle(q, (TMP105_ADDR << 1) | 0);
    byte_cycle(q, 0x00);
    wr(q, FLEXIO_SHIFTSIEN, 0);
    wr(q, FLEXIO_TIMSTAT, 1);

    /* Read phase: address, then clock TWO data bytes before draining either. */
    wr(q, FLEXIO_SHIFTSIEN, 3);
    byte_cycle(q, (TMP105_ADDR << 1) | 1);      /* address + read (ACK) */

    wr(q, FLEXIO_SHIFTBUFBBS_0, 0xff);          /* load + clock byte 0 (MSB) */
    shift(q);
    wr(q, FLEXIO_SHIFTBUFBBS_0, 0xff);          /* race: clock byte 1 early */
    shift(q);

    msb = rd(q, FLEXIO_SHIFTBUFBIS_1);          /* must still be the MSB */
    lsb = rd(q, FLEXIO_SHIFTBUFBIS_1);          /* then the LSB, in order */
    wr(q, FLEXIO_SHIFTSIEN, 0);

    g_assert_cmphex(msb, ==, (TMP105_TEMP_RAW >> 8) & 0xff);
    g_assert_cmphex(lsb, ==, TMP105_TEMP_RAW & 0xf0);

    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/flexio/i2c-datapath", test_flexio_i2c_read);
    qtest_add_func("/imx91/flexio/i2c-shift-ordering", test_flexio_i2c_race);
    return g_test_run();
}
