/*
 * VirtIOGPUDisplay v0.0.8 — M4': full IOFramebuffer driver on virtio-gpu.
 * Plumbing reused verbatim from verified v0.0.7 (submitCtrl etc.).
 * New: subclass IOFramebuffer, register 1280x800@60 ARGB mode, fb aperture,
 * flushWholeFb() (TRANSFER+FLUSH) to publish WindowServer frames to scanout.
 */
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pci/IOPCIBridge.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <mach/kmod.h>

// QEMU 10.2.1 common cfg layout (verified):
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

static bool setDisplayEDIDProps(IOService * fb);
#define VCC_Q_DESCHI 0x24
#define VCC_Q_AVAILLO 0x28
#define VCC_Q_AVAILHI 0x2C
#define VCC_Q_USEDLO  0x30
#define VCC_Q_USEDHI  0x34

#define VS_ACKNOWLEDGE 1
#define VS_DRIVER      2
#define VS_DRIVER_OK   4
#define VS_FEATURES_OK 8
#define VQ_DESC_F_NEXT 1
#define VQ_DESC_F_WRITE 2

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

struct vgpu_ctrl_hdr { UInt32 ctrl_type; UInt32 type_flags; UInt64 fence_id; UInt32 ctx_id; UInt32 padding; };
struct vgpu_rect { UInt32 x, y, w, h; };
struct vgpu_create2d { vgpu_ctrl_hdr hdr; UInt32 resource_id; UInt32 format; UInt32 width; UInt32 height; };
struct vgpu_attach_backing { vgpu_ctrl_hdr hdr; UInt32 resource_id; UInt32 nr_entries; };
struct vgpu_set_scanout { vgpu_ctrl_hdr hdr; vgpu_rect r; UInt32 scanout_id; UInt32 resource_id; };
struct vgpu_transfer2d { vgpu_ctrl_hdr hdr; vgpu_rect r; UInt64 offset; UInt32 resource_id; UInt32 padding; };
struct vgpu_flush { vgpu_ctrl_hdr hdr; vgpu_rect r; UInt32 resource_id; UInt32 padding; };

class VirtIOGPUDisplay : public IOFramebuffer {
    OSDeclareDefaultStructors(VirtIOGPUDisplay)
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
    UInt16   availNext;
    UInt16   descNext;

    UInt8 * ctrlVirt;
    IOBufferMemoryDescriptor * ctrlBuf;
    UInt64 ctrlPhys;

    UInt32 displayW, displayH, displayEnabled;

    UInt8 * fbVirt;
    IOBufferMemoryDescriptor * fbBuf;
    UInt64 fbPhys;
    UInt32 fbLen;

    IODisplayModeID displayModeId;
    bool scanoutAttached;
    UInt16 published;

    IOTimerEventSource * flushTimer;
    bool edidInjected;
public:

