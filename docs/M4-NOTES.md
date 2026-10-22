
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

## v0.0.31–v0.0.34 (IOAccelerator nub chain)
- v0.0.31–33: IOService nub "IOAccelerator" attach fb/pci + props (IOAccelIndex/Revision/Types) — GPUWrangler `accel 0x000000000` vẫn 0 → lookup là CLASS-based, plain IOService nubs bị bỏ qua.
- v0.0.34 BREAKTHROUGH: OSMetaClass::allocClassWithName("IOAccelerator") — instantiate class thật từ IOAcceleratorFamily2 (no header needed!) + attach PCI + registerService → GPUWrangler log `accel 0x1000002f7 /IOAccelerator` ✓✓. "Unable to find display pipe for IOFB service" GONE. IOAccelerationUserClient được mở (instance 1 — WindowServer).
- Chain giờ: wrangler sees accel ✓, FB created ✓, display-pipe error gone ✓. Remaining: (IOPresentment) "plist invalid for pluginType / no factories for plugin" → "Unable to get AGDC information 0x10000003 (kIOReturnNoResources)" → "Unable to locate service dependencies" → "Unable to acquire map". AGDC plugin = AppleGraphicsDeviceControlPlugin (id 0x1000002f6) nằm CON của AppleGPUWrangler (chung tree), nhưng wrangler `agdc 0x000000000` per-GPU vẫn 0 — cần AppleGraphicsDeviceControl gắn trên GPU device (như IOGraphicsDevice nub) — next: tạo IOGraphicsDevice nub con PCI để AGDC plugin attach.
- Commit 9033a51 (v0.0.31–33 notes inline).

## v0.0.35-37 (AGDC nub + panic recovery)
- v0.0.35: allocClassWithName("AppleGraphicsDeviceControl") tai start() -> NULL (kext chua load luc ta start). v0.0.36 retry-timer: getWorkLoop()=NULL luc start -> PANIC type-14 page fault (VirtIOGPUDisplay::start +0x230) — boot vgpu chet ngay, chi boot -s song.
- RECOVERY RECIPE (quan trong!): boot -s (them -s vao boot-args trong esp/EFI/OC/config.plist + rebuild-esp.sh) -> single-user shell. Console INPUT = HMP sendkey (QEMU key names: spc=space, shift-v=V, shift-minus=_, dot=., slash=/) — vncdo type KHONG chay newline dung. mount Data: mount_apfs /dev/disk1s1 /System/Volumes/Data (s1 = "Untitled - Data"! s2=Preboot, s3=Recovery, s4=System, s5=Update role 192, s6=VM). mv kext ra khoi Data/Library/Extensions -> nvram boot-args=-v -> reboot -> GUI boot (kext sach) -> SSH -> build v0.0.37 (static thunk agdcRetryTickThunk thay OSMemberFunctionCast) -> install + approve + vgpu boot.
- v0.0.37: timer setup bo qua (workloop NULL, guard IF) — khong panic; AGDC nub van chua attach (log "no workloop for AGDC timer"). GPUWrangler van agdc 0x000000000, accel OK 0x1000002f3.
- AGDC errors IOPresentment: "Unable to get AGDC information 0x10000003" — kIOReturnNoResources. AppleGraphicsDeviceControl ton tai trong kernelcache (kc.bin grep).
- qemu pkill pattern 'qemu-system-x86_64-vgpu' MISS vi cmdline QEMU that = 'qemu-system-x86_64' — dung pkill -f qemu-system-x86.

## macOS GPU VM lab — update 2026-10-23 (v0.0.38-39)
- v0.0.38: IOWorkLoop::workLoop() global thay getWorkLoop() (NULL luc start) — retry timer chay. 
- v0.0.39: allocClassWithName("AppleGraphicsDeviceControlPlugin"→NULL, "AppleGraphicsDeviceControl" THÀNH CÔNG sau delay (kext load sau) → "AGDC nub attached to PCI (retry 0)" 05:54:41.
- NHƯNG GPUWrangler van 'agdc 0x000000000' (nub khong duoc nhat qua per-GPU lookup) + IOPresentment van 'Unable to get AGDC information 0x10000003' → 'Unable to acquire map'.
- KET LUAN MOI: AGDC information = du lieu GPU that (AGDC protocol GPU-phia) — tren VM khong GPU that, KHONG BAO GIO co → IOPresentment path = DEAD END tren VM. Con duong: NDRV legacy path (fb classify IONDRV nhu vmware-svga GUI boot da chung hoat dong).
- Approve cycle van phai lam moi version; boot-args luc nao cung kiem tra config.plist co -s hay khong (da revert -v).
- OC picker: khoang 60s sau khi boot; sau do neu serial chi co 'BdsDxe loading Boot0002' = picker chua nhan key → gui lai rightx2+enter; xac nhan bang screenshot truoc khi enter.
- qemu pkill: dung 'pkill -f qemu-system-x86' (cmdline khong co -vgpu suffix).
- SSH guest co the up ~90s sau boot (khac nhau).

## 2026-10-23 buổi sáng — pivot + hybrid findings
- Confirm: vmware-svga boot = desktop FULL (legacy .Display_boot IONDRVFramebuffer trên PCI vmware, có display0). Legacy path SỐNG trên Tahoe x86.
- Hybrid boot (vgpu qemu + vmware-svga + virtio-gpu): desktop FULL + kext fb loaded NHƯNG kext fb vẫn offline (IOPresentment modern path — system_profiler không thấy kext display).
- vgpu2 (virtio class VGA thay OTHERS): OVMF mất console (Display output is not active qua VNC, serial vẫn boot). IONDRVSupport 'display' personality tạo IONDRVFramebuffer trên virtio NHƯNG KHÔNG có display0 (không phải boot-fb pseudo-NDRV; .Display_boot KHÔNG được tạo vì GOP surface không đăng ký). → class VGA là ngõ cụt.
- Cơ chế: .Display_boot (có display0) chỉ được tạo khi OVMF GOP boot console nằm trên device đó (vmware vgabios). Virtio GOP của OVMF không tạo boot-fb pseudo-NDRV.
- Bài toán còn lại: chuyển desktop render sang fb kext. Các hướng: (a) NDRV shim thật cho kext fb; (b) ép CoreDisplay VirtDisplay online (VFBHWFillRegion fbAddress) trỏ fb kext; (c) hybrid hiện tại: desktop trên vmware fb + kext fb thứ 2 offline — VNC vẫn desktop (không cần kext scanout) — NHƯNG đó KHÔNG phải 'giả lập mac hoàn hảo' qua virtio.
- qemu v2 binary: qemu-system-x86_64-vgpu2 (class DISPLAY_VGA) — KHÔNG dùng được cho boot-fb virtio. Giữ vgpu (class OTHERS) cho kext.
- Scripts mới: boot-tahoe-vmware.sh (desktop-only test), boot-tahoe-hybrid.sh (vmware+virtio), boot-tahoe-vgpu2.sh (class VGA — dead).
