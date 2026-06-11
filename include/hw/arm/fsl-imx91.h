/*
 * NXP i.MX 91 SoC definitions
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * Modeled on hw/arm/fsl-imx8mp.h (Bernhard Beschow) and the i.MX 95 port.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Unlike the i.MX 95, the i.MX 91 has NO System Manager: Linux programs the
 * CCM / ANATOP / IOMUXC / SRC blocks directly (no SCMI/SM indirection), so
 * those regions must eventually be modeled functionally rather than served
 * by an SM firmware stub. v0.0.1 installs logging stubs for them.
 *
 * All base addresses and IRQ numbers below are taken from the i.MX 91 Linux
 * device tree (arch/arm64/boot/dts/freescale/imx91.dtsi) and cross-checked
 * against the i.MX 91 Reference Manual (IMX93RM).
 */

#ifndef FSL_IMX91_H
#define FSL_IMX91_H

#include "target/arm/cpu.h"
#include "hw/core/clock.h"
#include "qemu/notify.h"
#include "hw/char/imx_lpuart.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/imx93_ccm.h"
#include "hw/misc/imx93_anatop.h"
#include "hw/misc/imx93_ele.h"
#include "hw/net/imx_fec.h"
#include "hw/net/imx93_dwmac.h"
#include "hw/net/flexcan.h"
#include "net/can_emu.h"
#include "hw/i2c/imx_lpi2c.h"
#include "hw/gpio/imx93_gpio.h"
#include "hw/display/imx93_lcdif.h"
#include "hw/display/imx93_isi.h"
#include "hw/misc/imx93_flexio.h"
#include "hw/misc/imx93_media_blk.h"
#include "hw/misc/imx_mu.h"
#include "hw/rtc/imx93_bbnsm.h"
#include "hw/watchdog/imx93_wdog.h"
#include "hw/misc/imx93_tmu.h"
#include "hw/adc/imx93_adc.h"
#include "hw/ssi/imx93_lpspi.h"
#include "hw/nvram/imx93_ocotp.h"
#include "hw/timer/imx93_sysctr.h"
#include "hw/timer/imx93_tpm.h"
#include "hw/timer/imx93_tstmr.h"
#include "hw/misc/imx93_sema42.h"
#include "hw/ssi/imx93_flexspi.h"
#include "hw/audio/imx93_xcvr.h"
#include "hw/dma/imx93_edma.h"
#include "hw/usb/chipidea.h"
#include "hw/audio/imx93_sai.h"
#include "hw/audio/imx93_micfil.h"
#include "hw/i3c/svc_i3c.h"
#include "hw/misc/imx9_ddrc.h"
#include "hw/sd/sdhci.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_FSL_IMX91 "fsl-imx91"
OBJECT_DECLARE_SIMPLE_TYPE(FslImx91State, FSL_IMX91)

/*
 * Main DDR window. The i.MX 91 maps DRAM starting at 0x8000_0000. The
 * 11x11 EVK ships with 2 GiB LPDDR4X; allow up to 4 GiB for the larger
 * board variants.
 */
#define FSL_IMX91_RAM_START         0x80000000ULL
#define FSL_IMX91_FLEXSPI_AHB_ADDR  0x28000000ULL  /* memory-mapped NOR (XIP) */
#define FSL_IMX91_RAM_SIZE_MAX      (4ULL * GiB)

/*
 * i.MX 91 application processor complex:
 *   - 1x Cortex-A55  (the i.MX 91 is single-core; the i.MX 93's 2nd A55 and
 *     Cortex-M33 are both absent - AN14012/AN14561).
 */
enum FslImx91Configuration {
    FSL_IMX91_NUM_A55_CPUS  = 1,
    FSL_IMX91_NUM_LPUARTS   = 8,    /* LPUART1..LPUART8 */
    FSL_IMX91_NUM_IRQS      = 320,  /* GICv3 SPI budget */
};

/*
 * All eight LPUART instances are modeled. LPUART1 is the 11x11 EVK console;
 * the board also enables LPUART5, and modeling the full set means any stock
 * board DT probes cleanly instead of taking an external abort on an unmapped
 * instance.
 */
#define FSL_IMX91_NUM_MODELED_LPUARTS   8

/* uSDHC controllers modeled with the real imx-usdhc device. */
#define FSL_IMX91_NUM_USDHCS            3

