/*
 * VirtIOGPUProbe v0.0.7 — M3 phase 2: framebuffer pipeline
 * after GET_DISPLAY_INFO: resource_create_2d -> attach_backing ->
 * set_scanout -> transfer_to_host_2d (pattern fill) -> resource_flush.
 * Generic ctrl submit: desc0(READ hdr+payload, NEXT) -> desc1(WRITE resp).
 */
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pci/IOPCIBridge.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <mach/kmod.h>

// QEMU 10.2.1 common cfg layout (verified from source + WRTEST; no magic header):
#define VCC_DFSELECT 0x00
#define VCC_DF       0x04
#define VCC_GFSELECT 0x08
#define VCC_GF       0x0C
#define VCC_MSIX     0x10
#define VCC_NUMQ     0x12
#define VCC_STATUS   0x14
#define VCC_GEN      0x15
#define VCC_Q_SELECT 0x16
#define VCC_Q_SIZE   0x18
#define VCC_Q_MSIX   0x1A
#define VCC_Q_ENABLE 0x1C
#define VCC_Q_NOFF   0x1E
#define VCC_Q_DESCLO 0x20
#define VCC_Q_DESCHI 0x24
#define VCC_Q_AVAILLO 0x28
#define VCC_Q_AVAILHI 0x2C
#define VCC_Q_USEDLO  0x30
#define VCC_Q_USEDHI  0x34

#define VS_ACKNOWLEDGE 1
#define VS_DRIVER      2
#define VS_DRIVER_OK   4
#define VS_FEATURES_OK 8
#define VQ_DESC_F_WRITE 2

// virtio_gpu ctrl types (from virtio_gpu.h enum):
#define VGPU_GET_DISPLAY_INFO    0x0100
#define VGPU_RESOURCE_CREATE_2D  0x0101
#define VGPU_RESOURCE_UNREF      0x0102
#define VGPU_SET_SCANOUT         0x0103
#define VGPU_RESOURCE_FLUSH      0x0104
#define VGPU_TRANSFER_TO_HOST_2D 0x0105
#define VGPU_RESOURCE_ATTACH_BACKING 0x0106

#define VGPU_RESP_OK_NODATA      0x1100
#define VGPU_RESP_OK_DISPLAY_INFO 0x1101

#define VGPU_FORMAT_B8G8R8A8     1

struct vgpu_ctrl_hdr {
    UInt32 ctrl_type;
    UInt32 type_flags;
    UInt64 fence_id;
    UInt32 ctx_id;
    UInt32 padding;
};  // 24 bytes

struct vgpu_rect {
    UInt32 x, y, w, h;
}; // 16 bytes

struct vgpu_create2d {
    struct vgpu_ctrl_hdr hdr;
    UInt32 resource_id;
    UInt32 format;
    UInt32 width;
    UInt32 height;
}; // 40

struct vgpu_attach_backing {
    struct vgpu_ctrl_hdr hdr;
    UInt32 resource_id;
    UInt32 nr_entries;
}; // 32; followed by mem_entry {u64 addr, u32 len, u32 pad} = 16

struct vgpu_set_scanout {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    UInt32 scanout_id;
    UInt32 resource_id;
}; // 48

struct vgpu_transfer2d {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    UInt64 offset;
    UInt32 resource_id;
    UInt32 padding;
}; // 56

struct vgpu_flush {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect r;
    UInt32 resource_id;
    UInt32 padding;
}; // 48

class VirtIOGPUProbe : public IOService {
    OSDeclareDefaultStructors(VirtIOGPUProbe)
private:
    IOPCIDevice * pci;
    IOMemoryMap * bar4Map;
    volatile UInt8 * common;
    volatile UInt8 * deviceCfg;
    volatile UInt8 * notifyBase;
    UInt32 notifyMultiplier;

    UInt8 *  ringVirt;
    IOBufferMemoryDescriptor * ringBuf;
    UInt64   ringPhys;
    UInt16   qsize;
    UInt64   availOff, usedOff;
    UInt16   availNext; // next avail ring index
    UInt16   descNext;  // next free desc index (pairs: 0/1, 2/3, ...)

    // ctrl area: req at 0..1024, resp at 1024..1536, backing entry at 1536..1552
    UInt8 * ctrlVirt;
    IOBufferMemoryDescriptor * ctrlBuf;
    UInt64 ctrlPhys;

