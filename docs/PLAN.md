# Plan

## Phase 0 - Host survey (read-only)
- [ ] `scripts/check-host.sh` on the reference machine (B550M + Ryzen 7 5800X + RTX 3060)
- [ ] Decide GPU layout: second GPU for the host, or headless host over SSH

## Phase 1 - Track A (passthrough)
- [ ] Install qemu/OVMF, enable IOMMU (`amd_iommu=on iommu=pt`)
- [ ] Isolate the card to vfio-pci (only after Phase 0 decision)
- [ ] macOS guest via OpenCore on AMD (CPU patches), basic display first
- [ ] Install the upstream NVIDIA driver in the guest, report Ampere results upstream
- [ ] Measure: Geekbench Metal / GFXBench against Windows or Linux on the same card

## Phase 2 - Generalise
- [ ] Document per-GPU / per-CPU results table (community reports)
- [ ] Scripted VM definitions per hardware family

## Phase 3 - Track B (research)
- [ ] Prototype virtio-gpu framebuffer kext (no acceleration)
- [ ] Evaluate AIR -> SPIR-V -> Venus path
