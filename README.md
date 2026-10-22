# macos-gpu-vm-lab

Research notes and tooling for running a **macOS guest with GPU acceleration** on a Linux/KVM host
(AMD Ryzen + NVIDIA GeForce RTX), and for finding out how far that can be generalised to other hardware.

**Status: early research. Nothing here is proven to work yet.**

## Tracks

| Track | Idea | Expected difficulty |
|---|---|---|
| A. VFIO passthrough | Give the guest the whole NVIDIA card; guest uses the community [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver) | Medium. Needs a second GPU (or headless host). Ampere is unverified upstream. |
| B. Shared GPU (paravirtual) | Guest macOS draws through the host's Vulkan (virtio-gpu Venus) | Research-grade. No macOS virtio-gpu driver exists. |

Track A comes first because it is the only one with a realistic path to full acceleration.

## Rules of this repo

- No Apple binaries, installers, recovery images or firmware are ever committed. Users fetch macOS themselves.
- No code is copied from NullMoth's driver (PolyForm Noncommercial). It is referenced by link only.
- Scripts here are read-only unless their name says otherwise; nothing changes your host config without an explicit step.
- Apple's EULA restricts running macOS on non-Apple hardware. Personal research use only; you are responsible for your use.

## Start

```bash
./scripts/check-host.sh
```

Prints CPU virtualisation flags, IOMMU groups, GPU placement and kernel cmdline. Paste the output in an issue to help map hardware.

See [docs/PLAN.md](docs/PLAN.md).
