
## M4-NOTES update (2026-10-23 01:xx-02:3x)

### v0.0.20-0.0.27 tiến trình
- EDID pipeline HOÀN TOÀN: hasDDCConnect=true + getDDCBlock trả gHmlEDID
  (kIODDCBlockTypeEDID block 1) → IODisplay tự đọc + parse → 
  IODisplayPrefsKey có EDID hash + IODisplayEDID trên AppleDisplay ✓
  (cách CHUẨN Apple, props không phải set tay vào registry)
- getTimingInfoForDisplayMode → appleTimingID=42 (FixedRateLCD)
- getVRAMRange override
- v0.0.27 timer repeat OK: setTimeoutMS+enable trong callback → 
  1031 ticks/6m (66ms)
- Quan trọng: CGXConstructVirtualFramebuffer offline + IOPresentment
  'Unable to acquire map' là NORMAL (cả GUI vmware boot cũng có) —
  KHÔNG phải blocker
- ROOT INSIGHT: WindowServer ĐANG VẼ vào fb kext (fb white/gradient,
  không còn pattern bytes 0xce3100ff) — scanout vẫn stale pattern
- Còn lại: flushWholeFb không emit TRANSFER_TO_HOST_2D (56B)/RESOURCE
  _FLUSH trong ticks — log chỉ thấy GET_DISPLAY_INFO 24B ×2937 —
  debug submitCtrl path/scanoutAttached guard trong tick context
- Approve recipe chuẩn: quit Settings → deep-link ?General → 
  kextload (trigger banner) → zoom button 2 (AppleScript) → 
  Allow @(941,586 hoặc 942,693 tùy window size) → password @(640,455)
  → Modify @(640,503)
- OC picker: click vào vùng trống để lấy keyboard focus rồi 
  sendkey right ×2 (mỗi key riêng) + kp_enter
# M4-NOTES append v0.0.28–v0.0.29 (23 Oct 2026)

## v0.0.28 — mode id 0 + scanout breakthrough
- CoreDisplay asks IOFBSetDisplayModeAndDepth mode 0x0 → displayModeId=0 now accepted.
- SCANOUT VERIFIED: QEMU VNC shows kext diagnostic pattern (screendump==VNC, pixel math matches).
  Earlier "flush not running" was a log-decode error: resp_type=0x1100 IS OK_NODATA (not display-info);
  each 66ms tick emits TRANSFER_TO_HOST_2D + RESOURCE_FLUSH, both OK (ctrl len=24 resp=1100 ×2/tick).
- Pipeline: CREATE_2D → ATTACH_BACKING(fb RAM) → SET_SCANOUT → transfer+flush loop — all working.

## v0.0.29 — AppleParavirtFramebuffer property
- WindowServer classifies driver via registry keys: AppleIntelFramebuffer / AMDFramebuffer /
  AppleParavirtFramebuffer / FB_Triage ("Can't get value for key ..." in log).
- setProperty("AppleParavirtFramebuffer","true") in start() → property visible in ioreg, but
  CoreDisplay still logs "Can't get value" → it reads from a different source (not fb entry props).
- Still offline; commit 031693a.

## Current chain status (vgpu boot)
- FB 1 of 1 created+opened; IOFramebufferUserClient opened by WindowServer (pid 204).
- IODisplayPrefsKey has EDID hash; display0/AppleDisplay complete; DDC path works.
- Blockers: (1) GPUWrangler "accel 0x000000000" — no IOAccelerator → "Unable to find display pipe
  for IOFB service" → CoreDisplay falls to CGXConstructVirtualFramebuffer offline; (2) IOPresentment
  "Unable to acquire map" (also present in v0.0.27 boots — earlier "gone" claim was wrong, that was GUI boot).

## Guest GUI tooling (approval cycles)
- Dialog "System Extension Updated" = process UserNotificationCenter → AX click OK works.
- Password dialog = secure session → only VNC input works: move+click field, vncdo type, key return.
- gclick/gclick2 (CGEvent clicker, compiled on guest) work for normal buttons but NOT the secure dialog.
- Allow button located via AX walk by outlier position (x>800): btn pos=914,574 → click via AX depth walk.
- Deep-link 'x-apple.systempreferences:com.apple.preferences.extensions?Security' may land on General —
  verify page per screenshot.
- /tmp/setprop tool: IORegistryEntrySetCFProperty from userland → e00002e2 (kIOReturnUnsupported) —
  needs kext-side setProperty.

## Next
- Fake/comp IOAccelerator on the PCI device (GPUWrangler needs accel for DisplayPipe), or make
  WindowServer read the paravirt key (find actual source — maybe IODT /provider props or IOFB attribute).
- Alternative route: change virtio-gpu device-id to 0x106b:0x1a0b (AppleVirtIOPCITransport match) —
  but AppleM2ScalerParavirtDriver requires AppleARMPlatform (x86 dead end).

## v0.0.30 — getVRAMRange returns fbBuf — acquire map PERSISTS
- Returned fbBuf (IOBufferMemoryDescriptor) directly as VRAM aperture → still "Unable to acquire map".
- IOPresentment.framework on disk is EMPTY (Resources+CodeSignature only) — code lives in dyld shared cache; can't strings it directly.
- 'IOSurfaceRootParavirtMapperInterface' instance count = 0 → IOPresentment's surface mapper missing.
  AppleParavirtIOSurface.kext provides it but requires AppleARMIODevice 'paravirtualizedgraphics,iosurface' — ARM-only. Dead end on x86.
- Commit f341bf8.

## GUI-boot vs vgpu-boot divergence (key insight)
- GUI boot (vmware-svga + IONDRV fw framebuffer): NO 'Creating FB'/'acquire map' lines — CoreDisplay uses
  a legacy path for IONDRV fbs, no IOPresentment pipeline.
- vgpu boot (our kext): CoreDisplay runs the modern IOPresentment path, which needs the paravirt surface
  mapper (absent on x86) → virtual framebuffer fallback.
- Reconciliation: to get online on vgpu boot we must either (a) make CoreDisplay treat our fb as a
  legacy/IONDRV-class device (e.g. present an NDRV-like interface or IOFramebuffer with the right
  probe properties), or (b) satisfy IOPresentment's mapping handshake some other way.

## Next
- Study how CoreDisplay picks legacy vs IOPresentment path (hint: IONDRV check via IOKit registry
  'IONDRVFramebuffer' class or IOFB attributes like kIOFBVendorKey / 'NDrv' presence).
- Consider writing a minimal NDRV shim: IOFramebuffer subclass exposing getInterfaceTable for
  IOFramebufferInstanceData / 'nub' with kIONDRVModuleKey.
