# M5 finding: the guest already ships Apple's paravirtual Metal driver

Checked on the running Tahoe guest (macOS 26.7.1, x86_64), 2026-10-23.

Present in `/System/Library/Extensions` (x86_64 build, not just ARM):
- `AppleParavirtGPU.kext` - IOKit personality `AppleParavirtGPUControl`
  - `IOPCIMatch = 0xEEEE106B` (PCI vendor 0x106B Apple, device 0xEEEE), `IOProbeScore 1000000`
  - `AcceleratorProperties`: `MetalPluginName = AppleParavirtGPUMetal`, class `AppleParavirtDevice`,
    display pipe + transactions supported
- `AppleParavirtGPUMetal.bundle` - the user-space Metal driver

Not loaded today because no PCI device 106b:eeee exists in the VM. `ioreg` shows 0 AppleParavirt nodes.

## What this changes

The earlier plan assumed we must write the guest accelerator kext and Metal plugin ourselves (M3-M6).
For this device class Apple already ships both. The missing piece is on the HOST: a virtual PCI device 106b:eeee
whose MMIO/ring protocol the stock guest driver speaks. On macOS hosts that device logic lives inside
ParavirtualizedGraphics.framework (QEMU's `apple-gfx-pci` just forwards MMIO to it); it is not built on Linux.

So the work becomes: reverse the AppleParavirtGPU guest <-> device protocol, implement the device in QEMU on Linux,
and translate what it carries (Metal commands, AIR shaders) to Vulkan on the host.

Unknown (not yet measured): how much of the protocol is MMIO registers vs. shared-memory rings, and whether commands
are serialised at the MTLCommandBuffer level. First step is a logging stub device (below), not a guess.

## Next step M5a: MMIO tracer

Add a QEMU PCI device with vendor 0x106B / device 0xEEEE and a few BARs that logs every guest access
(offset, size, value) and answers reads with zeros. Boot Tahoe, see what AppleParavirtGPUControl probes first.