    UInt32 displayW, displayH, displayEnabled;

    // framebuffer backing: 1280x800x4 = 4,096,000 bytes (contiguous)
    UInt8 * fbVirt;
    IOBufferMemoryDescriptor * fbBuf;
    UInt64 fbPhys;
    UInt32 fbLen;

    bool parseCaps();
    bool handshake();
    bool initControlQueue();
    // Generic ctrl: req at ctrlPhys (len reqLen), resp 512 into ctrlPhys+1024.
    // Returns response ctrl_type (0 = timeout).
    UInt32 submitCtrl(UInt32 reqLen);
    void runPipeline();
public:
    virtual bool init(OSDictionary * dict = 0) override;
    virtual void free() override;
    virtual IOService * probe(IOService * provider, SInt32 * score) override;
    virtual bool start(IOService * provider) override;
    virtual void stop(IOService * provider) override;
};

extern "C" kern_return_t kmod_dummy_start(kmod_info_t * ki, void * data);
extern "C" kern_return_t kmod_dummy_stop(kmod_info_t * ki, void * data);
extern "C" kmod_info_t kmod_info;
kmod_info_t kmod_info = {
    NULL, KMOD_INFO_VERSION, 0,
    "com.mylab.VirtIOGPUProbe", "0.0.7",
    0, NULL, 0, 0, 0,
    (kmod_start_func_t*)&kmod_dummy_start,
    (kmod_stop_func_t*)&kmod_dummy_stop,
};
extern "C" kern_return_t kmod_dummy_start(kmod_info_t * ki, void * data) { return KERN_SUCCESS; }
extern "C" kern_return_t kmod_dummy_stop(kmod_info_t * ki, void * data)  { return KERN_SUCCESS; }

OSDefineMetaClassAndStructors(VirtIOGPUProbe, IOService)

bool VirtIOGPUProbe::init(OSDictionary * dict) {
    if (!IOService::init(dict)) return false;
    pci = NULL; bar4Map = NULL; common = NULL; deviceCfg = NULL; notifyBase = NULL;
    notifyMultiplier = 0; ringVirt = NULL; ringBuf = NULL; ringPhys = 0;
    qsize = 0; availOff = 0; usedOff = 0; availNext = 0; descNext = 0;
    ctrlVirt = NULL; ctrlBuf = NULL; ctrlPhys = 0;
    displayW = displayH = displayEnabled = 0;
    fbVirt = NULL; fbBuf = NULL; fbPhys = 0; fbLen = 0;
    return true;
}

void VirtIOGPUProbe::free() {
    if (fbBuf) { fbBuf->release(); fbBuf = NULL; }
    if (ctrlBuf) { ctrlBuf->release(); ctrlBuf = NULL; }
    if (ringBuf) { ringBuf->release(); ringBuf = NULL; }
    IOService::free();
}

IOService * VirtIOGPUProbe::probe(IOService * provider, SInt32 * score) {
    IOService * res = IOService::probe(provider, score);
    IOLog("VGDRV: probe() hit score=%d\n", (int)*score);
    return res;
}

bool VirtIOGPUProbe::parseCaps() {
    UInt8 capOff = pci->configRead8(0x34);
    bool haveCommon = false, haveDevice = false, haveNotify = false;
    UInt64 notifyOff = 0;
    int guard = 0;
    while (capOff != 0 && capOff != 0xFF && guard++ < 64) {
        UInt8 capID  = pci->configRead8(capOff);
        UInt8 capLen = pci->configRead8(capOff + 2);
        if (capID == 0x09 && capLen >= 16) {
            UInt8 cfgType = pci->configRead8(capOff + 3);
            UInt8 bar     = pci->configRead8(capOff + 4);
            UInt32 offLo  = pci->configRead32(capOff + 8);
            UInt32 lenLo  = pci->configRead32(capOff + 12);
            UInt64 off = (UInt64)offLo;
            UInt64 len = (UInt64)lenLo;
            if (cfgType == 2 && bar == 4) {
                if (capLen >= 20) notifyMultiplier = pci->configRead32(capOff + 16);
                haveNotify = true; notifyOff = off;
            } else if (capLen >= 24) {
                off |= (UInt64)pci->configRead32(capOff + 16) << 32;
                len |= (UInt64)pci->configRead32(capOff + 20) << 32;
            }
            IOLog("VGDRV: cap type=%u bar=%u off=%llu len=%llu\n", cfgType, bar, off, len);
            if (cfgType == 1 && bar == 4 && off == 0)         haveCommon = true;
            else if (cfgType == 4 && bar == 4 && off == 8192) haveDevice = true;
        }
        capOff = pci->configRead8(capOff + 1);
    }
    if (!haveCommon || !haveDevice || !haveNotify) {
        IOLog("VGDRV: caps incomplete c=%d d=%d n=%d\n", haveCommon, haveDevice, haveNotify);
        return false;
    }
    bar4Map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + 16,
                                               kIOMapInhibitCache);
    if (!bar4Map) { IOLog("VGDRV: BAR4 map failed\n"); return false; }
    volatile UInt8 * base = (volatile UInt8 *)bar4Map->getVirtualAddress();
    common     = base + 0;
    deviceCfg  = base + 8192;
    notifyBase = base + notifyOff;
    IOLog("VGDRV: BAR4 mapped virt=%p size=%llu mult=%u\n",
          (void*)base, bar4Map->getLength(), notifyMultiplier);
    return true;
}

