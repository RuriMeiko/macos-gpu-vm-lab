/*
 * VirtIOGPUProbe v0.0.5 — M3: correct common-cfg offsets per virtio 1.0 spec
 * (status u8@0x25, q_enable@0x32, q_desc@0x38, q_avail@0x40, q_used@0x48),
 * full handshake, control queue init, GET_DISPLAY_INFO roundtrip.
 * Magic check skipped (QEMU 10.2.1 reads back 1 at common@0; PCI ID already proves device).
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

#define VGPU_GET_DISPLAY_INFO  0x0100
#define VGPU_RESP_DISPLAY_INFO 0x1100

struct vgpu_ctrl_hdr {
    UInt32 ctrl_type;
    UInt32 type_flags;
    UInt64 fence_id;
    UInt32 ctx_id;
    UInt32 padding;
};  // 24 bytes

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

    bool parseCaps();
    bool handshake();
    bool initControlQueue();
    void sendGetDisplayInfo();
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
    "com.mylab.VirtIOGPUProbe", "0.0.5",
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
    qsize = 0; availOff = 0; usedOff = 0;
    return true;
}

void VirtIOGPUProbe::free() { IOService::free(); }

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
    // Reset then ACK|DRIVER
    *(volatile UInt8 *)(common + VCC_STATUS) = 0;
    OSSynchronizeIO();
    *(volatile UInt8 *)(common + VCC_STATUS) = VS_ACKNOWLEDGE | VS_DRIVER;
    OSSynchronizeIO();
    IOLog("VGDRV: status after ACK|DRIVER=%u\n", *(volatile UInt8 *)(common + VCC_STATUS));

    // device features: DFSELECT=1 -> DF = high half
    *(volatile UInt32 *)(common + VCC_DFSELECT) = 1;
    UInt32 fHi = *(volatile UInt32 *)(common + VCC_DF);
    *(volatile UInt32 *)(common + VCC_DFSELECT) = 0;
    UInt32 fLo = *(volatile UInt32 *)(common + VCC_DF);
    IOLog("VGDRV: devfeat hi=%08x lo=%08x\n", fHi, fLo);

    // driver features: GFSELECT=1 -> GF = high half bit0 (VIRTIO_F_VERSION_1)
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
    return true;
}

void VirtIOGPUProbe::sendGetDisplayInfo() {
    IOBufferMemoryDescriptor * reqBuf =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous, 1024,
            0xFFFFFFFFFFFFF000ULL);
    if (!reqBuf) { IOLog("VGDRV: reqBuf alloc failed\n"); return; }
    IOByteCount segLen = 0;
    UInt64 reqPhys = reqBuf->getPhysicalSegment(0, &segLen, 0);
    UInt8 * reqVirt = (UInt8 *)reqBuf->getBytesNoCopy();
    bzero(reqVirt, 1024);
    IOLog("VGDRV: reqBuf virt=%p phys=%llx\n", reqVirt, reqPhys);

    struct vgpu_ctrl_hdr * req = (struct vgpu_ctrl_hdr *)reqVirt;
    req->ctrl_type = VGPU_GET_DISPLAY_INFO;

    volatile UInt8 * desc  = ringVirt;
    volatile UInt8 * avail = ringVirt + availOff;
    volatile UInt8 * used  = ringVirt + usedOff;

    *(volatile UInt64 *)(desc + 0)  = reqPhys;
    *(volatile UInt32 *)(desc + 8)  = 24;
    *(volatile UInt16 *)(desc + 12) = 1; // VRING_DESC_F_NEXT
    *(volatile UInt16 *)(desc + 14) = 1;
    *(volatile UInt64 *)(desc + 16) = reqPhys + 64;
    *(volatile UInt32 *)(desc + 24) = 512; // resp 408 max
    *(volatile UInt16 *)(desc + 28) = VQ_DESC_F_WRITE;
    *(volatile UInt16 *)(desc + 30) = 0;

    *(volatile UInt16 *)(avail + 0) = 0;    // flags
    *(volatile UInt16 *)(avail + 2) = 1;    // idx
    *(volatile UInt16 *)(avail + 4) = 0;    // ring[0]
    OSSynchronizeIO();

    UInt16 qNotify = *(volatile UInt16 *)(common + VCC_Q_NOFF);
    volatile UInt16 * doorbell =
        (volatile UInt16 *)(notifyBase + (UInt64)qNotify * notifyMultiplier);
    IOLog("VGDRV: kick doorbell (mult=%u qNotify=%u)\n", notifyMultiplier, qNotify);
    *doorbell = 0;
    OSSynchronizeIO();

    for (int i = 0; i < 2000; i++) {
        OSSynchronizeIO();
        UInt16 usedIdx = *(volatile UInt16 *)(used + 2);
        if (usedIdx >= 1) {
            volatile UInt8 * entry = used + 8;
            UInt32 eIdx = *(volatile UInt32 *)(entry + 0);
            UInt32 eLen = *(volatile UInt32 *)(entry + 4);
            IOLog("VGDRV: USED idx=%u len=%u\n", eIdx, eLen);
            volatile UInt8 * resp = reqVirt + 64;
            UInt32 rType = *(volatile UInt32 *)(resp + 0);
            IOLog("VGDRV: RESP type=%x (want %x)\n", rType, VGPU_RESP_DISPLAY_INFO);
            if (rType == VGPU_RESP_DISPLAY_INFO) {
                for (int d = 0; d < 8; d++) {
                    volatile UInt32 * de = (volatile UInt32 *)(resp + 24 + d*24);
                    UInt32 x = de[0], y = de[1], w = de[2], h = de[3];
                    UInt32 enabled = de[4];
                    if (enabled) IOLog("VGDRV: display[%d] enabled=%u rect=(%u,%u %ux%u)\n",
                                       d, enabled, x, y, w, h);
                }
                IOLog("VGDRV: === GET_DISPLAY_INFO SUCCESS ===\n");
            }
            break;
        }
        IODelay(1000);
        if (i == 1999) IOLog("VGDRV: TIMEOUT waiting for device\n");
    }
}

bool VirtIOGPUProbe::start(IOService * provider) {
    if (!IOService::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) { IOLog("VGDRV: not IOPCIDevice\n"); return false; }
    pci->setIOEnable(true);
    pci->setMemoryEnable(true);
    pci->setBusMasterEnable(true);
    IOLog("VGDRV: start() v0.0.6\n");
    if (!parseCaps()) return false;
    if (!handshake()) { IOLog("VGDRV: handshake failed\n"); return false; }
    if (!initControlQueue()) { IOLog("VGDRV: queue init failed\n"); return false; }
    sendGetDisplayInfo();
    registerService();
    return true;
}

void VirtIOGPUProbe::stop(IOService * provider) {
    IOLog("VGDRV: stop()\n");
    IOService::stop(provider);
}
