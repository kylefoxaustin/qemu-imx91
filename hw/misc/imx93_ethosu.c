/*
 * NXP i.MX 93 Arm Ethos-U65 microNPU (register-file bring-up model)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the Ethos-U65 NPU APB registers the NXP M33 ethos firmware reads
 * during device init (ethosu_dev_init / ethosu_dev_soft_reset), so the firmware
 * accepts the part, resets it, and brings up the rpmsg-ethosu-channel. The
 * register contract was read out of the firmware's own disassembly:
 *   - NPU_REG_CONFIG[31:28] (product) must be 1 (Ethos-U65);
 *   - NPU_REG_STATUS bit 3 must read clear (reset complete / not faulted);
 *   - NPU_REG_PROT must reflect a secure + privileged master (bit0=priv,
 *     bit1=non-secure=0), which verify_access_state checks;
 *   - NPU_REG_ID is reported as capabilities (arch/version), not gated.
 * The command-stream processor (real inference) is intentionally not modelled.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_ethosu.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/cutils.h"
#include "exec/cpu-common.h"

#define ETHOSU_REG_ID       0x00
#define ETHOSU_REG_STATUS   0x04
#define ETHOSU_REG_CMD      0x08
#define ETHOSU_REG_RESET    0x0c
#define ETHOSU_REG_QBASE    0x10
#define ETHOSU_REG_QBASE_HI 0x14
#define ETHOSU_REG_QREAD    0x18
#define ETHOSU_REG_QCONFIG  0x1c
#define ETHOSU_REG_QSIZE    0x20
#define ETHOSU_REG_PROT     0x24
#define ETHOSU_REG_CONFIG   0x28
#define ETHOSU_REG_REGIONCFG 0x3c
#define ETHOSU_REG_BASEP0   0x80     /* BASEP0..7: eight 64-bit region bases */

/* CMD register bits (Arm Ethos-U core driver). */
#define ETHOSU_CMD_TRANSITION_TO_RUNNING (1u << 0)
#define ETHOSU_CMD_CLEAR_IRQ             (1u << 1)

/* STATUS register bits (Arm Ethos-U core driver). */
#define ETHOSU_STATUS_STATE_RUNNING      (1u << 0)
#define ETHOSU_STATUS_IRQ_RAISED         (1u << 1)
#define ETHOSU_STATUS_CMD_END_REACHED    (1u << 5)

static uint32_t ethosu_cfg_u32(const char *name, uint32_t def)
{
    const char *e = getenv(name);
    unsigned long v;

    if (e && *e && qemu_strtoul(e, NULL, 0, &v) == 0) {
        return (uint32_t)v;
    }
    return def;
}

/* Instrumentation: ETHOSU_TRACE=1 dumps register + command-stream traffic. */
static bool ethosu_trace(void)
{
    static int t = -1;
    if (t < 0) {
        const char *e = getenv("ETHOSU_TRACE");
        t = (e && *e && *e != '0') ? 1 : 0;
    }
    return t;
}

static void ethosu_dump_submission(IMX93EthosuState *s)
{
    uint64_t qbase = s->regs[ETHOSU_REG_QBASE >> 2] |
                     ((uint64_t)s->regs[ETHOSU_REG_QBASE_HI >> 2] << 32);
    uint32_t qsize = s->regs[ETHOSU_REG_QSIZE >> 2];
    int i;

    fprintf(stderr, "ETHOSU: KICK qbase=0x%" PRIx64 " qsize=0x%x qconfig=0x%x "
            "regioncfg=0x%x\n", qbase, qsize,
            s->regs[ETHOSU_REG_QCONFIG >> 2],
            s->regs[ETHOSU_REG_REGIONCFG >> 2]);
    for (i = 0; i < 8; i++) {
        uint32_t lo = s->regs[(ETHOSU_REG_BASEP0 + i * 8) >> 2];
        uint32_t hi = s->regs[(ETHOSU_REG_BASEP0 + i * 8 + 4) >> 2];
        if (lo || hi) {
            fprintf(stderr, "ETHOSU:   BASEP%d = 0x%" PRIx64 "\n", i,
                    lo | ((uint64_t)hi << 32));
        }
    }

    if (qbase && qsize && qsize <= 0x4000) {
        uint8_t *cms = g_malloc(qsize);
        cpu_physical_memory_read(qbase, cms, qsize);
        fprintf(stderr, "ETHOSU: command-stream (%u bytes):\n", qsize);
        for (uint32_t off = 0; off < qsize; off += 16) {
            fprintf(stderr, "ETHOSU:   %04x:", off);
            for (uint32_t j = 0; j < 16 && off + j < qsize; j++) {
                fprintf(stderr, " %02x", cms[off + j]);
            }
            fprintf(stderr, "\n");
        }
        g_free(cms);
    }
}

