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

/* Register block (BAR0+0x1000) offsets, decoded from AppleParavirtGPU setupFIFO.
 * The driver's MMIO trace shows accesses at BAR offsets 0x10xx, i.e. the fifo
 * register block starts at 0x1000: reg "0x04" is BAR 0x1004, etc. */

#define REG_BASE     0x1000

#define REG_LENGTH   (REG_BASE + 0x04)   /* fifo buffer length (driver writes 0x10000) */
#define REG_WRITE    (REG_BASE + 0x08)   /* driver->device write ptr (byte offset into cmd area) */
#define REG_READ     (REG_BASE + 0x0c)   /* driver->device read ptr (we advance it) */
#define REG_CMDOFF   (REG_BASE + 0x10)   /* command area offset inside the buffer (0x1000) */
#define REG_FIFOB    (REG_BASE + 0x1c)   /* PFN of the device->driver ring (reply fifoB) */
#define REG_FIFOA    (REG_BASE + 0x30)   /* PFN of the driver->device ring (commands) */
#define HDR_WRITE    0x00   /* fifoB header: write ptr (device-owned) */
#define HDR_READ     0x04   /* fifoB header: read ptr (driver-owned) */

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
    uint32_t reply_cur;  /* device reply cursor (M5v, posted into +0x0c) */
    bool reply;          /* experiment M5d: answer each command in fifoB + MSI */
    uint32_t repl_seq;   /* seq number of the last reply we wrote */
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
 * Doorbell (register block base + 8, i.e. BAR0 0x1008).
 * Decoded from AppleParavirtGPU setupFIFO: the driver allocates a 64 KB buffer, zeroes page 0 (header), puts
 * commands from +0x1000, and programs block+0x30 = physical address >> 12, block+4 = length, block+0x10 = 0x1000.
 * A second buffer is programmed through block+0x1c the same way. Dump header page and command area of both.
 */
static void hexdump_guest(AppleGfxTrace *s, const char *tag, uint64_t gpa, int len)
{
    uint8_t buf[0x200];
    MemTxResult r = pci_dma_read(PCI_DEVICE(s), gpa, buf, len);

    fprintf(s->log, "  d%u %s gpa=0x%" PRIx64 " len=0x%x %s\n", s->dumps, tag, gpa, len,
            r == MEMTX_OK ? "ok" : "DMAFAIL");
    if (r != MEMTX_OK) {
        return;
    }
    for (int off = 0; off < len; off += 16) {
        fprintf(s->log, "    %03x:", off);
        for (int b2 = 0; b2 < 16; b2++) {
            fprintf(s->log, " %02x", buf[off + b2]);
        }
        fputc('\n', s->log);
    }
}

static void dump_guest_pages(AppleGfxTrace *s)
{
    static const struct { hwaddr reg; const char *name; } fifos[2] = {
        { 0x1030, "fifoA" }, { 0x101c, "fifoB" },
    };

    if (s->dumps >= 16 || !s->log) {
        return;
    }
    s->dumps++;
    for (int i = 0; i < 2; i++) {
        uint32_t pfn;
        char tag[32];

        memcpy(&pfn, s->mem + fifos[i].reg, 4);
        snprintf(tag, sizeof(tag), "%s.header", fifos[i].name);
        hexdump_guest(s, tag, (uint64_t)pfn << 12, 0x40);
        snprintf(tag, sizeof(tag), "%s.data", fifos[i].name);
        hexdump_guest(s, tag, ((uint64_t)pfn << 12) + 0x1000, 0x200);
    }
    fflush(s->log);
}