/* GPIO banks (gpio1..gpio4). */
#define FSL_IMX91_NUM_GPIOS            4

/* FlexCAN controllers (flexcan1, flexcan2). */
#define FSL_IMX91_NUM_FLEXCAN         2

/* USB OTG controllers (ChipIdea), usbotg1/usbotg2. */
#define FSL_IMX91_NUM_USBS            2

/* SAI audio interfaces (sai1, sai2, sai3). */
#define FSL_IMX91_NUM_SAIS            3

struct FslImx91State {
    SysBusDevice    parent_obj;

    ARMCPU          cpu[FSL_IMX91_NUM_A55_CPUS];
    GICv3State      gic;

    IMXMUState      mu1;                  /* MU1 @ 0x44230000 (A55-side MU)   */
    IMX93BbnsmState bbnsm;               /* BBNSM RTC @ 0x44440000          */
    IMX93WdogState wdog[5];              /* WDOG1-5                         */
    IMX93TmuState tmu;                   /* thermal monitor @ 0x44482000    */
    IMX93AdcState adc1;                  /* SAR-ADC @ 0x44530000            */
    IMX93LpspiState lpspi[8];            /* LPSPI1-8                        */
    IMX93OcotpState ocotp;               /* OCOTP fuses @ 0x47510000        */
    IMX93SysctrState sysctr;             /* system counter @ 0x44290000     */
    IMX93TpmState tpm[6];                /* TPM1-6 (timer/PWM)              */
    IMXMUState mu2;                       /* MU2 @ 0x42440000                */
    IMX93TstmrState tstmr[2];            /* TSTMR1/2 timestamp timers       */
    IMX93Sema42State sema42[2];          /* SEMA42 (AON + WAKEUP)           */
    IMX93FlexSpiState flexspi;           /* FlexSPI @ 0x425e0000 (NOR flash) */
    IMX93XcvrState xcvr;                 /* SPDIF transceiver @ 0x42680000  */
    IMXLPUARTState  lpuart[FSL_IMX91_NUM_MODELED_LPUARTS];
    IMX93CCMState   ccm;
    IMX93AnatopState anatop;
    IMX93EleState   ele;
    SDHCIState      usdhc[FSL_IMX91_NUM_USDHCS];
    IMXFECState     fec;
    IMX93DwmacState eqos;
    IMX93EdmaState  edma1;
    IMX93EdmaState  edma2;
    IMXLPI2CState   lpi2c1;
    IMXLPI2CState   lpi2c2;
    IMXLPI2CState   lpi2c8;
    IMXLPI2CState   lpi2c_exp[5];        /* LPI2C3-7: open expansion buses  */
    IMX93FlexioState flexio1;            /* FlexIO1 (configurable I/O / I2C) */
    IMX93GPIOState  gpio[FSL_IMX91_NUM_GPIOS];
    IMX93MediaBlkCtrlState media_blk_ctrl;
    IMX93SrcSliceState     mediamix;
    IMX93LcdifState lcdif;
    IMX93IsiState   isi;
    FlexCanState    flexcan[FSL_IMX91_NUM_FLEXCAN];
    CanBusState     *canbus[FSL_IMX91_NUM_FLEXCAN];
    ChipideaState   usb[FSL_IMX91_NUM_USBS];
    IMX93SaiState   sai[FSL_IMX91_NUM_SAIS];
    IMX93MicfilState micfil;
    SvcI3cState     i3c1;                 /* Silvaco I3C master @ 0x44330000  */
    Imx9DdrcState   ddrc;                 /* DDR controller + PMU @0x4e300000 */
    MemoryRegion    ocram;
    char            *flexspi_flash;      /* SSI device on the FlexSPI bus    */
};

/*
 * Memory map region identifiers. The actual addresses live in the memmap
 * table in fsl-imx91.c.
 */
enum FslImx91MemoryRegions {
    FSL_IMX91_RAM,

    /* GICv3 (i.MX 91 has no ITS in the base SoC) */
    FSL_IMX91_GIC_DIST,
    FSL_IMX91_GIC_REDIST,

    /* On-chip RAM */
    FSL_IMX91_OCRAM,

    /* LPUART block (AON + Wakeup domains); all 8 instances modeled */
    FSL_IMX91_LPUART1,
    FSL_IMX91_LPUART2,
    FSL_IMX91_LPUART3,
    FSL_IMX91_LPUART4,
    FSL_IMX91_LPUART5,
    FSL_IMX91_LPUART6,
    FSL_IMX91_LPUART7,
    FSL_IMX91_LPUART8,