bool VirtIOGPUProbe::handshake() {
    *(volatile UInt8 *)(common + VCC_STATUS) = 0;
    OSSynchronizeIO();
    *(volatile UInt8 *)(common + VCC_STATUS) = VS_ACKNOWLEDGE | VS_DRIVER;
    OSSynchronizeIO();
    IOLog("VGDRV: status after ACK|DRIVER=%u\n", *(volatile UInt8 *)(common + VCC_STATUS));

    *(volatile UInt32 *)(common + VCC_DFSELECT) = 1;
    UInt32 fHi = *(volatile UInt32 *)(common + VCC_DF);
    *(volatile UInt32 *)(common + VCC_DFSELECT) = 0;
    UInt32 fLo = *(volatile UInt32 *)(common + VCC_DF);
    IOLog("VGDRV: devfeat hi=%08x lo=%08x\n", fHi, fLo);

    *(volatile UInt32 *)(common + VCC_GFSELECT) = 1;
    *(volatile UInt32 *)(common + VCC_GF) = fHi & 1;
    *(volatile UInt32 *)(common + VCC_GFSELECT) = 0;
    *(volatile UInt32 *)(common + VCC_GF) = 0;
    OSSynchronizeIO();

    *(volatile UInt8 *)(common + VCC_STATUS) =
        *(volatile UInt8 *)(common + VCC_STATUS) | VS_FEATURES_OK;
    OSSynchronizeIO();
    UInt8 st = *(volatile UInt8 *)(common + VCC_STATUS);
    IOLog("VGDRV: status after FEAT_OK=%u\n", st);
    if (!(st & VS_FEATURES_OK)) return false;
    return true;
}

