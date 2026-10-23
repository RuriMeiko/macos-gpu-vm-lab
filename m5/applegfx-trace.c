/*
 * applegfx-trace: logging stub for the PCI device that Apple's AppleParavirtGPU
 * guest driver binds to (vendor 0x106B, device 0xEEEE).
 *
 * It implements no graphics. It maps BAR0, logs every guest MMIO access
 * (sequence, virtual time, offset, size, value) and answers reads from a plain
 * RAM-backed buffer, so values the guest wrote can be read back. Purpose:
 * find out what the stock guest driver probes first, before writing a real device.
 *
 * Lab code, PolyForm Noncommercial like the rest of this repo (see LICENSE). Built into QEMU (GPL-2.0).
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "system/dma.h"

#define TYPE_APPLEGFX_TRACE "applegfx-trace"
OBJECT_DECLARE_SIMPLE_TYPE(AppleGfxTrace, APPLEGFX_TRACE)

struct AppleGfxTrace {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    uint8_t *mem;
    FILE *log;
    uint64_t seq;
    /* properties */
    char *logpath;
    uint32_t bar0_size;
    uint32_t readfill;
    uint32_t dumps;      /* doorbell dumps done so far */
    uint32_t last_db;    /* last value written to the doorbell (0x1008) */
};

static void trace_line(AppleGfxTrace *s, const char *dir, hwaddr addr,
                       unsigned size, uint64_t val)
{
    if (!s->log) {
        return;
    }
    fprintf(s->log, "%" PRIu64 " t=%" PRId64 " %s off=0x%05" HWADDR_PRIx
            " size=%u val=0x%" PRIx64 "\n", s->seq++,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), dir, addr, size, val);
    fflush(s->log);
}


/*
 * Doorbell (offset 0x1008): the register meanings are unknown, so probe each value the driver programmed
 * (0x1004, 0x101c, 0x1030) as a guest address under several shifts and log only locations that hold data.
 */
static void dump_guest_pages(AppleGfxTrace *s)
{
    static const hwaddr regs[3] = { 0x1004, 0x101c, 0x1030 };
    static const int shifts[4] = { 12, 8, 4, 0 };
    PCIDevice *pdev = PCI_DEVICE(s);
    uint8_t buf[0x100];

    if (s->dumps >= 16 || !s->log) {
        return;
    }
    s->dumps++;
    for (int i = 0; i < 3; i++) {
        uint32_t v;

        memcpy(&v, s->mem + regs[i], 4);
        for (int j = 0; j < 4; j++) {
            uint64_t gpa = (uint64_t)v << shifts[j];
            bool nz = false;

            if (pci_dma_read(pdev, gpa, buf, sizeof(buf)) != MEMTX_OK) {
                fprintf(s->log, "  d%u reg=0x%" HWADDR_PRIx " v=0x%x shl=%d gpa=0x%" PRIx64 " DMAFAIL\n",
                        s->dumps, regs[i], v, shifts[j], gpa);
                continue;
            }
            for (int k = 0; k < (int)sizeof(buf); k++) {
                nz |= buf[k] != 0;
            }
            fprintf(s->log, "  d%u reg=0x%" HWADDR_PRIx " v=0x%x shl=%d gpa=0x%" PRIx64 " %s\n",
                    s->dumps, regs[i], v, shifts[j], gpa, nz ? "DATA" : "zero");
            if (nz) {
                for (int off = 0; off < (int)sizeof(buf); off += 16) {
                    fprintf(s->log, "    %03x:", off);
                    for (int b2 = 0; b2 < 16; b2++) {
                        fprintf(s->log, " %02x", buf[off + b2]);
                    }
                    fputc('\n', s->log);
                }
            }
        }
    }
    fflush(s->log);
}

static uint64_t gfx_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleGfxTrace *s = opaque;
    uint64_t val = 0;

    if (addr + size <= s->bar0_size) {
        memcpy(&val, s->mem + addr, size);   /* little-endian host/guest */
    }
    if (addr == 0x100c && size == 4) {
        val = s->last_db;                     /* experiment: consumer == producer */
    }
    trace_line(s, "R", addr, size, val);
    return val;
}

static void gfx_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    AppleGfxTrace *s = opaque;

    trace_line(s, "W", addr, size, val);
    if (addr + size <= s->bar0_size) {
        memcpy(s->mem + addr, &val, size);
    }
    if (addr == 0x1008) {
        s->last_db = val;
        dump_guest_pages(s);
    }
}

static const MemoryRegionOps gfx_mmio_ops = {
    .read = gfx_mmio_read,
    .write = gfx_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl  = { .min_access_size = 1, .max_access_size = 8 },
};

static void gfx_realize(PCIDevice *pdev, Error **errp)
{
    AppleGfxTrace *s = APPLEGFX_TRACE(pdev);

    if (s->bar0_size < 0x1000 || (s->bar0_size & (s->bar0_size - 1))) {
        error_setg(errp, "bar0-size must be a power of two >= 0x1000");
        return;
    }
    s->log = fopen(s->logpath ? s->logpath : "/tmp/applegfx-trace.log", "w");
    if (!s->log) {
        error_setg_errno(errp, errno, "cannot open trace log");
        return;
    }
    s->mem = g_malloc(s->bar0_size);
    memset(s->mem, s->readfill & 0xff, s->bar0_size);

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }
    memory_region_init_io(&s->mmio, OBJECT(s), &gfx_mmio_ops, s,
                          "applegfx-trace.mmio", s->bar0_size);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    fprintf(s->log, "# realize bar0=0x%x readfill=0x%02x\n", s->bar0_size,
            s->readfill & 0xff);
    fflush(s->log);
}

static void gfx_exit(PCIDevice *pdev)
{
    AppleGfxTrace *s = APPLEGFX_TRACE(pdev);

    msi_uninit(pdev);
    g_free(s->mem);
    if (s->log) {
        fclose(s->log);
    }
}

static const Property gfx_props[] = {
    DEFINE_PROP_STRING("log", AppleGfxTrace, logpath),
    DEFINE_PROP_UINT32("bar0-size", AppleGfxTrace, bar0_size, 0x40000),
    DEFINE_PROP_UINT32("readfill", AppleGfxTrace, readfill, 0),
};

static void gfx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = gfx_realize;
    k->exit = gfx_exit;
    k->vendor_id = 0x106b;          /* Apple */
    k->device_id = 0xeeee;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;
    device_class_set_props(dc, gfx_props);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo gfx_types[] = {
    {
        .name          = TYPE_APPLEGFX_TRACE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(AppleGfxTrace),
        .class_init    = gfx_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(gfx_types)