    /* Clock / reset / pinmux infrastructure (stubbed as unimplemented) */
    FSL_IMX91_CCM,
    FSL_IMX91_ANATOP,
    FSL_IMX91_IOMUXC,
    FSL_IMX91_SRC,

    /* BLK_CTRL / syscfg aggregates per power domain (stubbed) */
    FSL_IMX91_BLK_CTRL_AONMIX,
    FSL_IMX91_BLK_CTRL_WAKEUPMIX,
    FSL_IMX91_BLK_CTRL_DDRMIX,

    /* Ethernet: FEC (modeled) + eQOS dwmac (stubbed - no upstream model) */
    FSL_IMX91_FEC,
    FSL_IMX91_EQOS,

    /* eDMA controllers (edma1 AONMIX, edma2 WAKEUPMIX) */
    FSL_IMX91_EDMA1,
    FSL_IMX91_EDMA2,

    /*
     * OCOTP/efuse syscon: provides the FEC MAC-address nvmem cells. Mapping
     * it (reads 0 -> zero MAC -> FEC falls back to a random MAC) lets the
     * ethernet drivers bind instead of deferring on a missing MAC supplier.
     */
    FSL_IMX91_OCOTP,

    /*
     * Messaging Units: MU1 (AONMIX), MU2 (WAKEUPMIX), and the ELE/Sentinel
     * S4 MU. All enabled on the 11x11 EVK; unmapped MMIO here faults the
     * imx-mailbox driver probe with a synchronous external abort.
     */
    FSL_IMX91_MU1,
    FSL_IMX91_MU2,
    FSL_IMX91_ELE_MU,

    /* System counter */
    FSL_IMX91_SYSCTR,

    /*
     * Watchdogs: WDOG1/2 in AONMIX, WDOG3/4/5 in WAKEUPMIX. The 11x11 EVK
     * enables wdog3 (0x42490000); the rest are mapped for completeness.
     */
    FSL_IMX91_WDOG1,
    FSL_IMX91_WDOG2,
    FSL_IMX91_WDOG3,
    FSL_IMX91_WDOG4,
    FSL_IMX91_WDOG5,

    /* Trusted Resource Domain Controller (stubbed) */
    FSL_IMX91_TRDC,

    /* Battery-Backed Non-Secure Module (RTC + power key), syscon */
    FSL_IMX91_BBNSM,

    /* Thermal Management Unit (qoriq-tmu) */
    FSL_IMX91_TMU,

    /* ADC */
    FSL_IMX91_ADC1,

    /* uSDHC (eMMC / SD / SDIO) */
    FSL_IMX91_USDHC1,
    FSL_IMX91_USDHC2,
    FSL_IMX91_USDHC3,

    /*
     * Low-speed I/O controllers. All stubbed: they are board-enabled so an
     * unmapped instance aborts its (often deferred) probe, which blocks
     * wait_for_device_probe() and stalls the boot before init runs.
     */
    FSL_IMX91_TPM1, FSL_IMX91_TPM2, FSL_IMX91_TPM3,
    FSL_IMX91_TPM4, FSL_IMX91_TPM5, FSL_IMX91_TPM6,
    FSL_IMX91_I3C1, FSL_IMX91_I3C2,
    FSL_IMX91_LPI2C1, FSL_IMX91_LPI2C2, FSL_IMX91_LPI2C3, FSL_IMX91_LPI2C4,
    FSL_IMX91_LPI2C5, FSL_IMX91_LPI2C6, FSL_IMX91_LPI2C7, FSL_IMX91_LPI2C8,
    FSL_IMX91_LPSPI1, FSL_IMX91_LPSPI2, FSL_IMX91_LPSPI3, FSL_IMX91_LPSPI4,
    FSL_IMX91_LPSPI5, FSL_IMX91_LPSPI6, FSL_IMX91_LPSPI7, FSL_IMX91_LPSPI8,
    FSL_IMX91_FLEXCAN1, FSL_IMX91_FLEXCAN2,
    FSL_IMX91_SAI1, FSL_IMX91_SAI2, FSL_IMX91_SAI3,
    FSL_IMX91_MICFIL, FSL_IMX91_FLEXSPI1, FSL_IMX91_XCVR,
    FSL_IMX91_GPIO1, FSL_IMX91_GPIO2, FSL_IMX91_GPIO3, FSL_IMX91_GPIO4,
    FSL_IMX91_USBOTG1, FSL_IMX91_USBOTG2,