/*
 * Fork-only "host inference" path.
 *
 * The real Ethos-U65 command-stream compute engine is not modelled. Instead,
 * when the M33 firmware kicks the NPU, we run the *reference* int8 TFLite model
 * on the host (via a tiny helper, ETHOSU_HOST_INFER, on ETHOSU_HOST_MODEL),
 * feeding it the guest's IFM and writing back the exact OFM the silicon would
 * produce, then raise the completion IRQ. This yields a correct end-to-end
 * inference on the fork without a full compute-engine model. The IFM/OFM live
 * in the tensor arena (NPU region 1 = BASEP1) at the model's planned offsets.
 */
/* A single in-flight inference job (host TFLite run, off the main loop). */
typedef struct EthosuJob {
    IMX93EthosuState *s;
    uint64_t ofm_addr;
    uint8_t *ifm;
    uint32_t ifm_len;
    uint8_t *ofm;
    uint32_t ofm_len;
    bool ok;
} EthosuJob;

static bool ethosu_host_infer(const uint8_t *ifm, uint32_t ifm_len,
                              uint8_t *ofm, uint32_t ofm_len)
{
    const char *py = getenv("ETHOSU_HOST_INFER");
    const char *model = getenv("ETHOSU_HOST_MODEL");
    char *ifm_path = NULL, *ofm_path = NULL;
    bool ok = false;
    int ifd = -1, ofd = -1;
    gint status = 0;
    GError *err = NULL;
    gchar *ofm_data = NULL;
    gsize ofm_got = 0;

    if (ethosu_trace()) {
        fprintf(stderr, "ETHOSU: host_infer py=%s model=%s\n",
                py ? py : "(null)", model ? model : "(null)");
    }
    if (!py || !model) {
        fprintf(stderr, "ETHOSU: ETHOSU_HOST_INFER/ETHOSU_HOST_MODEL unset - "
                "cannot run host inference\n");
        return false;
    }

    ifm_path = g_strdup("/tmp/ethosu_ifm_XXXXXX");
    ofm_path = g_strdup("/tmp/ethosu_ofm_XXXXXX");
    ifd = g_mkstemp(ifm_path);
    ofd = g_mkstemp(ofm_path);
    if (ifd < 0 || ofd < 0 || write(ifd, ifm, ifm_len) != (ssize_t)ifm_len) {
        goto out;
    }

    /* g_spawn uses posix_spawn - no fork() copy of QEMU's big address space. */
    {
        char *spawn_argv[6] = { (char *)"python3", (char *)py, (char *)model,
                                ifm_path, ofm_path, NULL };
        if (!g_spawn_sync(NULL, spawn_argv, NULL,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                          NULL, NULL, NULL, NULL, &status, &err) ||
            status != 0) {
            fprintf(stderr, "ETHOSU: host inference helper failed (%s)\n",
                    err ? err->message : "non-zero exit");
            goto out;
        }
    }

    if (!g_file_get_contents(ofm_path, &ofm_data, &ofm_got, &err)) {
        fprintf(stderr, "ETHOSU: could not read host OFM: %s\n",
                err ? err->message : "?");
        goto out;
    }
    memset(ofm, 0, ofm_len);
    memcpy(ofm, ofm_data, ofm_got < ofm_len ? ofm_got : ofm_len);
    ok = true;

out:
    if (ifd >= 0) {
        close(ifd);
    }
    if (ofd >= 0) {
        close(ofd);
    }
    if (ifm_path) {
        unlink(ifm_path);
    }
    if (ofm_path) {
        unlink(ofm_path);
    }
    g_clear_error(&err);
    g_free(ofm_data);
    g_free(ifm_path);
    g_free(ofm_path);
    return ok;
}

