/*
 * VirtIOGPUProbe — M2 skeleton: khớp virtio-gpu PCI (1af4:1050), đọc config space,
 * đọc virtio device features qua PCI BAR, in ra kernel log (IOLog).
 * Bài học từ AppleVirtIO: legacy virtio PCI (no MSI-X modern) dùng BAR0 I/O? QEMU virtio-pci
 * modern = BAR 4 (virtio 1.0 common cfg + notify + ISR + device cfg, capability list).
 */
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pci/IOPCIBridge.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/OSDebug.h>

class VirtIOGPUProbe : public IOService {
  OSDeclareDefaultStructors(VirtIOGPUProbe)
private:
  IOPCIDevice * pci;
  IOMemoryMap * commonCfgMap;   // virtio 1.0 common config (cap type 1)
  IOMemoryMap * deviceCfgMap;   // virtio 1.0 device config (cap type 4)
  UInt8 readCapBar();
public:
  virtual bool init(OSDictionary * dict = 0) override;
  virtual void free() override;
  virtual IOService * probe(IOService * provider, SInt32 * score) override;
  virtual bool start(IOService * provider) override;
  virtual void stop(IOService * provider) override;
};


#include <mach/kmod.h>

// Kmod info — required by kmutil for kernel collections
extern "C" kern_return_t kmod_dummy_start(kmod_info_t * ki, void * data);
extern "C" kern_return_t kmod_dummy_stop(kmod_info_t * ki, void * data);

extern "C" kmod_info_t kmod_info;
kmod_info_t kmod_info = {
    NULL,                  // next (patched at load time)
    KMOD_INFO_VERSION,     // info_version
    0,                     // id (assigned at load)
    "com.mylab.VirtIOGPUProbe",
    "0.0.1",
    0,                     // reference_count
    NULL,                  // reference_list
    0,                     // address (vm_address_t)
    0,                     // size
    0,                     // hdr_size
    (kmod_start_func_t*)&kmod_dummy_start,   // start
    (kmod_stop_func_t*)&kmod_dummy_stop,     // stop
};

extern "C" kern_return_t kmod_dummy_start(kmod_info_t * ki, void * data) {
    return KERN_SUCCESS;
}
extern "C" kern_return_t kmod_dummy_stop(kmod_info_t * ki, void * data) {
    return KERN_SUCCESS;
}

OSDefineMetaClassAndStructors(VirtIOGPUProbe, IOService)

bool VirtIOGPUProbe::init(OSDictionary * dict) {
  if (!IOService::init(dict)) return false;
  pci = NULL; commonCfgMap = NULL; deviceCfgMap = NULL;
  return true;
}

void VirtIOGPUProbe::free() {
  IOService::free();
}

IOService * VirtIOGPUProbe::probe(IOService * provider, SInt32 * score) {
  IOService * res = IOService::probe(provider, score);
  IOLog("VirtIOGPUProbe: probe() hit, score=%d\n", (int)*score);
  return res;
}

bool VirtIOGPUProbe::start(IOService * provider) {
  if (!IOService::start(provider)) return false;
  pci = OSDynamicCast(IOPCIDevice, provider);
  if (!pci) { IOLog("VirtIOGPUProbe: provider is not IOPCIDevice\n"); return false; }

  pci->setIOEnable(true);
  pci->setMemoryEnable(true);

  // Parse PCI capability list (virtio 1.0): VENDOR_CAP (0x09) — cap type:
  // 1=common cfg, 2=notify, 3=ISR, 4=device cfg
  UInt8 capOff = pci->configRead8(0x34); // capabilities pointer
  UInt8 barCommon = 0xFF, barDevice = 0xFF;
  UInt64 offCommon = 0, offDevice = 0, lenCommon = 0, lenDevice = 0;
  int guard = 0;
  while (capOff != 0 && capOff != 0xFF && guard++ < 48) {
    UInt8 capID = pci->configRead8(capOff);
    if (capID == 0x09) { // PCI_CAP_ID_VENDOR
      UInt8 capLen  = pci->configRead8(capOff + 2);
      UInt8 cfgType = pci->configRead8(capOff + 3);
      UInt8 bar     = pci->configRead8(capOff + 4);
      UInt32 offLo  = pci->configRead32(capOff + 8);
      UInt32 lenLo  = pci->configRead32(capOff + 12);
      UInt32 offHi  = (capLen >= 20) ? pci->configRead32(capOff + 16) : 0;
      UInt32 lenHi  = (capLen >= 20) ? pci->configRead32(capOff + 20) : 0;
      UInt64 off = offLo | ((UInt64)offHi << 32);
      UInt64 len = lenLo | ((UInt64)lenHi << 32);
      IOLog("VirtIOGPUProbe: vendor cap type=%u bar=%u off=%llu len=%llu\n",
            cfgType, bar, off, len);
      if (cfgType == 1) { barCommon = bar; offCommon = off; lenCommon = len; }
      if (cfgType == 4) { barDevice = bar; offDevice = off; lenDevice = len; }
    }
    capOff = pci->configRead8(capOff + 1);
  }

  if (barCommon != 0xFF) {
    commonCfgMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + barCommon*4,
                                                    kIOMapInhibitCache);
    if (commonCfgMap) {
      volatile UInt32 * base = (volatile UInt32 *)commonCfgMap->getVirtualAddress();
      volatile UInt32 * commonBase = (volatile UInt32 *)((UInt8 *)base + offCommon);
      // virtio_mmio common cfg layout: magic(0) version(1) deviceID(2) vendorID(3)
      UInt32 magic   = commonBase[0];
      UInt32 version = commonBase[1];
      UInt32 devID   = commonBase[2];
      UInt32 vendor  = commonBase[3];
      // device features 64-bit at offsets 4/5 (low/high)
      UInt32 fLo = commonBase[4];
      UInt32 fHi = commonBase[5];
      IOLog("VirtIOGPUProbe: magic=0x%08x version=%u deviceID=0x%04x vendor=0x%04x\n",
            magic, version, devID, vendor);
      IOLog("VirtIOGPUProbe: deviceFeatures=0x%08x_%08x\n", fHi, fLo);
      // VirtIO spec device-independent feature bits: 1..23; virtio-gpu specific 24+
      // VIRTIO_F_VERSION_1 = bit 32; VIRTIO_F_IN_ORDER 35; IOMMU_PLATFORM 33...
    }
  }
  if (deviceCfgMap) {
    volatile UInt32 * base = (volatile UInt32 *)deviceCfgMap->getVirtualAddress();
    volatile UInt32 * devBase = (volatile UInt32 *)((UInt8 *)base + offDevice);
    // virtio_gpu_config: events_read, events_clear, events_flags, num_scanouts
    IOLog("VirtIOGPUProbe: gpu cfg events_read=%u events_clear=%u events_flags=%u num_scanouts=%u\n",
          devBase[0], devBase[1], devBase[2], devBase[3]);
  }

  registerService();
  return true;
}

void VirtIOGPUProbe::stop(IOService * provider) {
  IOService::stop(provider);
}