    /* MEDIAMIX: block control + imaging cluster */
    FSL_IMX91_MEDIAMIX_PD,      /* SRC power-domain slice (overlays SRC) */
    FSL_IMX91_MEDIA_BLK_CTRL,
    FSL_IMX91_LCDIF,
    FSL_IMX91_ISI,

    /* Group A: blocks not otherwise in the map (TSTMR/SEMA42/FLEXIO) */
    FSL_IMX91_TSTMR1,
    FSL_IMX91_TSTMR2,
    FSL_IMX91_SEMA42_1,
    FSL_IMX91_SEMA42_2,
    FSL_IMX91_FLEXIO1,
    FSL_IMX91_FLEXIO2,
    FSL_IMX91_DDRC,

    FSL_IMX91_NUM_REGIONS,
};

/*
 * IRQ assignments (GIC SPI numbers, from imx91.dtsi). These are the
 * interrupt numbers as the device tree presents them; the model adds
 * GIC_INTERNAL internally.
 */
enum FslImx91Irqs {
    FSL_IMX91_I3C1_IRQ      = 12,
    FSL_IMX91_DDRC_PMU_IRQ  = 90,
    FSL_IMX91_LPUART1_IRQ   = 19,
    FSL_IMX91_LPUART2_IRQ   = 20,
    FSL_IMX91_LPUART3_IRQ   = 68,
    FSL_IMX91_LPUART4_IRQ   = 69,
    FSL_IMX91_LPUART5_IRQ   = 70,
    FSL_IMX91_LPUART6_IRQ   = 71,
    FSL_IMX91_LPUART7_IRQ   = 210,
    FSL_IMX91_LPUART8_IRQ   = 211,
    FSL_IMX91_USDHC1_IRQ    = 86,
    FSL_IMX91_USDHC2_IRQ    = 87,
    FSL_IMX91_USDHC3_IRQ    = 205,
    FSL_IMX91_FEC_IRQ       = 179,  /* FEC MAC (int0) */
    FSL_IMX91_FEC_TIMER_IRQ = 182,  /* FEC 1588 timer */
    FSL_IMX91_EQOS_IRQ      = 184,
    FSL_IMX91_BBNSM_IRQ     = 73,    /* BBNSM RTC alarm -> A55 GIC SPI */
    FSL_IMX91_TMU_IRQ       = 83,    /* TMU temp alarm */
    FSL_IMX91_ADC1_IRQ      = 219,   /* SAR-ADC conversion (driver irq idx 2) */
    FSL_IMX91_ISI_IRQ       = 172,   /* WAKEUPMIX ISI interrupt */
    FSL_IMX91_SYSCTR_IRQ    = 74,    /* system counter compare */
    FSL_IMX91_MU2_IRQ       = 23,    /* MU2 -> A55 GIC SPI */
    FSL_IMX91_FLEXSPI1_IRQ  = 55,    /* FlexSPI */
    FSL_IMX91_XCVR_IRQ      = 203,   /* XCVR SPDIF */
    FSL_IMX91_ELE_TX_IRQ    = 31,   /* s4muap "tx" */
    FSL_IMX91_ELE_RX_IRQ    = 30,   /* s4muap "rx" */
    FSL_IMX91_MU1_IRQ       = 22,   /* MU1 -> A55 GIC SPI */
    FSL_IMX91_LPI2C1_IRQ    = 13,
    FSL_IMX91_LPI2C2_IRQ    = 14,
    FSL_IMX91_LPI2C3_IRQ    = 62,
    FSL_IMX91_LPI2C4_IRQ    = 63,
    FSL_IMX91_LPI2C5_IRQ    = 195,
    FSL_IMX91_LPI2C6_IRQ    = 196,
    FSL_IMX91_LPI2C7_IRQ    = 197,
    FSL_IMX91_LPI2C8_IRQ    = 198,
    FSL_IMX91_FLEXIO1_IRQ   = 53,
    FSL_IMX91_FLEXCAN1_IRQ  = 8,
    FSL_IMX91_FLEXCAN2_IRQ  = 51,
    FSL_IMX91_USB1_IRQ      = 187,
    FSL_IMX91_USB2_IRQ      = 188,
    FSL_IMX91_SAI1_IRQ      = 45,
    FSL_IMX91_SAI2_IRQ      = 170,
    FSL_IMX91_SAI3_IRQ      = 171,
    /* MICFIL has four lines (error, stream, VAD events). */
    FSL_IMX91_MICFIL_IRQ0   = 202,
    FSL_IMX91_MICFIL_IRQ1   = 201,
    FSL_IMX91_MICFIL_IRQ2   = 200,
    FSL_IMX91_MICFIL_IRQ3   = 199,
    /* eDMA1: channel N raises GIC SPI (EDMA1_IRQ_BASE + N). */
    FSL_IMX91_EDMA1_IRQ_BASE = 95,
    FSL_IMX91_EDMA1_CHANNELS = 31,
    /* eDMA2 (edma4): 64 channels, paired - channel N -> GIC SPI (128 + N/2). */
    FSL_IMX91_EDMA2_IRQ_BASE = 128,
    FSL_IMX91_EDMA2_CHANNELS = 64,
    FSL_IMX91_EDMA2_CHAN_STRIDE = 0x8000,
    FSL_IMX91_LCDIF_IRQ     = 176,
    /* GPIO banks: each has two GIC lines (the driver uses the first). */
    FSL_IMX91_GPIO1_IRQ     = 10,
    FSL_IMX91_GPIO1_IRQ_HI  = 11,
    FSL_IMX91_GPIO2_IRQ     = 57,
    FSL_IMX91_GPIO2_IRQ_HI  = 58,
    FSL_IMX91_GPIO3_IRQ     = 59,
    FSL_IMX91_GPIO3_IRQ_HI  = 60,
    FSL_IMX91_GPIO4_IRQ     = 189,
    FSL_IMX91_GPIO4_IRQ_HI  = 190,
};