/*
 * Completion bottom half (main loop, BQL held): write the OFM back to guest
 * memory and raise the NPU completion IRQ.
 */
static void ethosu_complete_bh(void *opaque)
{
    EthosuJob *job = opaque;
    IMX93EthosuState *s = job->s;

    if (job->ok) {
        cpu_physical_memory_write(job->ofm_addr, job->ofm, job->ofm_len);
        if (ethosu_trace()) {
            fprintf(stderr, "ETHOSU: host inference done, OFM -> 0x%" PRIx64
                    " (%u bytes)\n", job->ofm_addr, job->ofm_len);
        }
    }

    /* Signal completion: command stream consumed, IRQ raised, no error bits. */
    s->regs[ETHOSU_REG_QREAD >> 2] = s->regs[ETHOSU_REG_QSIZE >> 2];
    s->regs[ETHOSU_REG_STATUS >> 2] =
        ETHOSU_STATUS_IRQ_RAISED | ETHOSU_STATUS_CMD_END_REACHED;
    qemu_set_irq(s->irq, 1);

    s->busy = false;
    g_free(job->ifm);
    g_free(job->ofm);
    g_free(job);
}

/* Worker thread: run the (slow) host TFLite inference off the main loop. */
static void *ethosu_infer_thread(void *opaque)
{
    EthosuJob *job = opaque;

    job->ok = ethosu_host_infer(job->ifm, job->ifm_len, job->ofm, job->ofm_len);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ethosu_complete_bh, job);
    return NULL;
}

/*
 * Bottom half scheduled on an NPU kick: locate the IFM/OFM in the tensor arena,
 * read the IFM, and hand off to a worker thread that runs the host inference.
 * The IFM/OFM live in the tensor arena (NPU region 1 = BASEP1) at the model's
 * planned offsets (reported by NETWORK_INFO; here as device defaults).
 */
static void ethosu_infer_bh(void *opaque)
{
    IMX93EthosuState *s = opaque;
    uint32_t region = ethosu_cfg_u32("ETHOSU_ARENA_REGION", 1);
    uint32_t ifm_off = ethosu_cfg_u32("ETHOSU_IFM_OFF", 0x1000);
    uint32_t ifm_size = ethosu_cfg_u32("ETHOSU_IFM_SIZE", 256);
    uint32_t ofm_off = ethosu_cfg_u32("ETHOSU_OFM_OFF", 0);
    uint32_t ofm_size = ethosu_cfg_u32("ETHOSU_OFM_SIZE", 2);
    uint32_t basep = ETHOSU_REG_BASEP0 + region * 8;
    uint64_t arena = s->regs[basep >> 2] |
                     ((uint64_t)s->regs[(basep + 4) >> 2] << 32);
    EthosuJob *job;
    QemuThread t;

    if (!arena) {
        /* No arena programmed - just complete so the firmware does not hang. */
        s->regs[ETHOSU_REG_QREAD >> 2] = s->regs[ETHOSU_REG_QSIZE >> 2];
        s->regs[ETHOSU_REG_STATUS >> 2] =
            ETHOSU_STATUS_IRQ_RAISED | ETHOSU_STATUS_CMD_END_REACHED;
        qemu_set_irq(s->irq, 1);
        s->busy = false;
        return;
    }

    if (ethosu_trace()) {
        fprintf(stderr, "ETHOSU: BH arena=0x%" PRIx64 " ifm@0x%" PRIx64
                " ofm@0x%" PRIx64 " - spawning host-infer thread\n",
                arena, arena + ifm_off, arena + ofm_off);
    }

    job = g_new0(EthosuJob, 1);
    job->s = s;
    job->ofm_addr = arena + ofm_off;
    job->ifm_len = ifm_size;
    job->ofm_len = ofm_size;
    job->ifm = g_malloc(ifm_size);
    job->ofm = g_malloc0(ofm_size);
    cpu_physical_memory_read(arena + ifm_off, job->ifm, ifm_size);

    qemu_thread_create(&t, "ethosu-infer", ethosu_infer_thread, job,
                       QEMU_THREAD_DETACHED);
}

/*
 * ID: arch 1.0.6, product_major 1, version 0.0.0 - a plausible Ethos-U65
 * identity reported to the capabilities query (not gated by device init).
 */