/*
 * M5e: reply to every pending driver->device command.
 *
 * Decoded from AppleParavirtChannel init (0x148a9900) + reader (0x148a9ee0) +
 * segment parser (0x148a8ac2):
 *  - fifoB (reg +0x1c PFN, 64 KB) = root channel buffer. Page 0 = shared state,
 *    the per-channel SEGMENT DESCRIPTOR (root id=1) sits at fifoB+0x400:
 *      +0x00 u32 consumer ptr (driver-owned, byte offset into the ring)
 *      +0x04 u32 producer ptr (device-owned — WE advance it)
 *      +0x08 u32 (reply/flush id field, init 0xffffffff by driver?)
 *  - ring data area starts at buffer+0x1000, ring size = 64KB total.
 *  - Device->driver messages are APV "segments": [u32 0x10000 magic][u32 0]
 *    [u32 chunkCount][chunk table: {u32 len, u32 flags}][chunk data...]
 *  - Completion reply chunk (built by the driver at 0x148a8f52 as 12 bytes):
 *    { u32 id (= driver-side per-message counter); u64 host time }.
 *  - After writing the segment: advance the producer ptr and raise the MSI.
 *    The ISR reads dispatch masks from its own mirror struct (fields filled
 *    when the driver reads MMIO regs +0x14/+0x18 — those reads happen after
 *    the interrupt, so we don't need to pre-fill anything).
 */
static uint32_t reg_r32(AppleGfxTrace *s, unsigned off)
{
    uint32_t v;
    memcpy(&v, s->mem + off, 4);
    return v;
}

static void reg_w32(AppleGfxTrace *s, unsigned off, uint32_t v)
{
    memcpy(s->mem + off, &v, 4);
}

/* write n bytes into a byte-ring (base=gpa of ring start, size=ring size) */
static bool ring_write(AppleGfxTrace *s, uint64_t base, uint32_t size,
                       uint32_t off, const void *buf, uint32_t n)
{
    uint32_t first = MIN(n, size - off);
    const uint8_t *p = buf;

    if (pci_dma_write(PCI_DEVICE(s), base + off, p, first) != MEMTX_OK) {
        return false;
    }
    if (first < n &&
        pci_dma_write(PCI_DEVICE(s), base, p + first, n - first) != MEMTX_OK) {
        return false;
    }
    return true;
}