/* Trivial register-file I2C slave used for the board's PMIC + GPIO expander. */
#define TYPE_IMX93_I2C_REGDEV   "imx93.i2c-regdev"

/* MT9M114 camera sensor I2C slave (mt9m114 device-tree variant). */
#define TYPE_MT9M114            "mt9m114"

/* I2C addresses on lpi2c2: PMIC + GPIO expander. */
#define FSL_IMX91_PCA9451_ADDR  0x25
#define FSL_IMX91_PCA9451_DEVID 0x90    /* DEV_ID high nibble 0x9 = pca9451a */
#define FSL_IMX91_PCAL6524_ADDR 0x22
/*
 * ADP5585 I/O expander (io-expander@34) on lpi2c2. Its adp5585 MFD driver only
 * checks the ID register, then registers the GPIO sub-device whose hogs gate
 * the board's audio-power and CAN2-standby regulators - so without it those
 * regulators (and their consumers) sit in deferred probe. (On the i.MX 93 it
 * also drove the LVDS panel backlight; the i.MX 91 has no LVDS, but the
 * audio/CAN rails still depend on it.)
 */
#define FSL_IMX91_ADP5585_ADDR  0x34
#define FSL_IMX91_ADP5585_ID    0x20    /* ADP5585_MAN_ID_VALUE, bits [7:4] */

/* FEC RGMII PHY MDIO address on the 11x11 EVK (ethphy2, reg = <2>). */
#define FSL_IMX91_FEC_PHY_NUM   2

/*
 * virtio-mmio transports. Not present on real i.MX91 silicon; we add a few
 * slots in an unused hole of the memory map and inject matching device-tree
 * nodes (see imx93-evk.c) so a guest can bind e.g. a virtio-keyboard, giving
 * the emulated HDMI/LCDIF console real keyboard input. SPIs 230.. are unused
 * by the SoC; the GIC is configured with 320 SPIs.
 */
#define FSL_IMX91_VIRTIO_MMIO_BASE  0x70000000
#define FSL_IMX91_VIRTIO_MMIO_SIZE  0x200
#define FSL_IMX91_NUM_VIRTIO_MMIO   4
#define FSL_IMX91_VIRTIO_MMIO_IRQ   230     /* first SPI; one per transport */

/* WM8962 audio codec on LPI2C1. */
#define FSL_IMX91_WM8962_ADDR           0x1a



/* Camera (mt9m114 device-tree variant) on LPI2C8. */
#define FSL_IMX91_MT9M114_ADDR          0x48
#define FSL_IMX91_PCA9538_ADDR          0x70

#endif /* FSL_IMX91_H */