    bool parseCaps();
    bool handshake();
    bool initControlQueue();
    UInt32 submitCtrl(UInt32 reqLen);
    bool setupDisplayPipeline();
    void destroyPipeline();
public:
    virtual bool init(OSDictionary * dict = 0) override;
    void flushWholeFb();
    void edidTick();
    virtual void free() override;
    virtual bool start(IOService * provider) override;
    virtual void stop(IOService * provider) override;
    virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService * device) override;

    virtual IOItemCount getDisplayModeCount(void) override;
    virtual const char * getPixelFormats(void) override;
    virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
    virtual IOReturn getDisplayModes(IODisplayModeID * allDisplayModes) override;
    virtual IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
        IODisplayModeInformation * info) override;
    virtual IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
        IOPixelAperture aperture, IOPixelInformation * pixelInfo) override;
    virtual IOReturn getCurrentDisplayMode(IODisplayModeID * displayMode,
        IOIndex * aperture) override;
    virtual IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex aperture) override;
    virtual IOReturn setApertureEnable(IOPixelAperture aperture, IOOptionBits options) override;
    virtual IOReturn setStartupDisplayMode(IODisplayModeID displayMode, IOIndex aperture) override;
    virtual IOReturn getStartupDisplayMode(IODisplayModeID * displayMode, IOIndex * aperture) override;
    virtual IODeviceMemory * getApertureRange(IOPixelAperture aperture) override;
        virtual IOReturn getAppleSense(IOIndex connectIndex, UInt32 * senseType,
        UInt32 * primary, UInt32 * extended, UInt32 * displayType) override;
    virtual IOReturn connectFlags(IOIndex connectIndex, IODisplayModeID displayMode, IOOptionBits * flags) override;
    virtual IOItemCount getConnectionCount(void) override;
    virtual IOReturn enableController(void) override;
    virtual IODeviceMemory * getVRAMRange(void) override;
    virtual bool hasDDCConnect(IOIndex connectIndex) override;
    virtual IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
        IOSelect blockType, IOOptionBits options, UInt8 * data, IOByteCount * length) override;
    virtual IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
        uintptr_t * value) override;
    virtual IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
        uintptr_t value) override;
    virtual IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    virtual IOReturn getAttribute(IOSelect attribute, uintptr_t * value) override;
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
        UInt32 dataWidth, void * data) override;
    virtual IOReturn setCLUTWithEntries(IOColorEntry * colors, UInt32 index,
        UInt32 entries, IOOptionBits options) override;
    virtual IOReturn getTimingInfoForDisplayMode(IODisplayModeID displayMode,
        IOTimingInformation * info) override;
    virtual IOReturn validateDetailedTiming(void * description, IOByteCount descripSize) override;
    virtual IOReturn setDetailedTimings(OSArray * array) override;

    virtual void hideCursor() override;
    virtual void showCursor(IOGPoint * cursorLoc, int frame) override;
    virtual void moveCursor(IOGPoint * cursorLoc, int frame) override;
};

extern "C" kern_return_t kmod_dummy_start(kmod_info_t * ki, void * data);
extern "C" kern_return_t kmod_dummy_stop(kmod_info_t * ki, void * data);
extern "C" kmod_info_t kmod_info;
kmod_info_t kmod_info = {
    NULL, KMOD_INFO_VERSION, 0,
    "com.mylab.VirtIOGPUProbe", "0.0.8",
    0, NULL, 0, 0, 0,
    (kmod_start_func_t*)&kmod_dummy_start,
    (kmod_stop_func_t*)&kmod_dummy_stop,
};
extern "C" kern_return_t kmod_dummy_start(kmod_info_t * ki, void * data) { return KERN_SUCCESS; }
extern "C" kern_return_t kmod_dummy_stop(kmod_info_t * ki, void * data)  { return KERN_SUCCESS; }

OSDefineMetaClassAndStructors(VirtIOGPUDisplay, IOFramebuffer)

bool VirtIOGPUDisplay::init(OSDictionary * dict) {
    if (!IOFramebuffer::init(dict)) return false;
    flushTimer = NULL;
    edidInjected = false;
    pci = NULL; bar4Map = NULL; common = NULL; deviceCfg = NULL; notifyBase = NULL;
    notifyMultiplier = 0; ringVirt = NULL; ringBuf = NULL; ringPhys = 0;
    qsize = 0; availOff = 0; usedOff = 0; availNext = 0; descNext = 0;
    ctrlVirt = NULL; ctrlBuf = NULL; ctrlPhys = 0;
    displayW = displayH = displayEnabled = 0;
    fbVirt = NULL; fbBuf = NULL; fbPhys = 0; fbLen = 0;
    displayModeId = 0; scanoutAttached = false; published = 0;
    return true;
}

void VirtIOGPUDisplay::free() {
    if (flushTimer) { flushTimer->cancelTimeout(); if (getWorkLoop()) getWorkLoop()->removeEventSource(flushTimer); flushTimer->release(); flushTimer = NULL; }
    destroyPipeline();
    if (ctrlBuf) { ctrlBuf->release(); ctrlBuf = NULL; }
    if (ringBuf) { ringBuf->release(); ringBuf = NULL; }
    IOFramebuffer::free();
}

