# Plan (Track B first: virtual GPU with full acceleration, no passthrough)

Goal: a macOS guest on Linux/KVM that draws on the host GPU through a paravirtual device, so the host keeps its
GPU and several guests can share it.

## Architecture

```
macOS app -> Metal -> [our Metal plugin] -> AIR->SPIR-V translator -> Vulkan commands
   -> [guest kext: virtio-gpu transport, blob memory, fences] -> QEMU virtio-gpu (venus=on)
   -> virglrenderer Venus -> host Vulkan (NVIDIA/AMD/Intel Linux driver) -> GPU
```

Pieces we must write (nothing exists for macOS guests):

| # | Piece | Notes |
|---|---|---|
| 1 | Guest kext: PCI virtio-gpu transport (virtqueues, config, interrupts) | IOKit, DriverKit not enough for DMA/IRQ at this level |
| 2 | Display path: scanout/framebuffer so WindowServer has a screen | first visible milestone |
| 3 | Blob resources + host-visible memory mapping, fences, context_init (Venus capset) | the hard, risky part |
| 4 | Accelerator kext that WindowServer composites through | study how third-party macOS accelerators register |
| 5 | Metal plugin: Metal API -> Vulkan calls over Venus | reuse the LGPL translator idea; no code from PolyForm-licensed projects |

## Target macOS

**Tahoe (26) is the primary target**: it is the last macOS release that runs on x86 (Intel/AMD) hardware, so work done
here stays relevant. Sequoia (15) is the fallback/baseline because third-party GPU drivers are validated there first.
Notes: Tahoe refuses `iMacPro1,1` (use `MacPro7,1` SMBIOS); in KVM the guest CPU is presented as Intel
(`Skylake-Client,-hle,-rtm`), so no AMD kernel patches are needed even on a Ryzen host.

## Milestones (each independently verifiable)

- [x] M0 Host (done, see docs/M0-RESULT.md): QEMU >= 9.2 with `virtio-gpu-gl,venus=on,blob=on,hostmem=...` running a *Linux* guest with Vulkan (vkcube). Proves the host side on this machine.
- M1 macOS guest boots under OpenCore on Ryzen/KVM with basic display (no acceleration).
- M2 Guest kext enumerates virtio-gpu, reads features, negotiates VIRTIO_GPU_F_VENUS/blob.
- M3 Framebuffer via virtio-gpu scanout (resolution changes, no 3D).
- M4 Send one Venus command stream from macOS (vkEnumeratePhysicalDevices equivalent) and read the host reply.
- M5 Metal device visible, one triangle rendered.
- M6 WindowServer composites with acceleration.
- M7 Package, document, generalise to other host GPUs and CPUs.

M0 and M1 are known quantities. M2 to M4 are engineering. M5 to M6 are the real unknowns.

## Honest risk

No public project drives virtio-gpu Venus from macOS. Expect M6 to take months and possibly fail on latency or
synchronisation. Each milestone ships on its own value (M0 and M1 guides are useful by themselves).

## Legal

No Apple binaries or firmware in the repo. Personal research use. Repo licence: PolyForm Noncommercial 1.0.0. Upstream notices are kept in NOTICE when code is reused.
