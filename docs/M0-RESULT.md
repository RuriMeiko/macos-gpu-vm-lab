# M0 result: Venus works on the host (Linux guest)

Date: 2026-10-22. Host: Ubuntu 26.04, Ryzen 7 5800X, RTX 3060 (proprietary NVIDIA driver), QEMU 10.2.1,
libvirglrenderer 1.2.0 (both from Ubuntu repos, no custom build).

Guest: Ubuntu 24.04 cloud image, Mesa 25.2.8. QEMU device: `virtio-vga-gl,blob=on,venus=on,hostmem=4G`,
display `egl-headless`, memory backend memfd.

```
GPU0: deviceType = DISCRETE_GPU
      deviceName = Virtio-GPU Venus (NVIDIA GeForce RTX 3060)
      driverName = venus   (Mesa 25.2.8)
Venus wire format v1, VK_MESA_venus_protocol v3, renderer instance 1.4.341
```

Notes
- Guest user must be in `render` (or use root) to open `/dev/dri/renderD128`.
- Not yet tested: actual rendering (vkcube needs a display), performance.
- This only proves the host side. The macOS guest still needs its own transport (kext), see PLAN.md M2 onward.

Reproduce: `scripts/m0-install.sh`, then see `m0/run.sh` (`user-data` creates user lab/lab on a NAT-only VM).
