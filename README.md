# macos-gpu-vm-lab

Research notes and tooling for running a **macOS guest with GPU acceleration** on a Linux/KVM host
(AMD Ryzen + NVIDIA GeForce RTX), and for finding out how far that can be generalised to other hardware.

**Status: early research. Nothing here is proven to work yet.**

## Approach

Main track: **paravirtual GPU, no passthrough**. The guest draws through a virtio-gpu device using Venus
(Vulkan over virtio) while the host keeps its GPU. Passthrough is only a fallback/baseline.
See [docs/PLAN.md](docs/PLAN.md) for architecture and milestones (M0 to M7).

## Rules of this repo

- No Apple binaries, installers, recovery images or firmware are ever committed. Users fetch macOS themselves.
- Licence: PolyForm Noncommercial 1.0.0 (personal, research, educational use). This allows reusing code from NullMoth's
  noncommercial driver as long as its Required Notice is kept (see NOTICE); its LGPL translator stays LGPL.
- Scripts here are read-only unless their name says otherwise; nothing changes your host config without an explicit step.
- Apple's EULA restricts running macOS on non-Apple hardware. Personal research use only; you are responsible for your use.

## Start

```bash
./scripts/check-host.sh
```

Prints CPU virtualisation flags, IOMMU groups, GPU placement and kernel cmdline. Paste the output in an issue to help map hardware.

See [docs/PLAN.md](docs/PLAN.md).