bool VirtIOGPUDisplay::parseCaps() {
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
            IOLog("VGDISP: cap type=%u bar=%u off=%llu len=%llu\n", cfgType, bar, off, len);
            if (cfgType == 1 && bar == 4 && off == 0)         haveCommon = true;
            else if (cfgType == 4 && bar == 4 && off == 8192) haveDevice = true;
        }
        capOff = pci->configRead8(capOff + 1);
    }
    if (!haveCommon || !haveDevice || !haveNotify) {
        IOLog("VGDISP: caps incomplete c=%d d=%d n=%d\n", haveCommon, haveDevice, haveNotify);
        return false;
    }
    bar4Map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + 16,
                                               kIOMapInhibitCache);
    if (!bar4Map) { IOLog("VGDISP: BAR4 map failed\n"); return false; }
    volatile UInt8 * base = (volatile UInt8 *)bar4Map->getVirtualAddress();
    common     = base + 0;
    deviceCfg  = base + 8192;
    notifyBase = base + notifyOff;
    IOLog("VGDISP: BAR4 mapped virt=%p size=%llu mult=%u\n",
          (void*)base, bar4Map->getLength(), notifyMultiplier);
    return true;
}

bool VirtIOGPUDisplay::handshake() {
    *(volatile UInt8 *)(common + VCC_STATUS) = 0;
    OSSynchronizeIO();
    *(volatile UInt8 *)(common + VCC_STATUS) = VS_ACKNOWLEDGE | VS_DRIVER;
    OSSynchronizeIO();
    *(volatile UInt32 *)(common + VCC_DFSELECT) = 1;
    UInt32 fHi = *(volatile UInt32 *)(common + VCC_DF);
    *(volatile UInt32 *)(common + VCC_DFSELECT) = 0;
    UInt32 fLo = *(volatile UInt32 *)(common + VCC_DF);
    IOLog("VGDISP: devfeat hi=%08x lo=%08x\n", fHi, fLo);

    *(volatile UInt32 *)(common + VCC_GFSELECT) = 1;
    *(volatile UInt32 *)(common + VCC_GF) = fHi & 1;
    *(volatile UInt32 *)(common + VCC_GFSELECT) = 0;
    *(volatile UInt32 *)(common + VCC_GF) = 0;
    OSSynchronizeIO();

    *(volatile UInt8 *)(common + VCC_STATUS) =
        *(volatile UInt8 *)(common + VCC_STATUS) | VS_FEATURES_OK;
    OSSynchronizeIO();
    UInt8 st = *(volatile UInt8 *)(common + VCC_STATUS);
    IOLog("VGDISP: status after FEAT_OK=%u\n", st);
    if (!(st & VS_FEATURES_OK)) return false;
    return true;
}

bool VirtIOGPUDisplay::initControlQueue() {
    UInt16 nq = *(volatile UInt16 *)(common + VCC_NUMQ);
    IOLog("VGDISP: num_queues=%u\n", nq);
    *(volatile UInt16 *)(common + VCC_Q_SELECT) = 0;
    OSSynchronizeIO();
    UInt16 qsizeDev = *(volatile UInt16 *)(common + VCC_Q_SIZE);
    UInt16 qNotify  = *(volatile UInt16 *)(common + VCC_Q_NOFF);
    IOLog("VGDISP: q0 size=%u noff=%u\n", qsizeDev, qNotify);
    if (qsizeDev == 0) return false;
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
    if (!ringBuf) { IOLog("VGDISP: ring alloc failed\n"); return false; }
    IOByteCount segLen = 0;
    ringPhys = ringBuf->getPhysicalSegment(0, &segLen, 0);
    ringVirt = (UInt8 *)ringBuf->getBytesNoCopy();
    bzero(ringVirt, total);
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
    IOLog("VGDISP: q0 enable readback=%u\n", *(volatile UInt16 *)(common + VCC_Q_ENABLE));

    UInt8 st = *(volatile UInt8 *)(common + VCC_STATUS);
    *(volatile UInt8 *)(common + VCC_STATUS) = st | VS_DRIVER_OK;
    OSSynchronizeIO();
    IOLog("VGDISP: DRIVER_OK set, status=%u\n", *(volatile UInt8 *)(common + VCC_STATUS));

    ctrlBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, 2048,
        0xFFFFFFFFFFFFF000ULL);
    if (!ctrlBuf) return false;
    IOByteCount cLen = 0;
    ctrlPhys = ctrlBuf->getPhysicalSegment(0, &cLen, 0);
    ctrlVirt = (UInt8 *)ctrlBuf->getBytesNoCopy();
    bzero(ctrlVirt, 2048);
    IOLog("VGDISP: ctrlBuf virt=%p phys=%llx\n", ctrlVirt, ctrlPhys);
    return true;
}

