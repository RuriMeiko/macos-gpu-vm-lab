/*
 * VirtIOGPUProbe v0.0.3-debug — raw dumps to find why BAR4 magic reads as 1.
 * Dumps: config space 0x40-0xFF, all BAR regs, map physical addr, BAR4 first 64 bytes.
 */
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pci/IOPCIBridge.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <mach/kmod.h>

#define VS_ACKNOWLEDGE 1
#define VS_DRIVER      2
#define VS_DRIVER_OK   4
#define VS_FEATURES_OK 8

class VirtIOGPUProbe : public IOService {
    OSDeclareDefaultStructors(VirtIOGPUProbe)
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
    "com.mylab.VirtIOGPUProbe", "0.0.3",
    0, NULL, 0, 0, 0,
    (kmod_start_func_t*)&kmod_dummy_start,
    (kmod_stop_func_t*)&kmod_dummy_stop,
};
extern "C" kern_return_t kmod_dummy_start(kmod_info_t * ki, void * data) { return KERN_SUCCESS; }
extern "C" kern_return_t kmod_dummy_stop(kmod_info_t * ki, void * data)  { return KERN_SUCCESS; }

OSDefineMetaClassAndStructors(VirtIOGPUProbe, IOService)

bool VirtIOGPUProbe::init(OSDictionary * dict) { return IOService::init(dict); }
void VirtIOGPUProbe::free() { IOService::free(); }
IOService * VirtIOGPUProbe::probe(IOService * provider, SInt32 * score) {
    IOService * res = IOService::probe(provider, score);
    IOLog("VGDRV: probe() hit score=%d\n", (int)*score);
    return res;
}

void VirtIOGPUProbe::stop(IOService * provider) { IOLog("VGDRV: stop()\n"); IOService::stop(provider); }

bool VirtIOGPUProbe::start(IOService * provider) {
    if (!IOService::start(provider)) return false;
    IOPCIDevice * pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;
    pci->setIOEnable(true);
    pci->setMemoryEnable(true);
    pci->setBusMasterEnable(true);

    IOMemoryMap * m4 = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + 16,
                                                        kIOMapInhibitCache);
    if (!m4) { IOLog("VGDRV: BAR4 map FAIL\n"); return false; }
    volatile UInt8 * c = (volatile UInt8 *)m4->getVirtualAddress();
    IOLog("VGDRV: WRTEST begin\n");

    // 1. write status@0x25 = 7, readback
    *(volatile UInt8 *)(c + 0x25) = 0x07;
    OSSynchronizeIO();
    UInt8 rb25 = *(volatile UInt8 *)(c + 0x25);
    IOLog("VGDRV: WR status@0x25=07 -> rb=%02x %s\n", rb25, rb25==0x07?"OK":"FAIL");

    // 2. write status@0x24 = 7, readback
    *(volatile UInt8 *)(c + 0x24) = 0x07;
    OSSynchronizeIO();
    UInt8 rb24 = *(volatile UInt8 *)(c + 0x24);
    IOLog("VGDRV: WR @0x24=07 -> rb=%02x %s\n", rb24, rb24==0x07?"OK":"FAIL");

    // 3. write qsel@0x28 = 1, readback
    *(volatile UInt16 *)(c + 0x28) = 1;
    OSSynchronizeIO();
    UInt16 rb28 = *(volatile UInt16 *)(c + 0x28);
    IOLog("VGDRV: WR qsel@0x28=1 -> rb=%04x %s\n", rb28, rb28==1?"OK":"FAIL");

    // 4. write drvfeat@0x18 = 0xABCD, readback
    *(volatile UInt32 *)(c + 0x18) = 0xABCD;
    OSSynchronizeIO();
    UInt32 rb18 = *(volatile UInt32 *)(c + 0x18);
    IOLog("VGDRV: WR drvfeat@0x18=ABCD -> rb=%08x %s\n", rb18, rb18==0xABCD?"OK":"FAIL");

    // 5. write sel@0x14=1 then read @0x10 (should return hi dword)
    *(volatile UInt32 *)(c + 0x14) = 1;
    OSSynchronizeIO();
    UInt32 rb14 = *(volatile UInt32 *)(c + 0x14);
    UInt32 f0 = *(volatile UInt32 *)(c + 0x10);
    IOLog("VGDRV: WR sel@0x14=1 -> rb=%08x; read@0x10=%08x\n", rb14, f0);
    *(volatile UInt32 *)(c + 0x14) = 0;
    OSSynchronizeIO();

    IOLog("VGDRV: WRTEST end\n");
    return true;
}