static void fifoB_reply(AppleGfxTrace *s)
{
    uint32_t fifoA_pfn = reg_r32(s, REG_FIFOA);
    uint32_t fifoB_pfn = reg_r32(s, REG_FIFOB);
    uint32_t wr = reg_r32(s, REG_WRITE);
    uint32_t rd = reg_r32(s, REG_READ);
    uint32_t buflen = reg_r32(s, REG_LENGTH);
    /*
     * M5v model: fifoA is ONE ring shared both ways.
     *   +0x08 = driver->device cursor (doorbell: end offset of driver cmds)
     *   +0x0c = device->driver cursor (end offset of device reply segments)
     * Driver's reader waits for segdesc+4 (device producer) — synced from
     * MMIO +0x0c; replies are placed at cmd-area offset = device reply
     * cursor, then +0x0c updated, then MSI raised.
     */
    if (rd > buflen - 0x1000) {
        fprintf(s->log, "reply: read ptr 0x%x out of range, ignoring\n", rd);
        fflush(s->log);
        return;
    }
    fprintf(s->log, "reply: doorbell state wr=0x%x rd=0x%x len=0x%x A=0x%x B=0x%x\n",
            wr, rd, buflen, fifoA_pfn, fifoB_pfn);
    uint32_t cmdbytes = buflen - 0x1000;
    int replies = 0;

    if (!fifoB_pfn || !fifoA_pfn) {
        fprintf(s->log, "reply: fifo PFN not set yet (A=0x%x B=0x%x)\n",
                fifoA_pfn, fifoB_pfn);
        fflush(s->log);
        return;
    }

    uint64_t fifoA_base = (uint64_t)fifoA_pfn << 12;
    uint64_t fifoB_base = (uint64_t)fifoB_pfn << 12;
    /*
     * M5i decode: root shared block +0x400 = array of {u32 seq, u32 PFN}
     * — one per driver message. The per-message PAGE is the segment
     * descriptor (M5h decode at 0x148a9b9b):
     *   +0x00 consumer (driver), +0x04 producer (device), +0x08 flushid,
     *   +0x0c u16 id, +0x10 ring PFN (device writes reply segments here).
     */
    uint32_t prod = 0, cons = 0, ring_pfn = 0;

    /* consume every complete message in fifoA between rd and wr (wrap-safe);
     * cursors are offsets in the COMMAND AREA (base = fifoA_base + 0x1000) */
    uint32_t cmd_base = fifoA_base + 0x1000;
    while (rd != wr && replies < 8) {
        uint32_t hdr[3];
        uint8_t msg[256];
        uint32_t len;
        uint64_t fifoA_gpa = cmd_base + rd;

        uint32_t chunk = MIN(12, cmdbytes - rd);
        if (pci_dma_read(PCI_DEVICE(s), fifoA_gpa, hdr, chunk) != MEMTX_OK ||
            (chunk < 12 && pci_dma_read(PCI_DEVICE(s), cmd_base,
                                        (uint8_t *)hdr + chunk, 12 - chunk) != MEMTX_OK)) {
            fprintf(s->log, "reply: DMAFAIL reading fifoA hdr at cursor 0x%x\n", rd);
            break;
        }
        len = hdr[1];
        if (len < 12 || len % 4 || len > sizeof(msg) || len > cmdbytes) {
            fprintf(s->log, "reply: bad message len %u at rd=0x%x, stop\n", len, rd);
            break;
        }
        if (pci_dma_read(PCI_DEVICE(s), fifoA_gpa, msg, len) != MEMTX_OK) {
            fprintf(s->log, "reply: DMAFAIL reading fifoA msg at 0x%x\n", rd);
            break;
        }

        uint32_t seq = hdr[2];
        uint32_t op = hdr[0];

        /*
         * M5n: reader 0x148a9e56 spins on [segdesc+0x4] producer in shared
         * memory; segdesc = {prod@0, cons@4, flushid@8, id@c, ringpfn@10}.
         * Candidate segdesc PA: op-0x38 payload[3] = PFN (0x202f55 / 0x1f0996
         * / ... varies per boot). Dump it + reply into it.
         */
        if (op == 0x38 && hdr[1] >= 28 && ((uint32_t *)msg)[3] == 1) {
            uint32_t cand = ((uint32_t *)msg)[6];
            uint32_t page[1024];
            if (pci_dma_read(PCI_DEVICE(s), (uint64_t)cand << 12, page,
                             sizeof(page)) == MEMTX_OK) {
                fprintf(s->log, "reply: op38 pfn 0x%x dump:", cand);
                for (int i = 0; i < 16; i++) {
                    if (page[i]) fprintf(s->log, " +%x=%08x", i * 4, page[i]);
                }
                fprintf(s->log, "\n");
                fflush(s->log);
            }
        }
        if (op == 0x3a && hdr[1] >= 24) {
            uint32_t cand = ((uint32_t *)msg)[6];
            uint32_t page[1024];
            if (pci_dma_read(PCI_DEVICE(s), (uint64_t)cand << 12, page,
                             sizeof(page)) == MEMTX_OK) {
                fprintf(s->log, "reply: op3a pfn 0x%x dump:", cand);
                for (int i = 0; i < 32; i++) {
                    if (page[i]) fprintf(s->log, " +%x=%08x", i * 4, page[i]);
                }
                fprintf(s->log, "\n");
                fflush(s->log);
            }
        }

        /*
         * M5m: per-message slot {idx@+0xc, ptab_pfn@+0x10} (stride 0x14) là
         * IOMMU page-table bookkeeping — KHÔNG phải reply channel. Reply
         * quay về CÙNG fifoA ring: device ghi segment tại cursor producer
         * (mirror+0x8), rồi MMIO-write reg +0x8 = producer mới (device-owned
         * producer cursor), set masks + MSI. Driver ISR sẽ đọc reg +0x8,
         * update segdesc producer và chạy reader.
         */
        /*
         * M6a: reply RAW pull-based — driver reader copy len*4 bytes RAW từ
         * ring fifoA tại device reply cursor. Reply len theo op:
         *   op 0x38: payload[2] (=4) dwords → 16B
         *   op 0x3a: payload[1] (=0x200) bytes
         *   op 0x30: payload[0] dwords
         *   op 0x33: payload[1] dwords? (payload {0,1,0x100000}: len=1 dword?)
         * Nội dung: zeros + seq/id — driver caller sẽ parse; assert sẽ in
         * lên serial nếu sai.
         */
        uint32_t prod = s->reply_cur;
        uint32_t ring_size = cmdbytes;   /* fifoA cmd area size (0xf000) */
        uint32_t seglen;
        if (op == 0x38) seglen = ((uint32_t *)msg)[5] * 4;
        else if (op == 0x3a) seglen = ((uint32_t *)msg)[5];
        else if (op == 0x30) seglen = ((uint32_t *)msg)[3] * 4;
        else if (op == 0x33) seglen = ((uint32_t *)msg)[4] * 4;
        else seglen = 16;
        if (!seglen || seglen > 0x400) seglen = 16;
        uint32_t dcur = wr;              /* driver cmd cursor (doorbell) */
        uint32_t space = (prod >= dcur) ? (ring_size - (prod - dcur))
                                        : (dcur - prod);
        if (space < seglen + 4) {
            fprintf(s->log, "reply: fifoA ring full (prod=0x%x dcur=0x%x)\n", prod, dcur);
            break;
        }
        uint64_t ts = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint8_t segbuf[0x400];
        memset(segbuf, 0, sizeof(segbuf));
        ((uint32_t *)segbuf)[0] = seq;           /* id đầu reply */
        bool ok = ring_write(s, fifoA_base, ring_size, prod, segbuf, seglen);
        if (!ok) {
            fprintf(s->log, "reply: DMAFAIL writing fifoA segment (seq %u)\n", seq);
            break;
        }
        uint32_t nprod = prod + seglen;
        reg_w32(s, REG_READ, nprod);     /* device reply cursor -> +0x0c */
        s->reply_cur = nprod;
        fprintf(s->log,
                "reply: posted seq=%u op=0x%x len=%u -> fifoA reply 0x%x->0x%x (dcur=0x%x)\n",
                seq, op, seglen, prod, nprod, dcur);
        s->repl_seq = seq;
        replies++;

        /* advance the fifoA read ptr by one message */
        uint32_t nrd = rd + len;
        if (nrd >= cmdbytes) {
            nrd = nrd - cmdbytes;
        }
        rd = nrd;
        reg_w32(s, REG_READ, rd);
    }
    fflush(s->log);
    if (replies) {
        /*
         * M5g: the ISR dispatches per-channel on MMIO dispatch masks it reads
         * at regs +0x14 (event mask) and +0x18 (MSI dispatch mask), bit per
         * channel (root = bit0). In M5f both were 0 so the ISR ran but never
         * dispatched the reader. Set both masks before the MSI.
         */
        reg_w32(s, REG_BASE + 0x14, 0x1);
        reg_w32(s, REG_BASE + 0x18, 0x1);
        /* M6b: +0x2c bit0 = FAULT flag (panic handleFaultInterrupt) — KHÔNG set */
        fprintf(s->log, "reply: dispatch masks +0x14=1 +0x18=1 (+0x2c untouched)\n");
        fflush(s->log);
        if (msi_enabled(PCI_DEVICE(s))) {
            msi_notify(PCI_DEVICE(s), 0);
            fprintf(s->log, "reply: MSI vector 0 raised (%u replies)\n", replies);
            fflush(s->log);
        } else {
            fprintf(s->log, "reply: MSI NOT enabled, %u replies written silently\n", replies);
            fflush(s->log);
        }
    }
}

static uint64_t gfx_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleGfxTrace *s = opaque;
    uint64_t val = 0;

    if (addr + size <= s->bar0_size) {
        memcpy(&val, s->mem + addr, size);   /* little-endian host/guest */
    }
    /* M5v: +0x0c là reply cursor của device (reg_w32 đã ghi vào s->mem) —
     * bỏ override last_db cũ (M5f) để driver đọc đúng giá trị. */
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
        if (s->reply) {
            fifoB_reply(s);
        }
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
    DEFINE_PROP_BOOL("reply", AppleGfxTrace, reply, false),
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