UInt32 VirtIOGPUDisplay::submitCtrl(UInt32 reqLen) {
    if (!ringVirt) return 0;
    volatile UInt8 * desc  = ringVirt;
    volatile UInt8 * avail = ringVirt + availOff;
    volatile UInt8 * used  = ringVirt + usedOff;

    UInt16 d0 = descNext;
    UInt16 d1 = (UInt16)((d0 + 1) % qsize);
    descNext = (UInt16)((d0 + 2) % qsize);

    *(volatile UInt64 *)(desc + d0*16 + 0)  = ctrlPhys;
    *(volatile UInt32 *)(desc + d0*16 + 8)  = reqLen;
    *(volatile UInt16 *)(desc + d0*16 + 12) = VQ_DESC_F_NEXT;
    *(volatile UInt16 *)(desc + d0*16 + 14) = d1;
    *(volatile UInt64 *)(desc + d1*16 + 0)  = ctrlPhys + 1024;
    *(volatile UInt32 *)(desc + d1*16 + 8)  = 512;
    *(volatile UInt16 *)(desc + d1*16 + 12) = VQ_DESC_F_WRITE;
    *(volatile UInt16 *)(desc + d1*16 + 14) = 0;

    *(volatile UInt16 *)(avail + 4 + 2 * (published % qsize)) = d0;
    OSSynchronizeIO();
    *(volatile UInt16 *)(avail + 2) = (UInt16)(published + 1);
    OSSynchronizeIO();

    UInt16 qNotify = *(volatile UInt16 *)(common + VCC_Q_NOFF);
    volatile UInt16 * doorbell =
        (volatile UInt16 *)(notifyBase + (UInt64)qNotify * notifyMultiplier);
    *doorbell = 0;
    OSSynchronizeIO();

    for (int i = 0; i < 2000; i++) {
        OSSynchronizeIO();
        UInt16 uIdx = *(volatile UInt16 *)(used + 2);
        if (uIdx == (UInt16)(published + 1)) {
            volatile UInt8 * entry = used + 4 + 8 * (published % qsize);
            UInt32 eLen = *(volatile UInt32 *)(entry + 4);
            volatile UInt8 * resp = ctrlVirt + 1024;
            UInt32 rType = *(volatile UInt32 *)(resp + 0);
            published++;
            IOLog("VGDISP: ctrl len=%u resp_type=%x\n", eLen, rType);
            return rType;
        }
        IODelay(1000);
    }
    published++;
    IOLog("VGDISP: ctrl TIMEOUT (reqLen=%u)\n", reqLen);
    return 0;
}