bool VirtIOGPUProbe::initControlQueue() {
    UInt16 nq = *(volatile UInt16 *)(common + VCC_NUMQ);
    IOLog("VGDRV: num_queues=%u\n", nq);
    *(volatile UInt16 *)(common + VCC_Q_SELECT) = 0;
    OSSynchronizeIO();
    UInt16 qsizeDev = *(volatile UInt16 *)(common + VCC_Q_SIZE);
    UInt16 qNotify  = *(volatile UInt16 *)(common + VCC_Q_NOFF);
    IOLog("VGDRV: q0 size=%u noff=%u\n", qsizeDev, qNotify);
    if (qsizeDev == 0) { IOLog("VGDRV: q0 size=0 - abort\n"); return false; }
    qsize = qsizeDev;

    UInt64 descLen  = (UInt64)qsize * 16;
    UInt64 availLen = 6 + 2 * (UInt64)qsize;
    UInt64 usedLen  = 6 + 8 * (UInt64)qsize;
    availOff = (descLen + 1) & ~1ULL;
    usedOff  = (availOff + availLen + 3) & ~3ULL;
    UInt64 total = usedOff + usedLen;

    ringBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, total,
        0xFFFFFFFFFFFFF000ULL);
    if (!ringBuf) { IOLog("VGDRV: ring alloc failed\n"); return false; }
    IOByteCount segLen = 0;
    ringPhys = ringBuf->getPhysicalSegment(0, &segLen, 0);
    ringVirt = (UInt8 *)ringBuf->getBytesNoCopy();
    bzero(ringVirt, total);
    IOLog("VGDRV: ring virt=%p phys=%llx len=%llu avail@%llu used@%llu\n",
          ringVirt, ringPhys, (UInt64)segLen, availOff, usedOff);
    if ((UInt64)segLen < total) return false;

    *(volatile UInt32 *)(common + VCC_Q_DESCLO)  = (UInt32)(ringPhys & 0xFFFFFFFF);
    *(volatile UInt32 *)(common + VCC_Q_DESCHI)  = (UInt32)(ringPhys >> 32);
    *(volatile UInt32 *)(common + VCC_Q_AVAILLO) = (UInt32)((ringPhys + availOff) & 0xFFFFFFFF);
    *(volatile UInt32 *)(common + VCC_Q_AVAILHI) = (UInt32)((ringPhys + availOff) >> 32);
    *(volatile UInt32 *)(common + VCC_Q_USEDLO)  = (UInt32)((ringPhys + usedOff) & 0xFFFFFFFF);
    *(volatile UInt32 *)(common + VCC_Q_USEDHI)  = (UInt32)((ringPhys + usedOff) >> 32);
    OSSynchronizeIO();
    *(volatile UInt16 *)(common + VCC_Q_ENABLE) = 1;
    OSSynchronizeIO();
    IOLog("VGDRV: q0 enable readback=%u\n", *(volatile UInt16 *)(common + VCC_Q_ENABLE));

    UInt8 st = *(volatile UInt8 *)(common + VCC_STATUS);
    *(volatile UInt8 *)(common + VCC_STATUS) = st | VS_DRIVER_OK;
    OSSynchronizeIO();
    IOLog("VGDRV: DRIVER_OK set, status=%u\n", *(volatile UInt8 *)(common + VCC_STATUS));

    // ctrl area: req at 0..1024, resp at 1024..1536, backing entry at 1536
    ctrlBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, 2048,
        0xFFFFFFFFFFFFF000ULL);
    if (!ctrlBuf) { IOLog("VGDRV: ctrlBuf alloc failed\n"); return false; }
    IOByteCount cLen = 0;
    ctrlPhys = ctrlBuf->getPhysicalSegment(0, &cLen, 0);
    ctrlVirt = (UInt8 *)ctrlBuf->getBytesNoCopy();
    bzero(ctrlVirt, 2048);
    IOLog("VGDRV: ctrlBuf virt=%p phys=%llx\n", ctrlVirt, ctrlPhys);
    return true;
}

// Submit one ctrl request: desc pair (d0 READ reqLen + NEXT, d1 WRITE 512),
// avail entry desc0-index, kick doorbell, poll used ring for our entry.
// availNext/used idx are the shared sequence (device consumes sequentially).
UInt32 VirtIOGPUProbe::submitCtrl(UInt32 reqLen) {
    if (!ringVirt) return 0;
    volatile UInt8 * desc  = ringVirt;
    volatile UInt8 * avail = ringVirt + availOff;
    volatile UInt8 * used  = ringVirt + usedOff;

    UInt16 d0 = descNext;
    UInt16 d1 = (UInt16)((d0 + 1) % qsize);
    descNext = (UInt16)((d0 + 2) % qsize);

    // desc0: READ reqLen, NEXT
    *(volatile UInt64 *)(desc + d0*16 + 0)  = ctrlPhys;
    *(volatile UInt32 *)(desc + d0*16 + 8)  = reqLen;
    *(volatile UInt16 *)(desc + d0*16 + 12) = 1;              // NEXT
    *(volatile UInt16 *)(desc + d0*16 + 14) = d1;
    // desc1: WRITE 512
    *(volatile UInt64 *)(desc + d1*16 + 0)  = ctrlPhys + 1024;
    *(volatile UInt32 *)(desc + d1*16 + 8)  = 512;
    *(volatile UInt16 *)(desc + d1*16 + 12) = VQ_DESC_F_WRITE;
    *(volatile UInt16 *)(desc + d1*16 + 14) = 0;

    // avail: push d0. availNext = OUR counter of published entries (starts 0
    // after bzero; device reads entries from avail.idx which we publish).
    // We track how many entries we've published to compute expected used.idx.
    static UInt16 published = 0; // per-queue sequence; device starts at 0 too
    *(volatile UInt16 *)(avail + 4 + 2 * (published % qsize)) = d0;
    OSSynchronizeIO();
    *(volatile UInt16 *)(avail + 2) = (UInt16)(published + 1);
    OSSynchronizeIO();

    // kick doorbell q0
    UInt16 qNotify = *(volatile UInt16 *)(common + VCC_Q_NOFF);
    volatile UInt16 * doorbell =
        (volatile UInt16 *)(notifyBase + (UInt64)qNotify * notifyMultiplier);
    *doorbell = 0;
    OSSynchronizeIO();

    // poll used ring until used.idx == published+1
    for (int i = 0; i < 2000; i++) {
        OSSynchronizeIO();
        UInt16 uIdx = *(volatile UInt16 *)(used + 2);
        if (uIdx == (UInt16)(published + 1)) {
            volatile UInt8 * entry = used + 4 + 8 * (published % qsize);
            UInt32 eLen = *(volatile UInt32 *)(entry + 4);
            volatile UInt8 * resp = ctrlVirt + 1024;
            UInt32 rType = *(volatile UInt32 *)(resp + 0);
            published++;
            IOLog("VGDRV: ctrl len=%u resp_type=%x\n", eLen, rType);
            return rType;
        }
        IODelay(1000);
    }
    published++; // keep sequence in sync even on timeout
    IOLog("VGDRV: ctrl TIMEOUT (reqLen=%u)\n", reqLen);
    return 0;
}