#define ETHOSU_ID_VALUE     0x10061000
/*
 * CONFIG: product=1 (U65, [31:28]), cmd_stream_version=0 ([7:4]),
 * macs_per_cc=8 ([3:0], i.e. 2^8=256 MACs/cc for ethos-u65-256). The firmware's
 * verify_optimizer_config does an exact-match of macs_per_cc and
 * cmd_stream_version against the Vela-compiled model's optimizer config, so
 * these must equal what Vela 4.3.0 targeting ethos-u65-256 emits.
 */
#define ETHOSU_CONFIG_VALUE 0x10000008

static uint64_t imx93_ethosu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93EthosuState *s = opaque;

    switch (offset) {
    case ETHOSU_REG_ID:
        return ETHOSU_ID_VALUE;
    case ETHOSU_REG_STATUS:
        /* Reflects run/complete state set by the kick + completion BH. */
        return s->regs[ETHOSU_REG_STATUS >> 2];
    case ETHOSU_REG_CONFIG:
        return ETHOSU_CONFIG_VALUE;
    default:
        if ((offset >> 2) >= IMX93_ETHOSU_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void imx93_ethosu_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX93EthosuState *s = opaque;

    if (ethosu_trace()) {
        fprintf(stderr, "ETHOSU: W off=0x%03x val=0x%08x\n",
                (unsigned)offset, (unsigned)value);
    }

    switch (offset) {
    case ETHOSU_REG_ID:
    case ETHOSU_REG_STATUS:
    case ETHOSU_REG_CONFIG:
        return;     /* read-only identification/status */
    case ETHOSU_REG_CMD:
        s->regs[ETHOSU_REG_CMD >> 2] = value;
        if ((value & ETHOSU_CMD_TRANSITION_TO_RUNNING) && !s->busy) {
            if (ethosu_trace()) {
                ethosu_dump_submission(s);
            }
            /* Enter "running"; the BH runs host inference then raises IRQ. */
            s->busy = true;
            s->regs[ETHOSU_REG_STATUS >> 2] = ETHOSU_STATUS_STATE_RUNNING;
            aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                    ethosu_infer_bh, s);
        }
        if (value & ETHOSU_CMD_CLEAR_IRQ) {
            s->regs[ETHOSU_REG_STATUS >> 2] &= ~ETHOSU_STATUS_IRQ_RAISED;
            qemu_set_irq(s->irq, 0);
        }
        return;
    case ETHOSU_REG_RESET:
        /*
         * The firmware writes RESET with the requested access state in the low
         * bits ([0]=privileged, [1]=non-secure) and then reads PROT back to
         * confirm the NPU "switched" to it. Mirror those bits into PROT.
         */
        s->regs[ETHOSU_REG_PROT >> 2] = value & 0x3;
        s->regs[ETHOSU_REG_RESET >> 2] = value;
        return;
    default:
        if ((offset >> 2) < IMX93_ETHOSU_REGS) {
            s->regs[offset >> 2] = value;
        }
    }
}

static const MemoryRegionOps imx93_ethosu_ops = {
    .read = imx93_ethosu_read,
    .write = imx93_ethosu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_ethosu_reset(DeviceState *dev)
{
    IMX93EthosuState *s = IMX93_ETHOSU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->busy = false;
}

static void imx93_ethosu_realize(DeviceState *dev, Error **errp)
{
    IMX93EthosuState *s = IMX93_ETHOSU(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx93_ethosu_ops, s,
                          TYPE_IMX93_ETHOSU, IMX93_ETHOSU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imx93_ethosu = {
    .name = TYPE_IMX93_ETHOSU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93EthosuState, IMX93_ETHOSU_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_ethosu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx93_ethosu_realize;
    dc->vmsd = &vmstate_imx93_ethosu;
    device_class_set_legacy_reset(dc, imx93_ethosu_reset);
    dc->desc = "i.MX93 Arm Ethos-U65 microNPU (bring-up model)";
}

static const TypeInfo imx93_ethosu_types[] = {
    {
        .name = TYPE_IMX93_ETHOSU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93EthosuState),
        .class_init = imx93_ethosu_class_init,
    },
};

DEFINE_TYPES(imx93_ethosu_types)