bool VirtIOGPUDisplay::setupDisplayPipeline() {
    bzero(ctrlVirt, 2048);
    struct vgpu_ctrl_hdr * req = (struct vgpu_ctrl_hdr *)ctrlVirt;
    req->ctrl_type = VGPU_GET_DISPLAY_INFO;
    UInt32 r = submitCtrl(24);
    if (r != VGPU_RESP_OK_DISPLAY_INFO) {
        IOLog("VGDISP: GET_DISPLAY_INFO failed type=%x\n", r);
        return false;
    }
    volatile UInt8 * resp = ctrlVirt + 1024;
    displayEnabled = 0;
    for (int d = 0; d < 8; d++) {
        volatile UInt32 * de = (volatile UInt32 *)(resp + 24 + d*24);
        if (de[4]) {
            displayEnabled = 1;
            displayW = de[2]; displayH = de[3];
            IOLog("VGDISP: display[%d] rect=(%u,%u %ux%u) enabled\n", d, de[0], de[1], de[2], de[3]);
            break;
        }
    }
    if (!displayEnabled) { IOLog("VGDISP: no enabled display\n"); return false; }
    IOLog("VGDISP: display info %ux%u\n", displayW, displayH);

    fbLen = displayW * displayH * 4;
    fbBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, fbLen,
        0xFFFFFFFFFFFFF000ULL);
    if (!fbBuf) { IOLog("VGDISP: fb alloc failed\n"); return false; }
    IOByteCount fbSeg = 0;
    fbPhys = fbBuf->getPhysicalSegment(0, &fbSeg, 0);
    fbVirt = (UInt8 *)fbBuf->getBytesNoCopy();
    if ((UInt64)fbSeg < fbLen) { IOLog("VGDISP: fb not contiguous\n"); return false; }
    bzero(fbVirt, fbLen);
    IOLog("VGDISP: fb phys=%llx len=%u\n", fbPhys, fbLen);

    bzero(ctrlVirt, 2048);
    struct vgpu_create2d * c2 = (struct vgpu_create2d *)ctrlVirt;
    c2->hdr.ctrl_type = VGPU_RESOURCE_CREATE_2D;
    c2->resource_id = 1;
    c2->format = VGPU_FORMAT_B8G8R8A8;
    c2->width = displayW;
    c2->height = displayH;
    r = submitCtrl(sizeof(*c2));
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDISP: CREATE_2D failed %x\n", r); return false; }

    struct vgpu_attach_backing * ab = (struct vgpu_attach_backing *)ctrlVirt;
    bzero(ctrlVirt, 2048);
    ab->hdr.ctrl_type = VGPU_RESOURCE_ATTACH_BACKING;
    ab->resource_id = 1;
    ab->nr_entries = 1;
    volatile UInt8 * entry = ctrlVirt + 32;
    *(volatile UInt64 *)(entry + 0) = fbPhys;
    *(volatile UInt32 *)(entry + 8) = fbLen;
    *(volatile UInt32 *)(entry + 12) = 0;
    r = submitCtrl(32 + 16);
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDISP: ATTACH_BACKING failed %x\n", r); return false; }

    struct vgpu_set_scanout * ss = (struct vgpu_set_scanout *)ctrlVirt;
    bzero(ctrlVirt, 2048);
    ss->hdr.ctrl_type = VGPU_SET_SCANOUT;
    ss->r.x = 0; ss->r.y = 0; ss->r.w = displayW; ss->r.h = displayH;
    ss->scanout_id = 0;
    ss->resource_id = 1;
    r = submitCtrl(sizeof(*ss));
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDISP: SET_SCANOUT failed %x\n", r); return false; }
    scanoutAttached = true;
    IOLog("VGDISP: scanout attached\n");

    // Diagnostic pattern fill so the scanout shows something until
    // WindowServer takes over rendering:
    for (UInt32 y = 0; y < displayH; y++) {
        UInt8 * row = fbVirt + (UInt64)y * displayW * 4;
        for (UInt32 x = 0; x < displayW; x++) {
            UInt8 * px = row + (UInt64)x * 4;
            UInt8 v = (UInt8)(((x + y) * 255) / (displayW + displayH));
            if (x < 4 || y < 4 || x >= displayW-4 || y >= displayH-4) {
                px[0]=255; px[1]=255; px[2]=255; px[3]=255;
            } else if (((x/16 + y/16) & 1) == 0) {
                px[0]=v; px[1]=(UInt8)(v/2); px[2]=255-v; px[3]=255;
            } else {
                px[0]=(UInt8)(255-v); px[1]=v; px[2]=0; px[3]=255;
            }
        }
    }
    flushWholeFb();
    IOLog("VGDISP: pattern flushed\n");
    return true;
}

void VirtIOGPUDisplay::flushWholeFb() {
    if (!scanoutAttached) return;
    struct vgpu_transfer2d * t2 = (struct vgpu_transfer2d *)ctrlVirt;
    bzero(ctrlVirt, 2048);
    t2->hdr.ctrl_type = VGPU_TRANSFER_TO_HOST_2D;
    t2->r.x = 0; t2->r.y = 0; t2->r.w = displayW; t2->r.h = displayH;
    t2->offset = 0;
    t2->resource_id = 1;
    UInt32 r = submitCtrl(sizeof(*t2));
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDISP: TRANSFER_2D failed %x\n", r); return; }
    struct vgpu_flush * fl = (struct vgpu_flush *)ctrlVirt;
    bzero(ctrlVirt, 2048);
    fl->hdr.ctrl_type = VGPU_RESOURCE_FLUSH;
    fl->r.x = 0; fl->r.y = 0; fl->r.w = displayW; fl->r.h = displayH;
    fl->resource_id = 1;
    r = submitCtrl(sizeof(*fl));
    if (r != VGPU_RESP_OK_NODATA) { IOLog("VGDISP: FLUSH failed %x\n", r); }
}