void VirtIOGPUProbe::runPipeline() {
    // ---- GET_DISPLAY_INFO ----
    bzero(ctrlVirt, 2048);
    struct vgpu_ctrl_hdr * req = (struct vgpu_ctrl_hdr *)ctrlVirt;
    req->ctrl_type = VGPU_GET_DISPLAY_INFO;
    UInt32 r = submitCtrl(24);
    if (r != VGPU_RESP_OK_DISPLAY_INFO) {
        IOLog("VGDRV: GET_DISPLAY_INFO failed type=%x\n", r);
        return;
    }
    volatile UInt8 * resp = ctrlVirt + 1024;
    for (int d = 0; d < 8; d++) {
        volatile UInt32 * de = (volatile UInt32 *)(resp + 24 + d*24);
        if (de[4]) {
            displayEnabled = 1;
            displayW = de[2]; displayH = de[3];
            IOLog("VGDRV: display[%d] rect=(%u,%u %ux%u) enabled\n", d, de[0], de[1], de[2], de[3]);
            break;
        }
    }
    if (!displayEnabled) { IOLog("VGDRV: no enabled display\n"); return; }
    IOLog("VGDRV: === GET_DISPLAY_INFO SUCCESS %ux%u ===\n", displayW, displayH);

    // ---- alloc FB + pattern fill (diagonal blue/yellow gradient) ----
    fbLen = displayW * displayH * 4;
    fbBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, fbLen,
        0xFFFFFFFFFFFFF000ULL);
    if (!fbBuf) { IOLog("VGDRV: fb alloc failed\n"); return; }
    IOByteCount fbSeg = 0;
    fbPhys = fbBuf->getPhysicalSegment(0, &fbSeg, 0);
    fbVirt = (UInt8 *)fbBuf->getBytesNoCopy();
    IOLog("VGDRV: fb phys=%llx len=%u (seg=%u)\n", fbPhys, fbLen, (unsigned)fbSeg);
    if ((UInt64)fbSeg < fbLen) { IOLog("VGDRV: fb not contiguous\n"); return; }
    for (UInt32 y = 0; y < displayH; y++) {
        UInt8 * row = fbVirt + (UInt64)y * displayW * 4;
        for (UInt32 x = 0; x < displayW; x++) {
            UInt8 * px = row + (UInt64)x * 4;
            // BGRA: b,g,r,a — diagonal gradient blue->yellow with white border
            UInt8 v = (UInt8)(((x + y) * 255) / (displayW + displayH));
            if (x < 4 || y < 4 || x >= displayW-4 || y >= displayH-4) {
                px[0]=255; px[1]=255; px[2]=255; px[3]=255;
            } else if (((x/16 + y/16) & 1) == 0) {
                px[0]=v; px[1]=(UInt8)(v/2); px[2]=255-v; px[3]=255; // blue->red bands
            } else {
                px[0]=255-v; px[1]=v; px[2]=0; px[3]=255;            // yellow->green bands
            }
        }
    }
    IOLog("VGDRV: fb pattern filled\n");

    // ---- RESOURCE_CREATE_2D (id=1, B8G8R8A8) ----
    bzero(ctrlVirt, 2048);
    struct vgpu_create2d * c2 = (struct vgpu_create2d *)ctrlVirt;
    c2->hdr.ctrl_type = VGPU_RESOURCE_CREATE_2D;
    c2->resource_id = 1;
    c2->format = VGPU_FORMAT_B8G8R8A8;
    c2->width = displayW;
    c2->height = displayH;
    r = submitCtrl(sizeof(*c2));
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDRV: CREATE_2D failed %x\n", r); return; }
    IOLog("VGDRV: CREATE_2D ok\n");

    // ---- RESOURCE_ATTACH_BACKING (1 entry = whole fb) ----
    struct vgpu_attach_backing * ab = (struct vgpu_attach_backing *)ctrlVirt;
    bzero(ctrlVirt, 2048);
    ab->hdr.ctrl_type = VGPU_RESOURCE_ATTACH_BACKING;
    ab->resource_id = 1;
    ab->nr_entries = 1;
    volatile UInt8 * entry = ctrlVirt + 32;   // mem_entry right after struct
    *(volatile UInt64 *)(entry + 0) = fbPhys;
    *(volatile UInt32 *)(entry + 8) = fbLen;
    *(volatile UInt32 *)(entry + 12) = 0;
    r = submitCtrl(32 + 16);
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDRV: ATTACH_BACKING failed %x\n", r); return; }
    IOLog("VGDRV: ATTACH_BACKING ok\n");

    // ---- SET_SCANOUT (scanout 0, resource 1, full rect) ----
    struct vgpu_set_scanout * ss = (struct vgpu_set_scanout *)ctrlVirt;
    bzero(ctrlVirt, 2048);
    ss->hdr.ctrl_type = VGPU_SET_SCANOUT;
    ss->r.x = 0; ss->r.y = 0; ss->r.w = displayW; ss->r.h = displayH;
    ss->scanout_id = 0;
    ss->resource_id = 1;
    r = submitCtrl(sizeof(*ss));
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDRV: SET_SCANOUT failed %x\n", r); return; }
    IOLog("VGDRV: SET_SCANOUT ok\n");

    // ---- TRANSFER_TO_HOST_2D (whole fb) ----
    struct vgpu_transfer2d * t2 = (struct vgpu_transfer2d *)ctrlVirt;
    bzero(ctrlVirt, 2048);
    t2->hdr.ctrl_type = VGPU_TRANSFER_TO_HOST_2D;
    t2->r.x = 0; t2->r.y = 0; t2->r.w = displayW; t2->r.h = displayH;
    t2->offset = 0;
    t2->resource_id = 1;
    r = submitCtrl(sizeof(*t2));
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDRV: TRANSFER_2D failed %x\n", r); return; }
    IOLog("VGDRV: TRANSFER_2D ok\n");

    // ---- RESOURCE_FLUSH ----
    struct vgpu_flush * fl = (struct vgpu_flush *)ctrlVirt;
    bzero(ctrlVirt, 2048);
    fl->hdr.ctrl_type = VGPU_RESOURCE_FLUSH;
    fl->r.x = 0; fl->r.y = 0; fl->r.w = displayW; fl->r.h = displayH;
    fl->resource_id = 1;
    r = submitCtrl(sizeof(*fl));
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDRV: FLUSH failed %x\n", r); return; }
    IOLog("VGDRV: === M3 PIPELINE COMPLETE: fb on scanout ===\n");
}

bool VirtIOGPUProbe::start(IOService * provider) {
    if (!IOService::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) { IOLog("VGDRV: not IOPCIDevice\n"); return false; }
    pci->setIOEnable(true);
    pci->setMemoryEnable(true);
    pci->setBusMasterEnable(true);
    IOLog("VGDRV: start() v0.0.7\n");
    if (!parseCaps()) return false;
    if (!handshake()) { IOLog("VGDRV: handshake failed\n"); return false; }
    if (!initControlQueue()) { IOLog("VGDRV: queue init failed\n"); return false; }
    runPipeline();
    registerService();
    return true;
}

void VirtIOGPUProbe::stop(IOService * provider) {
    IOLog("VGDRV: stop()\n");
    IOService::stop(provider);
}