void VirtIOGPUDisplay::destroyPipeline() {
    if (!scanoutAttached || !ctrlVirt) return;
    struct vgpu_ctrl_hdr * req = (struct vgpu_ctrl_hdr *)ctrlVirt;
    bzero(ctrlVirt, 2048);
    req->ctrl_type = VGPU_RESOURCE_UNREF;
    *(UInt32 *)((UInt8*)req + 24) = 1;
    submitCtrl(28);
    scanoutAttached = false;
}

bool VirtIOGPUDisplay::start(IOService * provider) {
    if (!IOFramebuffer::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) { IOLog("VGDISP: not IOPCIDevice\n"); return false; }
    pci->setIOEnable(true);
    pci->setMemoryEnable(true);
    pci->setBusMasterEnable(true);
    IOLog("VGDISP: start() v0.0.8\n");
    if (!parseCaps()) return false;
    if (!handshake()) { IOLog("VGDISP: handshake failed\n"); return false; }
    if (!initControlQueue()) { IOLog("VGDISP: queue init failed\n"); return false; }
    if (!setupDisplayPipeline()) { IOLog("VGDISP: pipeline failed\n"); return false; }

    displayModeId = 1;
    setDisplayMode(displayModeId, 0);
    registerService();
    IOLog("VGDISP: === v0.0.8 IOFramebuffer %ux%u started ===\n", displayW, displayH);
    return true;
}

void VirtIOGPUDisplay::stop(IOService * provider) {
    IOLog("VGDISP: stop()\n");
    destroyPipeline();
    IOFramebuffer::stop(provider);
}

IOReturn VirtIOGPUDisplay::setPowerState(unsigned long ordinal, IOService * device) {
    return kIOPMAckImplied;
}

UInt64 VirtIOGPUDisplay::getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    return 0;
}

const char * VirtIOGPUDisplay::getPixelFormats(void) {
    return IO32BitDirectPixels;
}

IOItemCount VirtIOGPUDisplay::getDisplayModeCount(void) {
    return 1;
}

IOReturn VirtIOGPUDisplay::getDisplayModes(IODisplayModeID * allDisplayModes) {
    allDisplayModes[0] = displayModeId;
    return 1;
}

IOReturn VirtIOGPUDisplay::getInformationForDisplayMode(IODisplayModeID mode,
    IODisplayModeInformation * info) {
    if (mode != displayModeId) return kIOReturnUnsupportedMode;
    memset(info, 0, sizeof(*info));
    info->nominalWidth = displayW;
    info->nominalHeight = displayH;
    info->refreshRate = 60 << 16;
    info->maxDepthIndex = 1;
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::getPixelInformation(IODisplayModeID mode, IOIndex depth,
    IOPixelAperture aperture, IOPixelInformation * pixelInfo) {
    if (mode != displayModeId || depth != 0) return kIOReturnUnsupportedMode;
    memset(pixelInfo, 0, sizeof(*pixelInfo));
    pixelInfo->activeWidth = displayW;
    pixelInfo->activeHeight = displayH;
    pixelInfo->bytesPerRow = displayW * 4;
    pixelInfo->bitsPerPixel = 32;
    pixelInfo->pixelType = kIORGBDirectPixels;
    pixelInfo->componentCount = 3;
    pixelInfo->bitsPerComponent = 8;
    // BGRA byte layout: little-endian 32-bit = 0xAARRGGBB → masks R/G/B.
    pixelInfo->componentMasks[0] = 0x00ff0000;  // R
    pixelInfo->componentMasks[1] = 0x0000ff00;  // G
    pixelInfo->componentMasks[2] = 0x000000ff;  // B
    pixelInfo->componentMasks[3] = 0xff000000;  // alpha (4th component? kept)
    pixelInfo->flags = 0;
    strlcpy(pixelInfo->pixelFormat, IO32BitDirectPixels, sizeof(pixelInfo->pixelFormat));
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::getCurrentDisplayMode(IODisplayModeID * mode, IOIndex * aperture) {
    *mode = displayModeId;
    *aperture = 0;
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::setDisplayMode(IODisplayModeID mode, IOIndex aperture) {
    IOLog("VGDISP: setDisplayMode id=%u aperture=%u\n", (unsigned)mode, (unsigned)aperture);
    if (mode != displayModeId) return kIOReturnUnsupportedMode;
    edidTick();
    flushWholeFb();
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::setApertureEnable(IOPixelAperture aperture, IOOptionBits options) {
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::setStartupDisplayMode(IODisplayModeID mode, IOIndex aperture) {
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::getStartupDisplayMode(IODisplayModeID * mode, IOIndex * aperture) {
    *mode = displayModeId;
    *aperture = 0;
    return kIOReturnSuccess;
}

IODeviceMemory * VirtIOGPUDisplay::getApertureRange(IOPixelAperture aperture) {
    IOLog("VGDISP: getApertureRange aperture=%u\n", (unsigned)aperture);
    if (!fbBuf) return NULL;
    // Create an IODeviceMemory over our fb physical range (fresh instance per call).
    IOByteCount seg = 0;
    addr64_t phys = fbBuf->getPhysicalSegment(0, &seg, 0);
    return IODeviceMemory::withRange((IOPhysicalAddress)phys, seg);
}

IOReturn VirtIOGPUDisplay::getAppleSense(IOIndex connectIndex, UInt32 * senseType,
    UInt32 * primary, UInt32 * extended, UInt32 * displayType) {
    if (senseType) *senseType = 0;
    if (primary) *primary = 1;
    if (extended) *extended = 0;
    if (displayType) *displayType = 0;
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::connectFlags(IOIndex connectIndex, IODisplayModeID displayMode,
    IOOptionBits * flags) {
    if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

IOItemCount VirtIOGPUDisplay::getConnectionCount(void) {
    return 1;
}

static const UInt8 gHmlEDID[128] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x21, 0xAC, 0x17, 0x71, 0x22, 0x26, 0x1A, 0x18,
    0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEC, 0xC0, 0x61, 0x30, 0x54, 0x31, 0x58,
    0x35, 0x54, 0x54, 0x35, 0x54, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1B, 0xBC, 0x00, 0x05, 0x20, 0x03, 0xA0, 0x18, 0x00, 0x00,
    0x30, 0x20, 0x86, 0x8A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x3C, 0x3C, 0x32,
    0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x48,
    0x4D, 0x4C, 0x20, 0x56, 0x69, 0x72, 0x74, 0x75, 0x61, 0x6C, 0x47, 0x50, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53,
};

static bool setDisplayEDIDProps(IOService * fb) {
    // Walk children: IODisplayConnect -> AppleDisplay, inject EDID identity.
    IOLog("VGDISP: edid: fn enter\n");
    // Deterministic registry lookup for our display0 under this fb.
    IORegistryEntry * connect = IORegistryEntry::fromPath(
        "IOService:/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/S30@6/VirtIOGPUDisplay/display0",
        gIOServicePlane);
    if (!connect) { IOLog("VGDISP: edid: display0 path FAIL\n"); return false; }
    IOLog("VGDISP: edid: display0 via path OK\n");
    OSData * edidData = OSData::withBytes(gHmlEDID, 128);
    if (edidData) {
        connect->setProperty(kIODisplayEDIDKey, edidData);
        edidData->release();
    }
    connect->setProperty(kDisplayVendorID, (unsigned long long)0xBC0B, 32);
    connect->setProperty(kDisplayProductID, (unsigned long long)0x7117, 32);
    OSDictionary * nm = OSDictionary::withCapacity(1);
    if (nm) {
        const OSSymbol * en = OSSymbol::withCString("en");
        OSString * v = OSString::withCString("HML VirtualGPU");
        if (en && v) nm->setObject(en, v);
        if (en) en->release();
        if (v) v->release();
        connect->setProperty(kDisplayProductName, nm);
        nm->release();
    }
    connect->release();
    IOLog("VGDISP: EDID identity injected on IODisplayConnect\n");
    return true;
}

static void timerFlushThunk(OSObject * owner, IOTimerEventSource * sender) {
    VirtIOGPUDisplay * self = (VirtIOGPUDisplay *) owner;
    sender->setTimeoutMS(66);
    sender->enable();
    IOLog("VGDISP: timer tick\n");
    self->flushWholeFb();
}

IOReturn VirtIOGPUDisplay::enableController(void) {
    IOLog("VGDISP: enableController\n");
    flushWholeFb();
    if (!flushTimer) {
        flushTimer = IOTimerEventSource::timerEventSource(this, timerFlushThunk);
        if (flushTimer && getWorkLoop()) {
            getWorkLoop()->addEventSource(flushTimer);
            flushTimer->setTimeoutMS(66);
            flushTimer->enable();
            IOLog("VGDISP: periodic flush timer 66ms\n");
        } else {
            IOLog("VGDISP: flush timer init failed\n");
        }
    }
    setDisplayEDIDProps(this);
    return kIOReturnSuccess;
}

IODeviceMemory * VirtIOGPUDisplay::getVRAMRange(void) {
    return getApertureRange(kIOFBSystemAperture);
}

bool VirtIOGPUDisplay::hasDDCConnect(IOIndex connectIndex) {
    IOLog("VGDISP: hasDDCConnect idx=%u -> true\n", (unsigned)connectIndex);
    return true;
}

IOReturn VirtIOGPUDisplay::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
    IOSelect blockType, IOOptionBits options, UInt8 * data, IOByteCount * length) {
    IOLog("VGDISP: getDDCBlock block=%u type=%u\n", blockNumber, (unsigned)blockType);
    if (blockNumber != 1 || blockType != 0 /* kIODDCBlockTypeEDID */)
        return kIOReturnUnsupported;
    if (!data || !length || *length < 128)
        return kIOReturnBadArgument;
    bcopy(gHmlEDID, data, 128);
    *length = 128;
    return kIOReturnSuccess;
}

void VirtIOGPUDisplay::edidTick(void) {
    IOLog("VGDISP: edidTick called (injected=%d)\n", edidInjected ? 1 : 0);
    if (edidInjected) return;
    edidInjected = setDisplayEDIDProps(this);
}

IOReturn VirtIOGPUDisplay::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
    uintptr_t * value) {
    if (!edidInjected) edidTick();
    switch (attribute) {
        case 'cena':  // kConnectionCheckEnable — report connect enabled → online
            if (value) *value = 1;
            return kIOReturnSuccess;
        case 'enab':  // kConnectionEnable — always connected/enabled
            if (value) *value = 1;
            return kIOReturnSuccess;
        case 'chng':  // kConnectionChanged — nothing pending
            if (value) *value = 0;
            return kIOReturnSuccess;
        case 'asns':  // kConnectionSupportsAppleSense
            if (value) *value = 1;
            return kIOReturnSuccess;
        case 'hddc':  // kConnectionSupportsHLDDCSense — DDC/EDID supported
            return kIOReturnSuccess;
        case 'prob':  // kConnectionProbe — display detection moment
            edidTick();
            if (value) *value = 1;   // display detected/present
            return kIOReturnSuccess;
        default:
            IOLog("VGDISP: getAttrForConn 0x%x idx=%u -> 0\n", (unsigned)attribute, (unsigned)connectIndex);
            if (value) *value = 0;
            return kIOReturnSuccess;
    }
}

IOReturn VirtIOGPUDisplay::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
    uintptr_t value) {
    IOLog("VGDISP: setAttrForConn 0x%x idx=%u val=%lu\n", (unsigned)attribute, (unsigned)connectIndex, (unsigned long)value);
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::setAttribute(IOSelect attribute, uintptr_t value) {
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::getAttribute(IOSelect attribute, uintptr_t * value) {
    if (value) *value = 0;
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::setGammaTable(UInt32 channelCount, UInt32 dataCount,
    UInt32 dataWidth, void * data) {
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::setCLUTWithEntries(IOColorEntry * colors, UInt32 index,
    UInt32 entries, IOOptionBits options) {
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::getTimingInfoForDisplayMode(IODisplayModeID mode,
    IOTimingInformation * info) {
    memset(info, 0, sizeof(*info));
    info->appleTimingID = 42; /* kIOTimingIDApple_FixedRateLCD */
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUDisplay::validateDetailedTiming(void * description, IOByteCount descripSize) {
    return kIOReturnUnsupported;
}

IOReturn VirtIOGPUDisplay::setDetailedTimings(OSArray * array) {
    return kIOReturnUnsupported;
}

void VirtIOGPUDisplay::hideCursor() {}
void VirtIOGPUDisplay::showCursor(IOGPoint * cursorLoc, int frame) {}
void VirtIOGPUDisplay::moveCursor(IOGPoint * cursorLoc, int frame) {}
